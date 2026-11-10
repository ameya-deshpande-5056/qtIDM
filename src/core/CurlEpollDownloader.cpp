#include "core/CurlEpollDownloader.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QMutexLocker>
#include <QStorageInfo>
#include <QUrl>
#include <curl/curl.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <utility>

namespace qtidm {

struct CurlEpollDownloader::DownloadBatch {
    CurlEpollDownloader* owner = nullptr;
    QString id;
    DownloadRequest request;
    QString stagingPath;
    std::shared_ptr<SparseFileWriter> writer;
    QVector<SegmentInfo> segments;
    qint64 total = -1;
    qint64 lastReceived = 0;
    qint64 lastTick = 0;
    int activeSegments = 0;
    int connectionLimit = 1;
    qint64 lastCheckpointMs = 0;
    bool failed = false;
    bool quotaExceeded = false;
    bool failureReported = false;
    bool rangeFallbackAttempted = false;
    DownloadStatus reportedStatus = DownloadStatus::Downloading;
};

struct CurlEpollDownloader::SegmentTransfer {
    std::shared_ptr<DownloadBatch> batch;
    int segmentIndex = 0;
    qint64 start = 0;
    qint64 end = 0;
    qint64 written = 0;
    qint64 initialWritten = 0;
    qint64 requestedRangeStart = -1;
    qint64 requestedRangeEnd = -1;
    long responseCode = 0;
    qint64 responseContentLength = -1;
    qint64 contentRangeStart = -1;
    qint64 contentRangeEnd = -1;
    qint64 contentRangeTotal = -1;
    QByteArray urlBytes;
    QByteArray rangeBytes;
    QByteArray userPwdBytes;
    QByteArray proxyBytes;
    QByteArray requestBody;
    QByteArray caCertBytes;
    curl_slist* headers = nullptr;
    curl_slist* resolveList = nullptr;
    bool requiresHttpPartialResponse = false;
    bool forceHttp1 = false;
    int attempt = 0;
    qint64 retryAfterMs = 0;
    qint64 appliedSpeedLimit = -1;
    QString responseEntityTag;
    QString responseLastModified;
    QString responseLocation;
    QString validationError;
    qint64 startTimeMs = 0;
    qint64 lastProgressTimeMs = 0;

    ~SegmentTransfer()
    {
        curl_slist_free_all(headers);
        curl_slist_free_all(resolveList);
    }
};

namespace {
QString makeId(const QUrl& url, const QString& target)
{
    const auto seed = url.toString() + QLatin1Char('|') + target + QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    return QString::fromLatin1(QCryptographicHash::hash(seed.toUtf8(), QCryptographicHash::Sha256).toHex().left(16));
}

int epollEventsFromCurl(int action)
{
    int events = 0;
    if (action & CURL_POLL_IN) {
        events |= EPOLLIN;
    }
    if (action & CURL_POLL_OUT) {
        events |= EPOLLOUT;
    }
    return events;
}

qint64 batchReceived(const CurlEpollDownloader::DownloadBatch& batch)
{
    qint64 received = 0;
    for (const auto& segment : batch.segments) {
        received += segment.written;
    }
    return received;
}

QString stagingPathFor(const QString& targetPath)
{
    return targetPath + QStringLiteral(".part");
}

QString hostKey(const QUrl& url)
{
    const auto port = url.port();
    return url.scheme().toLower() + QStringLiteral("://") + url.host().toLower()
        + (port >= 0 ? QStringLiteral(":%1").arg(port) : QString {});
}

bool pathOrStagingExists(const QString& targetPath)
{
    return QFileInfo::exists(targetPath) || QFileInfo::exists(stagingPathFor(targetPath));
}

QString availableTargetPath(const QString& requestedPath)
{
    if (!pathOrStagingExists(requestedPath)) {
        return requestedPath;
    }
    const QFileInfo info(requestedPath);
    const auto suffix = info.suffix();
    const auto baseName = info.completeBaseName();
    for (int copy = 1; copy < 100000; ++copy) {
        const auto fileName = suffix.isEmpty()
            ? QStringLiteral("%1 (%2)").arg(baseName).arg(copy)
            : QStringLiteral("%1 (%2).%3").arg(baseName).arg(copy).arg(suffix);
        const auto candidate = info.dir().filePath(fileName);
        if (!pathOrStagingExists(candidate)) {
            return candidate;
        }
    }
    return {};
}

size_t discardCallback(char*, size_t size, size_t nmemb, void*)
{
    return size * nmemb;
}

size_t rangeProbeWriteCallback(char*, size_t size, size_t nmemb, void* userdata)
{
    const auto bytes = size * nmemb;
    auto* transfer = static_cast<CurlEpollDownloader::SegmentTransfer*>(userdata);
    return transfer->responseCode == 206 && transfer->contentRangeStart == 0
            && transfer->contentRangeEnd == 0 && bytes <= 1
        ? bytes : 0;
}

void resetResponseMetadata(CurlEpollDownloader::SegmentTransfer* transfer)
{
    transfer->responseContentLength = -1;
    transfer->contentRangeStart = -1;
    transfer->contentRangeEnd = -1;
    transfer->contentRangeTotal = -1;
    transfer->responseEntityTag.clear();
    transfer->responseLastModified.clear();
    transfer->responseLocation.clear();
}

void parseContentRange(const QByteArray& rawValue, CurlEpollDownloader::SegmentTransfer* transfer)
{
    auto value = rawValue.trimmed();
    if (value.left(6).compare("bytes ", Qt::CaseInsensitive) != 0) {
        return;
    }
    value = value.mid(6).trimmed();
    const auto slash = value.indexOf('/');
    if (slash < 0) {
        return;
    }
    const auto range = value.left(slash);
    const auto total = value.mid(slash + 1).trimmed();
    bool totalOk = false;
    const auto parsedTotal = total == "*" ? qint64(-1) : total.toLongLong(&totalOk);
    if (total != "*" && (!totalOk || parsedTotal < 0)) {
        return;
    }
    if (range == "*") {
        transfer->contentRangeTotal = parsedTotal;
        return;
    }
    const auto dash = range.indexOf('-');
    if (dash < 0) {
        return;
    }
    bool startOk = false;
    bool endOk = false;
    const auto start = range.left(dash).trimmed().toLongLong(&startOk);
    const auto end = range.mid(dash + 1).trimmed().toLongLong(&endOk);
    if (!startOk || !endOk || start < 0 || end < start) {
        return;
    }
    transfer->contentRangeStart = start;
    transfer->contentRangeEnd = end;
    transfer->contentRangeTotal = parsedTotal;
}

size_t headerCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    const auto bytes = size * nmemb;
    auto* transfer = static_cast<CurlEpollDownloader::SegmentTransfer*>(userdata);
    const QByteArray line(ptr, static_cast<qsizetype>(bytes));
    if (line.left(5).compare("HTTP/", Qt::CaseInsensitive) == 0) {
        resetResponseMetadata(transfer);
        const auto parts = line.simplified().split(' ');
        bool ok = false;
        const auto code = parts.size() > 1 ? parts[1].toLong(&ok) : 0;
        transfer->responseCode = ok ? code : 0;
    } else if (line.left(15).compare("Content-Length:", Qt::CaseInsensitive) == 0) {
        bool ok = false;
        const auto length = line.mid(15).trimmed().toLongLong(&ok);
        if (ok && length >= 0) {
            transfer->responseContentLength = length;
        }
    } else if (line.left(14).compare("Content-Range:", Qt::CaseInsensitive) == 0) {
        parseContentRange(line.mid(14), transfer);
    } else if (line.left(5).compare("ETag:", Qt::CaseInsensitive) == 0) {
        transfer->responseEntityTag = QString::fromLatin1(line.mid(5).trimmed());
    } else if (line.left(14).compare("Last-Modified:", Qt::CaseInsensitive) == 0) {
        transfer->responseLastModified = QString::fromLatin1(line.mid(14).trimmed());
    } else if (line.left(9).compare("Location:", Qt::CaseInsensitive) == 0) {
        transfer->responseLocation = QString::fromUtf8(line.mid(9).trimmed());
    } else if (line.left(12).compare("Retry-After:", Qt::CaseInsensitive) == 0) {
        bool ok = false;
        const auto seconds = line.mid(12).trimmed().toLongLong(&ok);
        if (ok && seconds >= 0) {
            transfer->retryAfterMs = qMin<qint64>(seconds * 1000, 300000);
        }
    }
    return bytes;
}

bool isRetryable(CURLcode result, long response)
{
    if (response == 408 || response == 425 || response == 429 || (response >= 500 && response <= 599)) {
        return true;
    }
    switch (result) {
    case CURLE_COULDNT_RESOLVE_PROXY:
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_CONNECT:
    case CURLE_OPERATION_TIMEDOUT:
    case CURLE_PARTIAL_FILE:
    case CURLE_SEND_ERROR:
    case CURLE_RECV_ERROR:
    case CURLE_HTTP2:
    case CURLE_HTTP2_STREAM:
        return true;
    default:
        return false;
    }
}

void extractCookies(CURL* easy, QStringList* serverCookies)
{
    if (!easy || !serverCookies) {
        return;
    }
    curl_slist* cookies = nullptr;
    if (curl_easy_getinfo(easy, CURLINFO_COOKIELIST, &cookies) == CURLE_OK && cookies) {
        for (auto* item = cookies; item; item = item->next) {
            const auto line = QString::fromUtf8(item->data);
            if (!serverCookies->contains(line)) {
                serverCookies->append(line);
            }
        }
        curl_slist_free_all(cookies);
    }
}

void applyRequestOptions(CURL* easy, const DownloadRequest& request, CurlEpollDownloader::SegmentTransfer* transfer)
{
    transfer->urlBytes = request.url.toString(QUrl::FullyEncoded).toUtf8();
    curl_easy_setopt(easy, CURLOPT_URL, transfer->urlBytes.constData());
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "http,https,ftp");
    curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(easy, CURLOPT_MAXREDIRS, static_cast<long>(request.maximumRedirects));
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, static_cast<long>(qBound(1, request.connectTimeoutSeconds, 3600)));
    curl_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(easy, CURLOPT_LOW_SPEED_TIME, static_cast<long>(qBound(1, request.lowSpeedTimeoutSeconds, 3600)));
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(easy, CURLOPT_FTP_RESPONSE_TIMEOUT, 30L);
    curl_easy_setopt(easy, CURLOPT_USE_SSL, CURLUSESSL_TRY);
    curl_easy_setopt(easy, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, headerCallback);
    curl_easy_setopt(easy, CURLOPT_HEADERDATA, transfer);

    // RFC 6265 & authentication scoping (Phase B)
    curl_easy_setopt(easy, CURLOPT_COOKIEFILE, "");
    curl_easy_setopt(easy, CURLOPT_UNRESTRICTED_AUTH, 0L);

    // Re-apply dynamically received server cookies
    for (const auto& cookieLine : request.serverCookies) {
        curl_easy_setopt(easy, CURLOPT_COOKIELIST, cookieLine.toUtf8().constData());
    }

    // RFC 6265 Structured Cookie Injection
    const bool isSecureScheme = request.url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0;
    for (const auto& cookie : request.cookies) {
        if (cookie.secure && !isSecureScheme) {
            continue;
        }
        const auto line = CurlEpollDownloader::toNetscapeCookieLine(cookie);
        curl_easy_setopt(easy, CURLOPT_COOKIELIST, line.toUtf8().constData());
    }

    // HTTP/2 Multiplexing & HTTP/1.1 fallback (Phase C)
    if (transfer && transfer->forceHttp1) {
        curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    } else {
        curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2_0);
        // Segmented downloads intentionally open parallel TCP connections rather than
        // stalling behind a single HTTP/2 connection via CURLOPT_PIPEWAIT.
        const long pipewait = (transfer && transfer->batch && transfer->batch->segments.size() > 1) ? 0L : 1L;
        curl_easy_setopt(easy, CURLOPT_PIPEWAIT, pipewait);
    }

    if (!request.pinnedIp.isEmpty() && transfer) {
        const int port = request.url.port(isSecureScheme ? 443 : 80);
        QStringList formattedIps;
        const auto rawIps = request.pinnedIp.split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (const auto& ip : rawIps) {
            const auto trimmed = ip.trimmed();
            if (trimmed.contains(QLatin1Char(':')) && !trimmed.startsWith(QLatin1Char('['))) {
                formattedIps.append(QStringLiteral("[%1]").arg(trimmed));
            } else {
                formattedIps.append(trimmed);
            }
        }
        if (!formattedIps.isEmpty()) {
            const auto entry = QStringLiteral("%1:%2:%3").arg(request.url.host()).arg(port).arg(formattedIps.join(QLatin1Char(','))).toUtf8();
            transfer->resolveList = curl_slist_append(transfer->resolveList, entry.constData());
            curl_easy_setopt(easy, CURLOPT_RESOLVE, transfer->resolveList);
        }
    }
    if (!request.caCertificatePath.isEmpty() && transfer) {
        transfer->caCertBytes = request.caCertificatePath.toUtf8();
        curl_easy_setopt(easy, CURLOPT_CAINFO, transfer->caCertBytes.constData());
    }
    if (request.insecureSkipSslVerification) {
        curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 0L);
    }

    if (!request.username.isEmpty() || !request.password.isEmpty()) {
        transfer->userPwdBytes = (request.username + QLatin1Char(':') + request.password).toUtf8();
        curl_easy_setopt(easy, CURLOPT_USERPWD, transfer->userPwdBytes.constData());
        curl_easy_setopt(easy, CURLOPT_HTTPAUTH, CURLAUTH_ANY);
    }
    if (!request.proxyUrl.isEmpty()) {
        transfer->proxyBytes = request.proxyUrl.toUtf8();
        curl_easy_setopt(easy, CURLOPT_PROXY, transfer->proxyBytes.constData());
    }
    if (request.httpMethod == QStringLiteral("POST")) {
        transfer->requestBody = request.requestBody;
        curl_easy_setopt(easy, CURLOPT_POST, 1L);
        curl_easy_setopt(easy, CURLOPT_POSTFIELDS, transfer->requestBody.constData());
        curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(transfer->requestBody.size()));
    }
    for (auto it = request.headers.cbegin(); it != request.headers.cend(); ++it) {
        const auto key = it.key();
        if (key.startsWith(QLatin1Char('_'))) {
            continue;
        }
        if (key.compare(QStringLiteral("Cookie"), Qt::CaseInsensitive) == 0) {
            // If request.cookies is empty, parse fallback raw cookies as host-only records
            if (request.cookies.isEmpty()) {
                const auto cookieHeader = it.value().toString();
                const auto pairs = cookieHeader.split(';', Qt::SkipEmptyParts);
                const auto host = request.url.host();
                const bool isSecure = request.url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0;
                for (const auto& pair : pairs) {
                    const auto trimmed = pair.trimmed();
                    const int eq = trimmed.indexOf('=');
                    if (eq > 0) {
                        CookieRecord fallback;
                        fallback.name = trimmed.left(eq).trimmed();
                        fallback.value = trimmed.mid(eq + 1).trimmed();
                        fallback.domain = host;
                        fallback.hostOnly = true;
                        fallback.path = QStringLiteral("/");
                        fallback.secure = isSecure;
                        fallback.session = true;
                        fallback.expirationDate = 0;
                        const auto line = CurlEpollDownloader::toNetscapeCookieLine(fallback);
                        curl_easy_setopt(easy, CURLOPT_COOKIELIST, line.toUtf8().constData());
                    }
                }
            }
            continue;
        }
        const auto line = key.toUtf8() + ": " + it.value().toString().toUtf8();
        transfer->headers = curl_slist_append(transfer->headers, line.constData());
    }
    if (transfer->headers) {
        curl_easy_setopt(easy, CURLOPT_HTTPHEADER, transfer->headers);
    }
}

bool normalizeResumeSegments(QVector<SegmentInfo>* segments, qint64 total, QString* error)
{
    if (segments->isEmpty()) {
        return true;
    }
    for (auto& segment : *segments) {
        if (segment.durableWritten >= 0) {
            segment.written = qMin(segment.written, segment.durableWritten);
        }
    }
    if (total <= 0) {
        auto& segment = (*segments)[0];
        if (segments->size() != 1 || segment.index != 0 || segment.start != 0
            || segment.end != -1 || segment.written < 0) {
            *error = QStringLiteral("saved unknown-length resume state is invalid");
            return false;
        }
        segment.status = DownloadStatus::Queued;
        return true;
    }

    qint64 nextStart = 0;
    for (int index = 0; index < segments->size(); ++index) {
        auto& segment = (*segments)[index];
        if (segments->size() == 1 && segment.start == 0 && segment.end == -1) {
            segment.end = total - 1;
        }
        if (segment.index != index || segment.start != nextStart || segment.end < segment.start
            || segment.end >= total) {
            *error = QStringLiteral("saved segment boundaries are invalid");
            return false;
        }
        const auto length = segment.end - segment.start + 1;
        if (segment.written < 0 || segment.written > length) {
            *error = QStringLiteral("saved segment progress exceeds its range");
            return false;
        }
        segment.status = segment.written == length
            ? DownloadStatus::Completed : DownloadStatus::Queued;
        nextStart = segment.end + 1;
    }
    if (nextStart != total) {
        *error = QStringLiteral("saved segments do not cover the expected file");
        return false;
    }
    return true;
}

bool isPrivateIpv4(uint32_t ip)
{
    // 0.0.0.0/8 (Current network)
    if ((ip >> 24) == 0) return true;
    // 127.0.0.0/8 (Loopback)
    if ((ip >> 24) == 127) return true;
    // 10.0.0.0/8 (Private)
    if ((ip >> 24) == 10) return true;
    // 172.16.0.0/12 (Private: 172.16.0.0 - 172.31.255.255)
    if ((ip & 0xFFF00000) == 0xAC100000) return true;
    // 192.168.0.0/16 (Private)
    if ((ip & 0xFFFF0000) == 0xC0A80000) return true;
    // 169.254.0.0/16 (Link-local, including 169.254.169.254 metadata endpoint)
    if ((ip & 0xFFFF0000) == 0xA9FE0000) return true;
    // 100.64.0.0/10 (Carrier-grade NAT, including 100.100.100.200 Alibaba metadata)
    if ((ip & 0xFFC00000) == 0x64400000) return true;
    // 192.0.0.0/24 (IETF protocol assignments)
    if ((ip & 0xFFFFFF00) == 0xC0000000) return true;
    // 198.18.0.0/15 (Benchmarking)
    if ((ip & 0xFFFE0000) == 0xC6120000) return true;
    // 224.0.0.0/4 (Multicast) & 240.0.0.0/4 (Reserved)
    if ((ip >> 28) >= 14) return true;
    // 255.255.255.255 (Broadcast)
    if (ip == 0xFFFFFFFF) return true;
    return false;
}

bool isPrivateIpv6(const struct in6_addr& in6)
{
    // ::1 (Loopback)
    static const unsigned char loopback[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1};
    if (std::memcmp(in6.s6_addr, loopback, 16) == 0) return true;
    // :: (Unspecified)
    static const unsigned char unspec[16] = {0};
    if (std::memcmp(in6.s6_addr, unspec, 16) == 0) return true;
    // fe80::/10 (Link-Local)
    if ((in6.s6_addr[0] == 0xfe) && ((in6.s6_addr[1] & 0xc0) == 0x80)) return true;
    // fc00::/7 (Unique Local Address, covers fc00::, fd00::, and fd00:ec2::254)
    if ((in6.s6_addr[0] & 0xfe) == 0xfc) return true;
    // ff00::/8 (Multicast)
    if (in6.s6_addr[0] == 0xff) return true;
    // IPv4-mapped IPv6 (::ffff:x.x.x.x)
    if (in6.s6_addr[0] == 0 && in6.s6_addr[1] == 0 && in6.s6_addr[2] == 0 && in6.s6_addr[3] == 0
        && in6.s6_addr[4] == 0 && in6.s6_addr[5] == 0 && in6.s6_addr[6] == 0 && in6.s6_addr[7] == 0
        && in6.s6_addr[8] == 0 && in6.s6_addr[9] == 0 && in6.s6_addr[10] == 0xff && in6.s6_addr[11] == 0xff) {
        uint32_t mappedIp = (static_cast<uint32_t>(in6.s6_addr[12]) << 24)
                          | (static_cast<uint32_t>(in6.s6_addr[13]) << 16)
                          | (static_cast<uint32_t>(in6.s6_addr[14]) << 8)
                          | static_cast<uint32_t>(in6.s6_addr[15]);
        return isPrivateIpv4(mappedIp);
    }
    // IPv4-compatible IPv6 (::x.x.x.x)
    if (in6.s6_addr[0] == 0 && in6.s6_addr[1] == 0 && in6.s6_addr[2] == 0 && in6.s6_addr[3] == 0
        && in6.s6_addr[4] == 0 && in6.s6_addr[5] == 0 && in6.s6_addr[6] == 0 && in6.s6_addr[7] == 0
        && in6.s6_addr[8] == 0 && in6.s6_addr[9] == 0 && in6.s6_addr[10] == 0 && in6.s6_addr[11] == 0) {
        uint32_t compatIp = (static_cast<uint32_t>(in6.s6_addr[12]) << 24)
                          | (static_cast<uint32_t>(in6.s6_addr[13]) << 16)
                          | (static_cast<uint32_t>(in6.s6_addr[14]) << 8)
                          | static_cast<uint32_t>(in6.s6_addr[15]);
        if (compatIp != 0 && compatIp != 1) {
            return isPrivateIpv4(compatIp);
        }
    }
    return false;
}
}

bool CurlEpollDownloader::isPrivateOrLoopbackHost(const QString& host, QString* resolvedPublicIp, QStringList* allPublicIps)
{
    if (resolvedPublicIp) {
        resolvedPublicIp->clear();
    }
    if (allPublicIps) {
        allPublicIps->clear();
    }
    QString cleanHost = host.trimmed().toLower();
    if (cleanHost.isEmpty()) {
        return false;
    }
    if (cleanHost.startsWith(QLatin1Char('[')) && cleanHost.endsWith(QLatin1Char(']'))) {
        cleanHost = cleanHost.mid(1, cleanHost.length() - 2);
    }

    // 1. Loopback, private domain suffixes, and cloud metadata hostnames
    if (cleanHost == QStringLiteral("localhost")
        || cleanHost.endsWith(QStringLiteral(".localhost"))
        || cleanHost.endsWith(QStringLiteral(".local"))
        || cleanHost.endsWith(QStringLiteral(".internal"))
        || cleanHost.endsWith(QStringLiteral(".lan"))
        || cleanHost.endsWith(QStringLiteral(".home"))
        || cleanHost.endsWith(QStringLiteral(".corp"))
        || cleanHost == QStringLiteral("metadata.google.internal")
        || cleanHost == QStringLiteral("metadata")
        || cleanHost == QStringLiteral("instance-data")) {
        return true;
    }

    // Single-label hostnames without any dot or colon (e.g. "router", "nas", "intranet", "corp")
    if (!cleanHost.contains(QLatin1Char('.')) && !cleanHost.contains(QLatin1Char(':'))) {
        return true;
    }

    const auto hostBytes = cleanHost.toUtf8();

    // 2. Standard and alternative IPv4 formats (decimal integer, octal, hex)
    struct in_addr in4;
    if (::inet_aton(hostBytes.constData(), &in4) == 1) {
        const uint32_t ip = ntohl(in4.s_addr);
        if (isPrivateIpv4(ip)) {
            return true;
        }
        if (resolvedPublicIp) {
            char buf[INET_ADDRSTRLEN];
            if (::inet_ntop(AF_INET, &in4, buf, sizeof(buf))) {
                *resolvedPublicIp = QString::fromLatin1(buf);
            }
        }
        if (allPublicIps && resolvedPublicIp && !resolvedPublicIp->isEmpty()) {
            allPublicIps->append(*resolvedPublicIp);
        }
        return false;
    }

    // Pure 32-bit integer string representation (e.g. 2130706433 for 127.0.0.1)
    bool isIntOk = false;
    const auto uVal = cleanHost.toULongLong(&isIntOk, 10);
    if (isIntOk && uVal <= 0xFFFFFFFFULL) {
        if (isPrivateIpv4(static_cast<uint32_t>(uVal))) {
            return true;
        }
        if (resolvedPublicIp) {
            struct in_addr tmpAddr;
            tmpAddr.s_addr = htonl(static_cast<uint32_t>(uVal));
            char buf[INET_ADDRSTRLEN];
            if (::inet_ntop(AF_INET, &tmpAddr, buf, sizeof(buf))) {
                *resolvedPublicIp = QString::fromLatin1(buf);
            }
        }
        if (allPublicIps && resolvedPublicIp && !resolvedPublicIp->isEmpty()) {
            allPublicIps->append(*resolvedPublicIp);
        }
        return false;
    }

    // 3. IPv6 literal addresses
    struct in6_addr in6;
    if (::inet_pton(AF_INET6, hostBytes.constData(), &in6) == 1) {
        if (isPrivateIpv6(in6)) {
            return true;
        }
        if (resolvedPublicIp) {
            char buf[INET6_ADDRSTRLEN];
            if (::inet_ntop(AF_INET6, &in6, buf, sizeof(buf))) {
                *resolvedPublicIp = QString::fromLatin1(buf);
            }
        }
        if (allPublicIps && resolvedPublicIp && !resolvedPublicIp->isEmpty()) {
            allPublicIps->append(QStringLiteral("[%1]").arg(*resolvedPublicIp));
        }
        return false;
    }

    // 4. DNS resolution inspection (catches DNS rebinding, private IP resolution, etc.)
    struct addrinfo hints {};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    struct addrinfo* res = nullptr;
    if (::getaddrinfo(hostBytes.constData(), nullptr, &hints, &res) == 0 && res) {
        bool resolvedPrivate = false;
        QString firstPublic;
        QStringList publicList;
        for (auto* cur = res; cur != nullptr; cur = cur->ai_next) {
            if (cur->ai_family == AF_INET) {
                auto* s4 = reinterpret_cast<struct sockaddr_in*>(cur->ai_addr);
                const uint32_t ip = ntohl(s4->sin_addr.s_addr);
                if (isPrivateIpv4(ip)) {
                    resolvedPrivate = true;
                    break;
                }
                char buf[INET_ADDRSTRLEN];
                if (::inet_ntop(AF_INET, &s4->sin_addr, buf, sizeof(buf))) {
                    const auto str = QString::fromLatin1(buf);
                    if (firstPublic.isEmpty()) {
                        firstPublic = str;
                    }
                    if (!publicList.contains(str)) {
                        publicList.append(str);
                    }
                }
            } else if (cur->ai_family == AF_INET6) {
                auto* s6 = reinterpret_cast<struct sockaddr_in6*>(cur->ai_addr);
                if (isPrivateIpv6(s6->sin6_addr)) {
                    resolvedPrivate = true;
                    break;
                }
                char buf[INET6_ADDRSTRLEN];
                if (::inet_ntop(AF_INET6, &s6->sin6_addr, buf, sizeof(buf))) {
                    const auto rawStr = QString::fromLatin1(buf);
                    const auto bracketedStr = QStringLiteral("[%1]").arg(rawStr);
                    if (firstPublic.isEmpty()) {
                        firstPublic = rawStr;
                    }
                    if (!publicList.contains(bracketedStr)) {
                        publicList.append(bracketedStr);
                    }
                }
            }
        }
        ::freeaddrinfo(res);
        if (resolvedPrivate) {
            return true;
        }
        if (resolvedPublicIp && !firstPublic.isEmpty()) {
            *resolvedPublicIp = firstPublic;
        }
        if (allPublicIps && !publicList.isEmpty()) {
            *allPublicIps = publicList;
        }
    }

    return false;
}

bool CurlEpollDownloader::validateRedirectUrl(const QUrl& originalUrl, const QUrl& targetUrl,
                                             bool allowPrivateNetworks, QString* error,
                                             QString* pinnedPublicIp)
{
    if (pinnedPublicIp) {
        pinnedPublicIp->clear();
    }
    if (!targetUrl.isValid()) {
        if (error) *error = QStringLiteral("Redirect target URL is malformed or invalid");
        return false;
    }
    const auto targetScheme = targetUrl.scheme().toLower();
    const auto origScheme = originalUrl.scheme().toLower();

    // 1. Whitelist supported network protocols
    if (origScheme == QStringLiteral("http") || origScheme == QStringLiteral("https")) {
        if (targetScheme != QStringLiteral("http") && targetScheme != QStringLiteral("https")) {
            if (error) *error = QStringLiteral("Disallowed redirect scheme '%1'; only HTTP and HTTPS are permitted")
                                    .arg(targetScheme);
            return false;
        }
    } else if (origScheme == QStringLiteral("ftp")) {
        if (targetScheme != QStringLiteral("ftp")) {
            if (error) *error = QStringLiteral("Disallowed redirect scheme '%1' from FTP; only FTP is permitted")
                                    .arg(targetScheme);
            return false;
        }
    } else {
        if (error) *error = QStringLiteral("Unsupported source scheme '%1' for redirection").arg(origScheme);
        return false;
    }

    // 2. Reject empty hostname
    if (targetUrl.host().trimmed().isEmpty()) {
        if (error) *error = QStringLiteral("Redirect target URL is missing a valid hostname");
        return false;
    }

    // 3. Reject unexpected embedded credentials (RFC 7230 / phishing prevention)
    if (!targetUrl.userName().isEmpty() || !targetUrl.password().isEmpty()) {
        if (error) *error = QStringLiteral("Redirect target URL contains embedded user credentials (RFC 7230 violation)");
        return false;
    }

    // 4. Private network & SSRF protection policy:
    // If the original URL was a public internet host, reject redirects to private/loopback/link-local addresses
    // unless explicitly allowed.
    QString firstPublic;
    QStringList allPublic;
    const bool origIsPrivate = isPrivateOrLoopbackHost(originalUrl.host());
    const bool targetIsPrivate = isPrivateOrLoopbackHost(targetUrl.host(), &firstPublic, &allPublic);
    if (!origIsPrivate && targetIsPrivate && !allowPrivateNetworks) {
        if (error) *error = QStringLiteral("Redirect from public origin '%1' to private/local network address '%2' is blocked for SSRF security")
                                .arg(originalUrl.host(), targetUrl.host());
        return false;
    }
    if (pinnedPublicIp) {
        *pinnedPublicIp = !allPublic.isEmpty() ? allPublic.join(QLatin1Char(',')) : firstPublic;
    }

    return true;
}

QString CurlEpollDownloader::toNetscapeCookieLine(const CookieRecord& cookie)
{
    // Netscape cookie format:
    // [#HttpOnly_]domain \t include_subdomains \t path \t secure \t expiration \t name \t value
    QString domain = cookie.domain.trimmed();
    QString includeSubdomains;
    if (cookie.hostOnly) {
        includeSubdomains = QStringLiteral("FALSE");
        if (domain.startsWith(QLatin1Char('.'))) {
            domain.remove(0, 1);
        }
    } else {
        includeSubdomains = QStringLiteral("TRUE");
        if (!domain.startsWith(QLatin1Char('.'))) {
            domain.prepend(QLatin1Char('.'));
        }
    }
    const QString prefix = cookie.httpOnly ? QStringLiteral("#HttpOnly_") : QString();
    const QString secure = cookie.secure ? QStringLiteral("TRUE") : QStringLiteral("FALSE");
    const QString expiration = cookie.session ? QStringLiteral("0") : QString::number(cookie.expirationDate);
    const QString path = cookie.path.isEmpty() ? QStringLiteral("/") : cookie.path;

    return QStringLiteral("%1%2\t%3\t%4\t%5\t%6\t%7\t%8")
        .arg(prefix, domain, includeSubdomains, path, secure, expiration, cookie.name, cookie.value);
}

bool CurlEpollDownloader::isSafeCrossOriginHeader(const QString& name)
{
    static const QSet<QString> safeHeaders = {
        QStringLiteral("user-agent"),
        QStringLiteral("accept"),
        QStringLiteral("accept-language"),
        QStringLiteral("accept-encoding"),
        QStringLiteral("range"),
        QStringLiteral("if-range"),
        QStringLiteral("if-match"),
        QStringLiteral("if-none-match"),
        QStringLiteral("if-modified-since"),
        QStringLiteral("if-unmodified-since"),
        QStringLiteral("cache-control"),
        QStringLiteral("pragma"),
        QStringLiteral("te")
    };
    const auto lower = name.trimmed().toLower();
    if (safeHeaders.contains(lower)) {
        return true;
    }
    if (lower.startsWith(QStringLiteral("sec-ch-ua"))) {
        return true;
    }
    return false;
}

void CurlEpollDownloader::applyCrossOriginHeaderPolicy(QVariantMap* headers, QString* username, QString* password)
{
    if (username) {
        username->clear();
    }
    if (password) {
        password->clear();
    }
    if (!headers) {
        return;
    }
    for (auto it = headers->begin(); it != headers->end();) {
        if (!isSafeCrossOriginHeader(it.key())) {
            it = headers->erase(it);
        } else {
            ++it;
        }
    }
}

CurlEpollDownloader::CurlEpollDownloader(QObject* parent)
    : QObject(parent)
{
}

CurlEpollDownloader::~CurlEpollDownloader()
{
    stop();
}

void CurlEpollDownloader::start()
{
    if (running_.exchange(true)) {
        return;
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    epollFd_ = epoll_create1(EPOLL_CLOEXEC);
    wakeFd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    epoll_event wakeEvent {};
    wakeEvent.events = EPOLLIN;
    wakeEvent.data.u64 = static_cast<uint64_t>(wakeFd_);
    epoll_ctl(epollFd_, EPOLL_CTL_ADD, wakeFd_, &wakeEvent);

    constexpr int probeWorkerCount = 4;
    probeThreads_.reserve(probeWorkerCount);
    for (int i = 0; i < probeWorkerCount; ++i) {
        probeThreads_.emplace_back([this] { runProbeWorker(); });
    }
    thread_ = std::jthread([this] { run(); });
}

void CurlEpollDownloader::stop()
{
    if (!running_.exchange(false)) {
        return;
    }
    {
        QMutexLocker lock(&pendingMutex_);
        pendingCv_.wakeAll();
    }
    wake();
    for (auto& probeThread : probeThreads_) {
        if (probeThread.joinable()) {
            probeThread.join();
        }
    }
    probeThreads_.clear();
    if (thread_.joinable()) {
        thread_.join();
    }
    if (wakeFd_ >= 0) {
        close(wakeFd_);
        wakeFd_ = -1;
    }
    if (epollFd_ >= 0) {
        close(epollFd_);
        epollFd_ = -1;
    }
    {
        QMutexLocker lock(&pendingMutex_);
        pending_.clear();
        probingIds_.clear();
    }
    {
        QMutexLocker lock(&probedMutex_);
        probed_.clear();
    }
    {
        QMutexLocker lock(&controlMutex_);
        controls_.clear();
        canceledIds_.clear();
    }
    deferredControls_.clear();
    batchOrder_.clear();
    curlTimeoutDeadlineMs_ = -1;
    curl_global_cleanup();
}

QString CurlEpollDownloader::enqueue(DownloadRequest request)
{
    const auto id = request.existingId.isEmpty() ? makeId(request.url, request.targetPath) : request.existingId;
    {
        QMutexLocker lock(&pendingMutex_);
        pending_.enqueue(QueuedRequest { id, std::move(request) });
        pendingCv_.wakeOne();
    }
    wake();
    return id;
}

void CurlEpollDownloader::pause(const QString& id)
{
    QMutexLocker lock(&controlMutex_);
    controls_.insert(id, DownloadStatus::Paused);
    wake();
}

void CurlEpollDownloader::cancel(const QString& id)
{
    QMutexLocker lock(&controlMutex_);
    controls_.insert(id, DownloadStatus::Canceled);
    canceledIds_.insert(id);
    wake();
}

void CurlEpollDownloader::setAlternateSpeedLimit(qint64 bytesPerSecond)
{
    alternateSpeedLimit_.store(qMax<qint64>(0, bytesPerSecond));
    wake();
}

qint64 CurlEpollDownloader::alternateSpeedLimit() const
{
    return alternateSpeedLimit_.load();
}

qint64 CurlEpollDownloader::sessionBytesReceived() const
{
    return sessionBytesReceived_.load();
}

void CurlEpollDownloader::resetSessionBytesReceived()
{
    sessionBytesReceived_.store(0);
}

void CurlEpollDownloader::acknowledgeCheckpoint(const QString& id, const QVector<SegmentInfo>& confirmedSegments)
{
    {
        QMutexLocker lock(&checkpointMutex_);
        pendingAcks_[id] = confirmedSegments;
    }
    wake();
}

void CurlEpollDownloader::setPersistenceHandler(std::function<bool(const QString& id, const QVector<SegmentInfo>& segments)> handler)
{
    persistenceHandler_ = std::move(handler);
}

void CurlEpollDownloader::applyAcknowledgedCheckpoints()
{
    QHash<QString, QVector<SegmentInfo>> acks;
    {
        QMutexLocker lock(&checkpointMutex_);
        if (pendingAcks_.isEmpty()) {
            return;
        }
        acks.swap(pendingAcks_);
    }
    for (auto it = acks.begin(); it != acks.end(); ++it) {
        auto batchIt = batches_.find(it.key());
        if (batchIt != batches_.end()) {
            auto& batch = *batchIt;
            const auto& confirmed = it.value();
            for (const auto& confSeg : confirmed) {
                if (confSeg.index >= 0 && confSeg.index < batch->segments.size()) {
                    auto& localSeg = batch->segments[confSeg.index];
                    localSeg.durableWritten = qMax(localSeg.durableWritten, confSeg.durableWritten);
                }
            }
        }
    }
}

int CurlEpollDownloader::socketCallback(CURL*, curl_socket_t socket, int what, void* userp, void*)
{
    auto* self = static_cast<CurlEpollDownloader*>(userp);
    if (what == CURL_POLL_REMOVE) {
        self->removeSocket(socket);
    } else {
        self->updateSocket(socket, what);
    }
    return 0;
}

int CurlEpollDownloader::timerCallback(CURLM*, long timeoutMs, void* userp)
{
    auto* self = static_cast<CurlEpollDownloader*>(userp);
    if (timeoutMs < 0) {
        self->curlTimeoutDeadlineMs_ = -1;
        return 0;
    }
    self->curlTimeoutDeadlineMs_ = QDateTime::currentMSecsSinceEpoch() + timeoutMs;
    if (timeoutMs == 0) {
        self->wake();
    }
    return 0;
}

size_t CurlEpollDownloader::writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    const auto bytes = static_cast<qint64>(size * nmemb);
    auto* transfer = static_cast<SegmentTransfer*>(userdata);
    auto& batch = *transfer->batch;
    auto& segment = batch.segments[transfer->segmentIndex];

    if (transfer->requiresHttpPartialResponse) {
        if (transfer->responseCode != 206) {
            transfer->validationError = QStringLiteral("server did not honor requested byte range");
            return 0;
        }
        const bool shortRangeOnKnownSize = (transfer->requestedRangeEnd >= 0
            && transfer->contentRangeEnd < transfer->requestedRangeEnd
            && (transfer->contentRangeTotal <= 0 || transfer->requestedRangeEnd < transfer->contentRangeTotal));
        if (transfer->contentRangeStart != transfer->requestedRangeStart
            || transfer->contentRangeEnd < transfer->contentRangeStart
            || (transfer->requestedRangeEnd >= 0
                && transfer->contentRangeEnd > transfer->requestedRangeEnd)
            || shortRangeOnKnownSize) {
            transfer->validationError = QStringLiteral("server returned an invalid Content-Range");
            return 0;
        }
        if (batch.total > 0 && transfer->contentRangeTotal > 0
            && transfer->contentRangeTotal != batch.total) {
            transfer->validationError = QStringLiteral("remote size changed during download");
            return 0;
        }
        const auto responseLength = transfer->contentRangeEnd - transfer->contentRangeStart + 1;
        const auto responseWritten = transfer->written - transfer->initialWritten;
        if (responseWritten > responseLength || bytes > responseLength - responseWritten) {
            transfer->validationError = QStringLiteral("response exceeds its declared Content-Range");
            return 0;
        }
    }

    bool changedMetadata = false;
    if (batch.request.entityTag.isEmpty() && !transfer->responseEntityTag.isEmpty()) {
        batch.request.entityTag = transfer->responseEntityTag;
        changedMetadata = true;
    }
    if (batch.request.lastModified.isEmpty() && !transfer->responseLastModified.isEmpty()) {
        batch.request.lastModified = transfer->responseLastModified;
        changedMetadata = true;
    }
    qint64 discoveredTotal = transfer->contentRangeTotal;
    if (discoveredTotal <= 0 && !transfer->requiresHttpPartialResponse) {
        discoveredTotal = transfer->responseContentLength;
    }
    if (discoveredTotal > 0 && batch.total <= 0) {
        if (!batch.writer->setExpectedSize(discoveredTotal)) {
            transfer->validationError = batch.writer->lastError();
            return 0;
        }
        batch.total = discoveredTotal;
        changedMetadata = true;
        if (batch.segments.size() == 1 && segment.end < segment.start) {
            segment.end = discoveredTotal - 1;
            transfer->end = segment.end;
        }
    }
    if (changedMetadata) {
        emit batch.owner->metadataChanged(batch.id, batch.total,
                                          batch.request.entityTag, batch.request.lastModified);
    }

    if (segment.end >= segment.start) {
        const auto expectedLength = segment.end - segment.start + 1;
        if (transfer->written > expectedLength || bytes > expectedLength - transfer->written) {
            transfer->validationError = QStringLiteral("response exceeds expected segment size");
            return 0;
        }
    }

    const auto currentSessionBytes = batch.owner->sessionBytesReceived_.load();
    if (batch.request.sessionDataLimitBytes > 0
        && (bytes > batch.request.sessionDataLimitBytes - qMin(currentSessionBytes,
                                                               batch.request.sessionDataLimitBytes))) {
        batch.quotaExceeded = true;
        return 0;
    }
    const auto offset = transfer->start + transfer->written;
    if (!batch.writer->writeAt(offset, ptr, bytes)) {
        transfer->validationError = batch.writer->lastError();
        return 0;
    }
    batch.owner->sessionBytesReceived_.fetch_add(bytes);
    transfer->written += bytes;
    transfer->lastProgressTimeMs = QDateTime::currentMSecsSinceEpoch();
    segment.written = transfer->written;
    segment.status = DownloadStatus::Downloading;
    if (bytes > 0 && !batch.failed && batch.reportedStatus != DownloadStatus::Downloading) {
        batch.reportedStatus = DownloadStatus::Downloading;
        emit batch.owner->statusChanged(batch.id, DownloadStatus::Downloading, {});
    }
    return static_cast<size_t>(bytes);
}

int CurlEpollDownloader::progressCallback(void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    return 0;
}

void CurlEpollDownloader::run()
{
    multi_ = curl_multi_init();

    curl_multi_setopt(multi_, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);
    curl_multi_setopt(multi_, CURLMOPT_MAX_CONCURRENT_STREAMS, 2L);
    curl_multi_setopt(multi_, CURLMOPT_SOCKETFUNCTION, &CurlEpollDownloader::socketCallback);
    curl_multi_setopt(multi_, CURLMOPT_SOCKETDATA, this);
    curl_multi_setopt(multi_, CURLMOPT_TIMERFUNCTION, &CurlEpollDownloader::timerCallback);
    curl_multi_setopt(multi_, CURLMOPT_TIMERDATA, this);

    int active = 0;
    while (running_) {
        addProbedTransfers();
        applyControlRequests();
        addDueRetries();
        applyAcknowledgedCheckpoints();
        refillQueuedSegments();
        applyRateLimits();
        applyControlRequests();

        const auto nowBeforeWait = QDateTime::currentMSecsSinceEpoch();
        if (curlTimeoutDeadlineMs_ >= 0 && nowBeforeWait >= curlTimeoutDeadlineMs_) {
            curlTimeoutDeadlineMs_ = -1;
            curl_multi_socket_action(multi_, CURL_SOCKET_TIMEOUT, 0, &active);
            checkCompleted();
            applyAcknowledgedCheckpoints();
            applyControlRequests();
        }

        const auto waitStart = QDateTime::currentMSecsSinceEpoch();
        int waitMs = batches_.isEmpty() ? 1000 : 250;
        if (curlTimeoutDeadlineMs_ >= 0) {
            const qint64 remaining = qMax<qint64>(0, curlTimeoutDeadlineMs_ - waitStart);
            waitMs = qMin<int>(waitMs, static_cast<int>(qMin<qint64>(remaining, 1000)));
        }
        for (const auto& retry : std::as_const(retries_)) {
            if (retry.dueAtMs > waitStart) {
                const qint64 remaining = retry.dueAtMs - waitStart;
                waitMs = qMin<int>(waitMs, static_cast<int>(qMin<qint64>(remaining, 1000)));
            }
        }

        epoll_event events[64] {};
        const int count = epoll_wait(epollFd_, events, 64, waitMs);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (count == 0) {
            const auto now = QDateTime::currentMSecsSinceEpoch();
            if (curlTimeoutDeadlineMs_ >= 0 && now >= curlTimeoutDeadlineMs_) {
                curlTimeoutDeadlineMs_ = -1;
                curl_multi_socket_action(multi_, CURL_SOCKET_TIMEOUT, 0, &active);
                checkCompleted();
                applyAcknowledgedCheckpoints();
                applyControlRequests();
            }
            emitBatchProgress();
            continue;
        }
        for (int i = 0; i < count; ++i) {
            const uint64_t u64 = events[i].data.u64;
            const int fd = static_cast<int>(u64 & 0xFFFFFFFF);
            const uint32_t gen = static_cast<uint32_t>(u64 >> 32);
            if (fd == wakeFd_) {
                eventfd_t value = 0;
                eventfd_read(wakeFd_, &value);
            } else {
                auto genIt = socketGenerations_.find(fd);
                if (genIt == socketGenerations_.end() || genIt->second != gen) {
                    continue; // Drop stale event from closed/recycled socket
                }
                int action = 0;
                if (events[i].events & EPOLLIN) action |= CURL_CSELECT_IN;
                if (events[i].events & EPOLLOUT) action |= CURL_CSELECT_OUT;
                if (events[i].events & (EPOLLERR | EPOLLHUP)) action |= CURL_CSELECT_ERR;
                if (action != 0) {
                    curl_multi_socket_action(multi_, fd, action, &active);
                }
            }
            checkCompleted();
            applyAcknowledgedCheckpoints();
            applyControlRequests();
        }
        const auto nowAfterEvents = QDateTime::currentMSecsSinceEpoch();
        if (curlTimeoutDeadlineMs_ >= 0 && nowAfterEvents >= curlTimeoutDeadlineMs_) {
            curlTimeoutDeadlineMs_ = -1;
            curl_multi_socket_action(multi_, CURL_SOCKET_TIMEOUT, 0, &active);
            checkCompleted();
            applyAcknowledgedCheckpoints();
            applyControlRequests();
        }
        emitBatchProgress();
    }

    for (const auto& [easy, transfer] : transfers_) {
        curl_multi_remove_handle(multi_, easy);
        curl_easy_cleanup(easy);
    }
    transfers_.clear();
    batches_.clear();
    batchOrder_.clear();
    retries_.clear();
    curl_multi_cleanup(multi_);
    multi_ = nullptr;
}

void CurlEpollDownloader::applyRateLimits()
{
    QHash<QString, int> activeByDownload;
    for (const auto& [easy, transfer] : transfers_) {
        Q_UNUSED(easy);
        activeByDownload[transfer->batch->id]++;
    }

    const qint64 alternateLimit = alternateSpeedLimit_.load();
    for (const auto& [easy, transfer] : transfers_) {
        qint64 downloadLimit = transfer->batch->request.speedLimitBytesPerSecond;
        if (alternateLimit > 0) {
            downloadLimit = downloadLimit > 0 ? qMin(downloadLimit, alternateLimit) : alternateLimit;
        }
        const int connections = qMax(1, activeByDownload.value(transfer->batch->id, 1));
        const qint64 perConnectionLimit = downloadLimit > 0
            ? qMax<qint64>(1, downloadLimit / connections)
            : 0;
        if (transfer->appliedSpeedLimit != perConnectionLimit) {
            curl_easy_setopt(easy, CURLOPT_MAX_RECV_SPEED_LARGE,
                             static_cast<curl_off_t>(perConnectionLimit));
            transfer->appliedSpeedLimit = perConnectionLimit;
        }
    }
}

void CurlEpollDownloader::addDueRetries()
{
    if (retries_.isEmpty()) {
        return;
    }
    QHash<const DownloadBatch*, int> runningForBatch;
    QHash<QString, int> runningForHost;
    for (const auto& [easy, transfer] : transfers_) {
        Q_UNUSED(easy);
        runningForBatch[transfer->batch.get()]++;
        runningForHost[hostKey(transfer->batch->request.url)]++;
    }

    const auto now = QDateTime::currentMSecsSinceEpoch();
    for (int index = retries_.size() - 1; index >= 0; --index) {
        const auto& candidate = retries_[index];
        if (!batches_.contains(candidate.batch->id) || candidate.batch->failed) {
            retries_.removeAt(index);
            continue;
        }
        if (candidate.dueAtMs > now) {
            continue;
        }
        const auto host = hostKey(candidate.batch->request.url);
        const int hostLimit = qBound(1, candidate.batch->request.perHostConnectionLimit, 128);
        const int batchRunning = runningForBatch.value(candidate.batch.get(), 0);
        const int hostRunning = runningForHost.value(host, 0);
        if (batchRunning >= candidate.batch->connectionLimit || hostRunning >= hostLimit) {
            continue;
        }
        const auto retry = retries_.takeAt(index);
        addSegmentTransfer(retry.batch, retry.segmentIndex, retry.attempt, retry.forceHttp1);
        runningForBatch[retry.batch.get()] = batchRunning + 1;
        runningForHost[host] = hostRunning + 1;
    }
}

void CurlEpollDownloader::wake()
{
    if (wakeFd_ >= 0) {
        eventfd_write(wakeFd_, 1);
    }
}

bool CurlEpollDownloader::isPendingOrProbing(const QString& id) const
{
    {
        QMutexLocker lock(&pendingMutex_);
        if (probingIds_.contains(id)) {
            return true;
        }
        for (const auto& item : pending_) {
            if (item.id == id) {
                return true;
            }
        }
    }
    {
        QMutexLocker lock(&probedMutex_);
        for (const auto& item : probed_) {
            if (item.queued.id == id) {
                return true;
            }
        }
    }
    return false;
}

void CurlEpollDownloader::runProbeWorker()
{
    while (running_) {
        QueuedRequest queued;
        {
            QMutexLocker lock(&pendingMutex_);
            while (running_ && pending_.isEmpty()) {
                pendingCv_.wait(&pendingMutex_);
            }
            if (!running_) {
                return;
            }
            queued = pending_.dequeue();
            probingIds_.insert(queued.id);
        }

        ProbedRequest probed;
        probed.queued = std::move(queued);

        bool canceled = false;
        {
            QMutexLocker lock(&controlMutex_);
            canceled = canceledIds_.contains(probed.queued.id);
        }

        if (!canceled) {
            const QString host = probed.queued.request.url.host().trimmed();
            if (!host.isEmpty()) {
                const auto hostBytes = host.toUtf8();
                struct in_addr in4;
                struct in6_addr in6;
                const bool isIpLiteral = (::inet_aton(hostBytes.constData(), &in4) == 1)
                                      || (::inet_pton(AF_INET6, hostBytes.constData(), &in6) == 1);
                if (isIpLiteral) {
                    probed.queued.request.pinnedIp.clear();
                    if (!probed.queued.request.allowPrivateNetworkRedirects) {
                        if (host == QStringLiteral("169.254.169.254") || host == QStringLiteral("100.100.100.200")
                            || host == QStringLiteral("[fd00:ec2::254]") || host == QStringLiteral("fd00:ec2::254")) {
                            probed.error = QStringLiteral("destination resolves to a blocked cloud metadata address");
                        }
                    }
                } else {
                    QString validatedPublicIp;
                    QStringList allPublicIps;
                    const bool isPrivateOrBlocked = isPrivateOrLoopbackHost(host, &validatedPublicIp, &allPublicIps);
                    if (!probed.queued.request.allowPrivateNetworkRedirects && isPrivateOrBlocked) {
                        probed.error = QStringLiteral("destination resolves to a private or loopback address");
                    } else if (validatedPublicIp.isEmpty() && !probed.queued.request.allowPrivateNetworkRedirects) {
                        probed.error = QStringLiteral("could not resolve destination host to a public IP address");
                    } else if (!allPublicIps.isEmpty()) {
                        probed.queued.request.pinnedIp = allPublicIps.join(QLatin1Char(','));
                    } else if (!validatedPublicIp.isEmpty()) {
                        probed.queued.request.pinnedIp = validatedPublicIp;
                    } else if (!probed.queued.request.allowPrivateNetworkRedirects) {
                        probed.queued.request.pinnedIp.clear();
                    }
                }
            }
            if (probed.error.isEmpty() && probed.queued.request.httpMethod != QStringLiteral("POST")) {
                probed.total = probeSize(probed.queued.request, &probed.rangeSupported,
                                         &probed.probedEntityTag, &probed.probedLastModified,
                                         &probed.error, probed.queued.id);
            }
        }

        const QString probeId = probed.queued.id;
        {
            QMutexLocker lock(&probedMutex_);
            probed_.enqueue(std::move(probed));
        }
        {
            QMutexLocker lock(&pendingMutex_);
            probingIds_.remove(probeId);
        }
        wake();
    }
}

void CurlEpollDownloader::addProbedTransfers()
{
    QQueue<ProbedRequest> local;
    {
        QMutexLocker lock(&probedMutex_);
        std::swap(local, probed_);
    }
    while (!local.isEmpty()) {
        addProbedTransfer(local.dequeue());
    }
}

void CurlEpollDownloader::addProbedTransfer(ProbedRequest probed)
{
    auto queued = std::move(probed.queued);
    if (const auto controlIt = deferredControls_.find(queued.id);
        controlIt != deferredControls_.end() && controlIt.value() == DownloadStatus::Canceled) {
        deferredControls_.erase(controlIt);
        {
            QMutexLocker lock(&controlMutex_);
            canceledIds_.remove(queued.id);
        }
        emit statusChanged(queued.id, DownloadStatus::Canceled, {});
        return;
    }

    if (queued.request.resumeSegments.isEmpty() && pathOrStagingExists(queued.request.targetPath)) {
        if (queued.request.fileConflictPolicy == FileConflictPolicy::Skip) {
            deferredControls_.remove(queued.id);
            emit statusChanged(queued.id, DownloadStatus::Canceled,
                               QStringLiteral("destination already exists; download skipped"));
            return;
        }
        if (queued.request.fileConflictPolicy == FileConflictPolicy::AutoRename) {
            const auto renamed = availableTargetPath(queued.request.targetPath);
            if (renamed.isEmpty()) {
                deferredControls_.remove(queued.id);
                emit statusChanged(queued.id, DownloadStatus::Failed,
                                   QStringLiteral("could not find an available destination filename"));
                return;
            }
            queued.request.targetPath = renamed;
        }
    }
    if (!probed.error.isEmpty()) {
        deferredControls_.remove(queued.id);
        emit statusChanged(queued.id, DownloadStatus::Failed, probed.error);
        return;
    }

    auto total = probed.total;
    const bool rangeSupported = probed.rangeSupported;
    const auto& probedEntityTag = probed.probedEntityTag;
    const auto& probedLastModified = probed.probedLastModified;

    if (queued.request.expectedTotalBytes > 0 && total > 0 && queued.request.expectedTotalBytes != total) {
        deferredControls_.remove(queued.id);
        emit statusChanged(queued.id, DownloadStatus::Failed, QStringLiteral("remote size changed; resume refused"));
        return;
    }
    if (total <= 0 && queued.request.expectedTotalBytes > 0) {
        total = queued.request.expectedTotalBytes;
    }
    const bool hasSavedProgress = std::any_of(
        queued.request.resumeSegments.cbegin(), queued.request.resumeSegments.cend(),
        [](const SegmentInfo& segment) { return segment.written > 0; });
    const bool entityChanged = hasSavedProgress && !queued.request.entityTag.isEmpty()
        && !probedEntityTag.isEmpty() && queued.request.entityTag != probedEntityTag;
    const bool modificationTimeChanged = hasSavedProgress && queued.request.entityTag.isEmpty()
        && !queued.request.lastModified.isEmpty() && !probedLastModified.isEmpty()
        && queued.request.lastModified != probedLastModified;
    if (entityChanged || modificationTimeChanged) {
        deferredControls_.remove(queued.id);
        emit statusChanged(queued.id, DownloadStatus::Failed,
                           QStringLiteral("remote entity changed; resume refused"));
        return;
    }
    if (!probedEntityTag.isEmpty()) {
        queued.request.entityTag = probedEntityTag;
    }
    if (!probedLastModified.isEmpty()) {
        queued.request.lastModified = probedLastModified;
    }
    if (hasSavedProgress && !rangeSupported
        && queued.request.url.scheme().startsWith(QStringLiteral("http"))) {
        deferredControls_.remove(queued.id);
        emit statusChanged(queued.id, DownloadStatus::Failed,
                           QStringLiteral("server does not support safe resume; partial file was preserved"));
        return;
    }
    QString resumeError;
    if (!normalizeResumeSegments(&queued.request.resumeSegments, total, &resumeError)) {
        deferredControls_.remove(queued.id);
        emit statusChanged(queued.id, DownloadStatus::Failed, resumeError);
        return;
    }
    const int requestedSegments = queued.request.httpMethod == QStringLiteral("POST")
        ? 1 : std::clamp(queued.request.segments, 1, 32);
    const bool canResumeSegments = rangeSupported && !queued.request.resumeSegments.isEmpty();
    constexpr qint64 minimumDynamicPartSize = 64 * 1024;
    const int dynamicParts = total > 0
        ? static_cast<int>(qMin<qint64>(requestedSegments * 4LL,
              qMax<qint64>(1, (total + minimumDynamicPartSize - 1) / minimumDynamicPartSize)))
        : requestedSegments;
    const int plannedSegments = queued.request.dynamicSegmentation
        ? qMax(requestedSegments, dynamicParts)
        : requestedSegments;
    const int boundedPlannedSegments = total > 0
        ? static_cast<int>(qMin<qint64>(plannedSegments, total))
        : plannedSegments;
    const int segmentCount = canResumeSegments ? queued.request.resumeSegments.size()
        : (rangeSupported && total > 0 ? boundedPlannedSegments : 1);

    auto batch = std::make_shared<DownloadBatch>();
    batch->owner = this;
    batch->id = queued.id;
    batch->request = queued.request;
    batch->stagingPath = stagingPathFor(queued.request.targetPath);
    batch->writer = std::make_shared<SparseFileWriter>();
    batch->total = total;
    batch->lastTick = QDateTime::currentMSecsSinceEpoch();
    batch->activeSegments = segmentCount;
    batch->connectionLimit = qMin(requestedSegments, segmentCount);
    const bool stagingExists = QFileInfo::exists(batch->stagingPath);
    const bool legacyPartialExists = QFileInfo::exists(queued.request.targetPath);
    if (hasSavedProgress && !stagingExists && !legacyPartialExists) {
        deferredControls_.remove(queued.id);
        emit statusChanged(batch->id, DownloadStatus::Failed,
                           QStringLiteral("saved partial file is missing; resume refused"));
        return;
    }
    if (queued.request.sessionDataLimitBytes > 0) {
        const auto remainingQuota = queued.request.sessionDataLimitBytes - sessionBytesReceived_.load();
        const auto requiredBytes = total > 0 ? qMax<qint64>(0, total - QFileInfo(batch->stagingPath).size()) : 1;
        if (remainingQuota < requiredBytes) {
            deferredControls_.remove(queued.id);
            emit statusChanged(batch->id, DownloadStatus::Failed,
                QStringLiteral("session data limit exceeded: %1 bytes remain, %2 bytes required")
                    .arg(qMax<qint64>(0, remainingQuota)).arg(requiredBytes));
            return;
        }
    }
    if (total > 0) {
        const QStorageInfo storage(QFileInfo(batch->stagingPath).absolutePath());
        const qint64 existingBytes = QFileInfo(batch->stagingPath).size();
        const qint64 additionalBytes = qMax<qint64>(0, total - existingBytes);
        if (storage.isValid() && storage.isReady() && storage.bytesAvailable() < additionalBytes) {
            deferredControls_.remove(queued.id);
            emit statusChanged(batch->id, DownloadStatus::Failed,
                               QStringLiteral("not enough disk space: %1 bytes required, %2 bytes available")
                                   .arg(additionalBytes)
                                   .arg(storage.bytesAvailable()));
            return;
        }
    }
    if (!queued.request.resumeSegments.isEmpty()
        && !QFileInfo::exists(batch->stagingPath)
        && QFileInfo::exists(queued.request.targetPath)
        && !QFile::rename(queued.request.targetPath, batch->stagingPath)) {
        deferredControls_.remove(queued.id);
        emit statusChanged(batch->id, DownloadStatus::Failed,
                           QStringLiteral("could not migrate legacy partial download to %1").arg(batch->stagingPath));
        return;
    }
    if (hasSavedProgress) {
        qint64 requiredExtent = 0;
        for (const auto& segment : std::as_const(queued.request.resumeSegments)) {
            requiredExtent = qMax(requiredExtent, segment.start + segment.written);
        }
        if (QFileInfo(batch->stagingPath).size() < requiredExtent) {
            deferredControls_.remove(queued.id);
            emit statusChanged(batch->id, DownloadStatus::Failed,
                               QStringLiteral("partial file is shorter than saved segment progress"));
            return;
        }
    }
    const bool preserveUnknownPartial = canResumeSegments && total <= 0;
    if (preserveUnknownPartial) {
        QFile partial(batch->stagingPath);
        const auto savedBytes = queued.request.resumeSegments[0].written;
        if (!partial.open(QIODevice::ReadWrite) || !partial.resize(savedBytes)) {
            deferredControls_.remove(queued.id);
            emit statusChanged(batch->id, DownloadStatus::Failed,
                               QStringLiteral("could not align partial file with saved progress"));
            return;
        }
        partial.close();
    }
    if (!batch->writer->open(batch->stagingPath, total, preserveUnknownPartial)) {
        deferredControls_.remove(queued.id);
        emit statusChanged(batch->id, DownloadStatus::Failed, batch->writer->lastError());
        return;
    }

    DownloadRecord record;
    record.id = batch->id;
    record.url = queued.request.url;
    record.targetPath = queued.request.targetPath;
    record.category = queued.request.category;
    record.totalBytes = total;
    record.status = DownloadStatus::Downloading;
    record.createdAt = QDateTime::currentDateTimeUtc();
    record.updatedAt = record.createdAt;
    record.request = queued.request;

    const qint64 segmentSize = total > 0 ? ((total + segmentCount - 1) / segmentCount) : -1;
    for (int index = 0; index < segmentCount; ++index) {
        SegmentInfo segment;
        if (canResumeSegments) {
            segment = queued.request.resumeSegments[index];
        } else {
            segment.index = index;
            segment.start = total > 0 ? index * segmentSize : 0;
            segment.end = total > 0 ? qMin(total - 1, segment.start + segmentSize - 1) : -1;
            segment.status = DownloadStatus::Queued;
        }
        batch->segments.append(segment);
        record.segments.append(segment);
    }

    record.completedBytes = batchReceived(*batch);
    batches_.insert(batch->id, batch);
    batchOrder_.append(batch->id);
    batch->lastReceived = record.completedBytes;
    batch->reportedStatus = DownloadStatus::Downloading;
    emit downloadAdded(record);
    emit statusChanged(batch->id, DownloadStatus::Downloading, {});

    for (const auto& segment : std::as_const(batch->segments)) {
        if (segment.end >= segment.start && segment.written >= segment.end - segment.start + 1) {
            batch->activeSegments--;
        }
    }
    emit segmentsChanged(batch->id, batch->segments);
    if (batch->activeSegments == 0) {
        deferredControls_.remove(batch->id);
        batch->writer->close();
        QString verificationError;
        bool verified = verifyCompletedDownload(batch, &verificationError);
        if (verified) {
            verified = publishCompletedDownload(batch, &verificationError);
        }
        emit progressChanged(batch->id, batchReceived(*batch), batch->total, 0.0);
        emit statusChanged(batch->id, verified ? DownloadStatus::Completed : DownloadStatus::Failed, verificationError);
        batches_.remove(batch->id);
        batchOrder_.removeAll(batch->id);
    }
}

bool CurlEpollDownloader::splitActiveSegment(const std::shared_ptr<DownloadBatch>& batch)
{
    if (!batch || batch->failed || batch->total <= 0 || !batch->request.dynamicSegmentation) {
        return false;
    }
    const auto batchHost = hostKey(batch->request.url);
    const int hostLimit = qBound(1, batch->request.perHostConnectionLimit, 128);
    if (activeConnectionsForHost(batchHost) >= hostLimit) {
        return false;
    }

    constexpr qint64 splitThresholdBytes = 64 * 1024;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

    CURL* candidateEasy = nullptr;
    int candidateIndex = -1;
    qint64 candidateUnread = 0;
    double candidateScore = -1.0;

    for (const auto& [easy, transfer] : transfers_) {
        if (transfer->batch != batch) {
            continue;
        }
        const int idx = transfer->segmentIndex;
        if (idx < 0 || idx >= batch->segments.size()) {
            continue;
        }
        const auto& seg = batch->segments[idx];
        const qint64 nextOffset = seg.start + seg.written;
        if (seg.end < nextOffset) {
            continue;
        }
        const qint64 unread = seg.end - nextOffset + 1;
        if (unread < splitThresholdBytes) {
            continue;
        }
        const qint64 elapsedMs = qMax<qint64>(1, nowMs - transfer->startTimeMs);
        const bool hasResponse = (transfer->responseCode == 206);
        const qint64 minElapsedMs = hasResponse ? 200 : 1000;
        if (elapsedMs < minElapsedMs) {
            continue;
        }
        const qint64 bytesSinceStart = qMax<qint64>(0, transfer->written - transfer->initialWritten);
        const double speedBytesPerSec = static_cast<double>(bytesSinceStart) * 1000.0 / static_cast<double>(elapsedMs);
        const double score = static_cast<double>(unread) / qMax(1024.0, speedBytesPerSec);
        if (score > candidateScore) {
            candidateScore = score;
            candidateEasy = easy;
            candidateIndex = idx;
            candidateUnread = unread;
        }
    }

    if (!candidateEasy || candidateIndex < 0 || candidateUnread < splitThresholdBytes) {
        return false;
    }

    auto& origSeg = batch->segments[candidateIndex];
    const qint64 nextOffset = origSeg.start + origSeg.written;
    const qint64 unread = origSeg.end - nextOffset + 1;
    if (unread < splitThresholdBytes) {
        return false;
    }
    const qint64 originalEnd = origSeg.end;
    const qint64 mid = nextOffset + (unread / 2) - 1;
    if (mid < nextOffset || mid >= originalEnd) {
        return false;
    }

    // 1. Synchronously remove and cleanup candidate transfer
    const bool candidateForceHttp1 = transfers_[candidateEasy]->forceHttp1;
    curl_multi_remove_handle(multi_, candidateEasy);
    curl_easy_cleanup(candidateEasy);
    transfers_.erase(candidateEasy);
    emit hostConnectionCountChanged(batchHost, activeConnectionsForHost(batchHost));

    // Invalidate any delayed retries for the candidate segment
    for (int i = retries_.size() - 1; i >= 0; --i) {
        if (retries_[i].batch == batch && retries_[i].segmentIndex == candidateIndex) {
            retries_.removeAt(i);
        }
    }

    // 2. Synchronize partial file data before partitioning
    if (!batch->writer->sync()) {
        addSegmentTransfer(batch, candidateIndex, 0, candidateForceHttp1);
        return false;
    }

    // 3. In-memory partitioning
    const qint64 lowerLength = mid - origSeg.start + 1;
    origSeg.end = mid;
    origSeg.written = qMin(origSeg.written, lowerLength);
    origSeg.durableWritten = qMin(origSeg.durableWritten, lowerLength);

    SegmentInfo newSeg;
    newSeg.index = candidateIndex + 1;
    newSeg.start = mid + 1;
    newSeg.end = originalEnd;
    newSeg.written = 0;
    newSeg.durableWritten = 0;
    newSeg.status = DownloadStatus::Queued;

    batch->segments.insert(candidateIndex + 1, newSeg);
    batch->activeSegments++;

    for (int i = 0; i < batch->segments.size(); ++i) {
        batch->segments[i].index = i;
    }

    // Synchronously update active transfers and retries for shifted indices
    for (auto& [easy, transfer] : transfers_) {
        if (transfer->batch == batch && transfer->segmentIndex > candidateIndex) {
            transfer->segmentIndex++;
        }
    }
    for (auto& retry : retries_) {
        if (retry.batch == batch && retry.segmentIndex > candidateIndex) {
            retry.segmentIndex++;
        }
    }

    // 4. Persist split layout to database BEFORE replacement transfers start writing!
    if (persistenceHandler_) {
        if (!persistenceHandler_(batch->id, batch->segments)) {
            // Roll back in-memory split if database commit failed
            batch->segments.removeAt(candidateIndex + 1);
            batch->activeSegments--;
            origSeg.end = originalEnd;
            for (int i = 0; i < batch->segments.size(); ++i) {
                batch->segments[i].index = i;
            }
            for (auto& [easy, transfer] : transfers_) {
                if (transfer->batch == batch && transfer->segmentIndex > candidateIndex) {
                    transfer->segmentIndex--;
                }
            }
            for (auto& retry : retries_) {
                if (retry.batch == batch && retry.segmentIndex > candidateIndex) {
                    retry.segmentIndex--;
                }
            }
            addSegmentTransfer(batch, candidateIndex, 0, candidateForceHttp1);
            return false;
        }
    }

    // 5. Dispatch both replacement sub-ranges now that split is persisted
    addSegmentTransfer(batch, candidateIndex, 0, candidateForceHttp1);
    addSegmentTransfer(batch, candidateIndex + 1, 0, candidateForceHttp1);

    emit segmentsChanged(batch->id, batch->segments);
    return true;
}

int CurlEpollDownloader::activeConnectionsForHost(const QString& host) const
{
    return static_cast<int>(std::count_if(transfers_.cbegin(), transfers_.cend(), [&host](const auto& entry) {
        return hostKey(entry.second->batch->request.url) == host;
    }));
}

void CurlEpollDownloader::refillQueuedSegments()
{
    QHash<const DownloadBatch*, int> runningForBatch;
    QHash<QString, int> runningForHost;
    for (const auto& [easy, transfer] : transfers_) {
        Q_UNUSED(easy);
        runningForBatch[transfer->batch.get()]++;
        runningForHost[hostKey(transfer->batch->request.url)]++;
    }

    QSet<QString> changedBatches;
    bool startedAny = true;
    while (startedAny) {
        startedAny = false;
        std::shared_ptr<DownloadBatch> bestBatch;
        int bestRunning = std::numeric_limits<int>::max();
        int bestSegIndex = -1;

        for (const auto& id : std::as_const(batchOrder_)) {
            const auto batch = batches_.value(id);
            if (!batch || batch->failed) {
                continue;
            }
            const int batchRunning = runningForBatch.value(batch.get(), 0);
            if (batchRunning >= batch->connectionLimit) {
                continue;
            }
            const auto host = hostKey(batch->request.url);
            const int hostLimit = qBound(1, batch->request.perHostConnectionLimit, 128);
            if (runningForHost.value(host, 0) >= hostLimit) {
                continue;
            }
            if (batchRunning >= bestRunning) {
                continue;
            }
            int nextSeg = -1;
            for (const auto& segment : std::as_const(batch->segments)) {
                if (segment.status == DownloadStatus::Queued
                    || (segment.status == DownloadStatus::Paused && segment.written < segment.end - segment.start + 1)) {
                    const bool alreadyActive = std::any_of(transfers_.cbegin(), transfers_.cend(),
                        [&batch, &segment](const auto& entry) {
                            return entry.second->batch == batch && entry.second->segmentIndex == segment.index;
                        });
                    if (alreadyActive) {
                        continue;
                    }
                    const bool pendingRetry = std::any_of(retries_.cbegin(), retries_.cend(),
                        [&batch, &segment](const auto& retry) {
                            return retry.batch == batch && retry.segmentIndex == segment.index;
                        });
                    if (pendingRetry) {
                        continue;
                    }
                    nextSeg = segment.index;
                    break;
                }
            }
            if (nextSeg >= 0) {
                bestBatch = batch;
                bestRunning = batchRunning;
                bestSegIndex = nextSeg;
            }
        }

        if (bestBatch && bestSegIndex >= 0) {
            const auto host = hostKey(bestBatch->request.url);
            addSegmentTransfer(bestBatch, bestSegIndex);
            runningForBatch[bestBatch.get()] = bestRunning + 1;
            runningForHost[host] = runningForHost.value(host, 0) + 1;
            changedBatches.insert(bestBatch->id);
            startedAny = true;
        }
    }

    for (const auto& id : changedBatches) {
        if (const auto batch = batches_.value(id)) {
            emit segmentsChanged(batch->id, batch->segments);
        }
    }

    bool splitAny = true;
    while (splitAny) {
        splitAny = false;
        QList<std::pair<int, std::shared_ptr<DownloadBatch>>> candidates;
        for (const auto& id : std::as_const(batchOrder_)) {
            const auto batch = batches_.value(id);
            if (!batch || batch->failed || batch->total <= 0 || !batch->request.dynamicSegmentation) {
                continue;
            }
            const int batchRunning = runningForBatch.value(batch.get(), 0);
            if (batchRunning >= batch->connectionLimit) {
                continue;
            }
            const auto host = hostKey(batch->request.url);
            const int hostLimit = qBound(1, batch->request.perHostConnectionLimit, 128);
            if (runningForHost.value(host, 0) >= hostLimit) {
                continue;
            }
            candidates.append({ batchRunning, batch });
        }
        std::stable_sort(candidates.begin(), candidates.end(),
                         [](const auto& left, const auto& right) { return left.first < right.first; });
        for (const auto& entry : std::as_const(candidates)) {
            if (splitActiveSegment(entry.second)) {
                const auto host = hostKey(entry.second->request.url);
                runningForBatch[entry.second.get()] = entry.first + 1;
                runningForHost[host] = runningForHost.value(host, 0) + 1;
                splitAny = true;
                break;
            }
        }
    }
}

bool CurlEpollDownloader::verifyCompletedDownload(const std::shared_ptr<DownloadBatch>& batch, QString* error) const
{
    if (batch->total < 0) {
        *error = QStringLiteral("download completed without a determinable final size");
        return false;
    }
    qint64 nextStart = 0;
    for (const auto& segment : batch->segments) {
        if (batch->total == 0 && batch->segments.size() == 1
            && segment.start == 0 && segment.end == -1 && segment.written == 0) {
            continue;
        }
        if (segment.start != nextStart || segment.end < segment.start) {
            *error = QStringLiteral("completed segments contain a gap or overlap");
            return false;
        }
        const auto expectedLength = segment.end - segment.start + 1;
        if (segment.written != expectedLength) {
            *error = QStringLiteral("segment %1 is incomplete: expected %2 bytes, wrote %3")
                         .arg(segment.index + 1).arg(expectedLength).arg(segment.written);
            return false;
        }
        nextStart = segment.end + 1;
    }
    if (nextStart != batch->total) {
        *error = QStringLiteral("completed segments do not cover the expected file size");
        return false;
    }
    const auto actualSize = QFileInfo(batch->stagingPath).size();
    if (actualSize != batch->total) {
        *error = QStringLiteral("completed file size mismatch: expected %1 bytes, got %2")
                     .arg(batch->total).arg(actualSize);
        return false;
    }

    const auto expectedText = batch->request.expectedChecksum.isEmpty()
        ? batch->request.expectedSha256
        : batch->request.expectedChecksum;
    if (expectedText.isEmpty()) {
        return true;
    }
    const auto algorithmName = batch->request.expectedChecksum.isEmpty()
        ? QStringLiteral("sha256")
        : batch->request.checksumAlgorithm.trimmed().toLower();
    QCryptographicHash::Algorithm algorithm;
    int expectedLength = 0;
    if (algorithmName == QStringLiteral("md5")) {
        algorithm = QCryptographicHash::Md5;
        expectedLength = 32;
    } else if (algorithmName == QStringLiteral("sha1")) {
        algorithm = QCryptographicHash::Sha1;
        expectedLength = 40;
    } else if (algorithmName == QStringLiteral("sha256")) {
        algorithm = QCryptographicHash::Sha256;
        expectedLength = 64;
    } else if (algorithmName == QStringLiteral("sha512")) {
        algorithm = QCryptographicHash::Sha512;
        expectedLength = 128;
    } else {
        *error = QStringLiteral("unsupported checksum algorithm: %1").arg(algorithmName);
        return false;
    }
    const auto algorithmLabel = algorithmName == QStringLiteral("sha1") ? QStringLiteral("SHA-1")
        : algorithmName == QStringLiteral("sha256") ? QStringLiteral("SHA-256")
        : algorithmName == QStringLiteral("sha512") ? QStringLiteral("SHA-512")
        : QStringLiteral("MD5");
    const auto expected = expectedText.trimmed().toLatin1().toLower();
    if (expected.size() != expectedLength || !std::all_of(expected.cbegin(), expected.cend(), [](char value) {
            return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
        })) {
        *error = QStringLiteral("invalid expected %1 checksum").arg(algorithmLabel);
        return false;
    }
    QFile file(batch->stagingPath);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("could not verify %1: %2").arg(algorithmLabel, file.errorString());
        return false;
    }
    QCryptographicHash hash(algorithm);
    if (!hash.addData(&file)) {
        *error = QStringLiteral("could not read completed file for %1 verification").arg(algorithmLabel);
        return false;
    }
    const auto actual = hash.result().toHex();
    if (actual != expected) {
        *error = QStringLiteral("%1 mismatch: expected %2, got %3")
                     .arg(algorithmLabel, QString::fromLatin1(expected), QString::fromLatin1(actual));
        return false;
    }
    return true;
}

bool CurlEpollDownloader::publishCompletedDownload(const std::shared_ptr<DownloadBatch>& batch, QString* error) const
{
    const auto targetPath = batch->request.targetPath;
    if (QFileInfo::exists(targetPath) && !QFile::remove(targetPath)) {
        *error = QStringLiteral("could not replace existing destination file %1").arg(targetPath);
        return false;
    }
    if (!QFile::rename(batch->stagingPath, targetPath)) {
        *error = QStringLiteral("could not publish completed download from %1 to %2")
                     .arg(batch->stagingPath, targetPath);
        return false;
    }
    return true;
}

void CurlEpollDownloader::addSegmentTransfer(const std::shared_ptr<DownloadBatch>& batch, int segmentIndex, int attempt, bool forceHttp1)
{
    auto& segment = batch->segments[segmentIndex];
    auto transfer = std::make_unique<SegmentTransfer>();
    transfer->batch = batch;
    transfer->segmentIndex = segmentIndex;
    transfer->start = segment.start;
    transfer->end = segment.end;
    transfer->written = segment.written;
    transfer->initialWritten = segment.written;
    transfer->attempt = attempt;
    transfer->forceHttp1 = forceHttp1;
    transfer->startTimeMs = QDateTime::currentMSecsSinceEpoch();
    transfer->lastProgressTimeMs = transfer->startTimeMs;

    auto* easy = curl_easy_init();
    const auto rangeStart = segment.start + segment.written;
    const bool resumed = segment.written > 0;
    if ((segment.end >= rangeStart && (batch->segments.size() > 1 || resumed))
        || (resumed && segment.end < 0)) {
        transfer->requestedRangeStart = rangeStart;
        transfer->requestedRangeEnd = segment.end;
        transfer->rangeBytes = QByteArray::number(rangeStart) + "-"
            + (segment.end >= 0 ? QByteArray::number(segment.end) : QByteArray {});
        transfer->requiresHttpPartialResponse = batch->request.url.scheme().startsWith(QStringLiteral("http"));
    }
    auto transferRequest = batch->request;
    if (resumed && transfer->requiresHttpPartialResponse) {
        const auto validator = !batch->request.entityTag.isEmpty()
                && !batch->request.entityTag.startsWith(QStringLiteral("W/"))
            ? batch->request.entityTag : batch->request.lastModified;
        if (!validator.isEmpty()) {
            transferRequest.headers.insert(QStringLiteral("If-Range"), validator);
        }
    }
    applyRequestOptions(easy, transferRequest, transfer.get());
    if (!transfer->rangeBytes.isEmpty()) {
        curl_easy_setopt(easy, CURLOPT_RANGE, transfer->rangeBytes.constData());
    }
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, &CurlEpollDownloader::writeCallback);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, transfer.get());
    curl_easy_setopt(easy, CURLOPT_XFERINFOFUNCTION, &CurlEpollDownloader::progressCallback);
    curl_easy_setopt(easy, CURLOPT_XFERINFODATA, transfer.get());
    curl_easy_setopt(easy, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(easy, CURLOPT_PRIVATE, easy);
    segment.status = DownloadStatus::Downloading;
    transfers_.emplace(easy, std::move(transfer));
    curl_multi_add_handle(multi_, easy);
    const auto host = hostKey(batch->request.url);
    emit hostConnectionCountChanged(host, activeConnectionsForHost(host));
}

void CurlEpollDownloader::retryTransfer(std::unique_ptr<SegmentTransfer> transfer, long responseCode, CURLcode result)
{
    const auto batch = transfer->batch;
    if (!batch->request.pinnedIp.isEmpty()) {
        auto ips = batch->request.pinnedIp.split(QLatin1Char(','), Qt::SkipEmptyParts);
        if (ips.size() > 1) {
            ips.append(ips.takeFirst());
            batch->request.pinnedIp = ips.join(QLatin1Char(','));
        }
    }
    const int nextAttempt = transfer->attempt + 1;
    const qint64 exponentialDelay = static_cast<qint64>(qMax(100, batch->request.retryBaseDelayMs))
        * (qint64(1) << qMin(transfer->attempt, 9));
    const qint64 delayMs = qMin<qint64>(qMax(exponentialDelay, transfer->retryAfterMs), 300000);
    auto& segment = batch->segments[transfer->segmentIndex];
    segment.written = transfer->written;
    segment.status = DownloadStatus::Queued;
    emit segmentsChanged(batch->id, batch->segments);
    const bool hasActiveTransfers = std::any_of(transfers_.cbegin(), transfers_.cend(),
        [&batch](const auto& entry) { return entry.second->batch == batch; });
    const auto nextStatus = hasActiveTransfers
        ? DownloadStatus::Downloading
        : DownloadStatus::Connecting;
    batch->reportedStatus = nextStatus;
    emit statusChanged(batch->id, nextStatus,
                       QStringLiteral("Segment %1 retry %2/%3 in %4 ms after %5")
                           .arg(transfer->segmentIndex + 1)
                           .arg(nextAttempt)
                           .arg(batch->request.maxRetries)
                           .arg(delayMs)
                           .arg(responseCode >= 400 ? QStringLiteral("HTTP %1").arg(responseCode)
                                                   : QString::fromUtf8(curl_easy_strerror(result))));
    retries_.append({
        QDateTime::currentMSecsSinceEpoch() + delayMs,
        batch,
        transfer->segmentIndex,
        nextAttempt,
        transfer->forceHttp1
    });
}

void CurlEpollDownloader::updateSocket(curl_socket_t socket, int action)
{
    const bool known = sockets_.contains(socket);
    uint32_t gen = 0;
    if (!known) {
        ++currentGeneration_;
        if (currentGeneration_ == 0) {
            currentGeneration_ = 1;
        }
        gen = currentGeneration_;
        socketGenerations_[socket] = gen;
    } else {
        gen = socketGenerations_[socket];
    }
    epoll_event event {};
    event.events = epollEventsFromCurl(action);
    event.data.u64 = (static_cast<uint64_t>(gen) << 32) | static_cast<uint32_t>(socket);
    if (epollFd_ >= 0) {
        epoll_ctl(epollFd_, known ? EPOLL_CTL_MOD : EPOLL_CTL_ADD, socket, &event);
    }
    sockets_[socket] = event.events;
}

void CurlEpollDownloader::removeSocket(curl_socket_t socket)
{
    if (epollFd_ >= 0) {
        epoll_ctl(epollFd_, EPOLL_CTL_DEL, socket, nullptr);
    }
    sockets_.erase(socket);
    socketGenerations_.erase(socket);
}

uint32_t CurlEpollDownloader::socketGeneration(curl_socket_t socket) const
{
    auto it = socketGenerations_.find(socket);
    return it != socketGenerations_.end() ? it->second : 0;
}

uint32_t CurlEpollDownloader::currentGeneration() const
{
    return currentGeneration_;
}

void CurlEpollDownloader::setNextGenerationForTesting(uint32_t nextGen)
{
    currentGeneration_ = nextGen;
}

bool CurlEpollDownloader::isStaleEvent(curl_socket_t socket, uint32_t eventGen) const
{
    auto it = socketGenerations_.find(socket);
    return it == socketGenerations_.end() || it->second != eventGen;
}

qint64 CurlEpollDownloader::probeSize(DownloadRequest& request, bool* rangeSupported,
                                      QString* entityTag, QString* lastModified, QString* error,
                                      const QString& downloadId)
{
    *rangeSupported = false;
    entityTag->clear();
    lastModified->clear();
    qint64 total = -1;

    struct ProbeAbortContext {
        const CurlEpollDownloader* self;
        QString downloadId;
    } abortContext { this, downloadId };

    auto abortCallback = +[](void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
        const auto* ctx = static_cast<const ProbeAbortContext*>(clientp);
        if (!ctx->self->running_.load()) {
            return 1;
        }
        if (!ctx->downloadId.isEmpty()) {
            QMutexLocker lock(const_cast<QMutex*>(&ctx->self->controlMutex_));
            if (ctx->self->canceledIds_.contains(ctx->downloadId)) {
                return 1;
            }
        }
        return 0;
    };

    int redirectsFollowed = 0;
    const int maxRedirects = qMax(0, request.maximumRedirects);

    // 1. HEAD probe (hop-by-hop redirect following with credential isolation)
    while (redirectsFollowed <= maxRedirects) {
        auto transfer = std::make_unique<SegmentTransfer>();
        auto* easy = curl_easy_init();
        applyRequestOptions(easy, request, transfer.get());
        curl_easy_setopt(easy, CURLOPT_NOBODY, 1L);
        curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, discardCallback);
        curl_easy_setopt(easy, CURLOPT_XFERINFOFUNCTION, abortCallback);
        curl_easy_setopt(easy, CURLOPT_XFERINFODATA, &abortContext);
        curl_easy_setopt(easy, CURLOPT_NOPROGRESS, 0L);
        auto rc = curl_easy_perform(easy);
        extractCookies(easy, &request.serverCookies);
        long response = 0;
        curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &response);
        if (rc == CURLE_ABORTED_BY_CALLBACK || !running_.load()) {
            curl_easy_cleanup(easy);
            return -1;
        }
        char* redirectUrlStr = nullptr;
        if (response == 301 || response == 302 || response == 303 || response == 307 || response == 308) {
            curl_easy_getinfo(easy, CURLINFO_REDIRECT_URL, &redirectUrlStr);
            const auto location = (redirectUrlStr && *redirectUrlStr)
                ? QString::fromUtf8(redirectUrlStr) : transfer->responseLocation;
            if (!location.isEmpty()) {
                const QUrl newUrl = request.url.resolved(QUrl(location));
                QString redirectError;
                QString newPinnedIp;
                if (!validateRedirectUrl(request.url, newUrl, request.allowPrivateNetworkRedirects, &redirectError, &newPinnedIp)) {
                    if (error) *error = redirectError;
                    curl_easy_cleanup(easy);
                    return -1;
                }
                const bool crossOrigin = (newUrl.scheme().toLower() != request.url.scheme().toLower()
                                       || newUrl.host().toLower() != request.url.host().toLower()
                                       || newUrl.port() != request.url.port());
                request.url = newUrl;
                request.pinnedIp = newPinnedIp;
                if (crossOrigin) {
                    applyCrossOriginHeaderPolicy(&request.headers, &request.username, &request.password);
                }
                redirectsFollowed++;
                curl_easy_cleanup(easy);
                continue;
            }
        }
        curl_off_t contentLength = -1;
        curl_easy_getinfo(easy, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &contentLength);
        if (rc == CURLE_OK && response < 400) {
            if (contentLength > 0) {
                total = static_cast<qint64>(contentLength);
            }
            *entityTag = transfer->responseEntityTag;
            *lastModified = transfer->responseLastModified;
        }
        curl_easy_cleanup(easy);
        break;
    }

    // 2. Range probe on the resolved target URL
    while (redirectsFollowed <= maxRedirects) {
        auto rangeProbe = std::make_unique<SegmentTransfer>();
        auto* easy = curl_easy_init();
        applyRequestOptions(easy, request, rangeProbe.get());
        rangeProbe->rangeBytes = "0-0";
        curl_easy_setopt(easy, CURLOPT_RANGE, rangeProbe->rangeBytes.constData());
        curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, rangeProbeWriteCallback);
        curl_easy_setopt(easy, CURLOPT_WRITEDATA, rangeProbe.get());
        curl_easy_setopt(easy, CURLOPT_XFERINFOFUNCTION, abortCallback);
        curl_easy_setopt(easy, CURLOPT_XFERINFODATA, &abortContext);
        curl_easy_setopt(easy, CURLOPT_NOPROGRESS, 0L);
        auto rc = curl_easy_perform(easy);
        extractCookies(easy, &request.serverCookies);
        long response = 0;
        curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &response);
        if (rc == CURLE_ABORTED_BY_CALLBACK || !running_.load()) {
            curl_easy_cleanup(easy);
            return -1;
        }
        char* redirectUrlStr = nullptr;
        if (response == 301 || response == 302 || response == 303 || response == 307 || response == 308) {
            curl_easy_getinfo(easy, CURLINFO_REDIRECT_URL, &redirectUrlStr);
            const auto location = (redirectUrlStr && *redirectUrlStr)
                ? QString::fromUtf8(redirectUrlStr) : rangeProbe->responseLocation;
            if (!location.isEmpty()) {
                const QUrl newUrl = request.url.resolved(QUrl(location));
                QString redirectError;
                QString newPinnedIp;
                if (!validateRedirectUrl(request.url, newUrl, request.allowPrivateNetworkRedirects, &redirectError, &newPinnedIp)) {
                    if (error) *error = redirectError;
                    curl_easy_cleanup(easy);
                    return -1;
                }
                const bool crossOrigin = (newUrl.scheme().toLower() != request.url.scheme().toLower()
                                       || newUrl.host().toLower() != request.url.host().toLower()
                                       || newUrl.port() != request.url.port());
                request.url = newUrl;
                request.pinnedIp = newPinnedIp;
                if (crossOrigin) {
                    applyCrossOriginHeaderPolicy(&request.headers, &request.username, &request.password);
                }
                redirectsFollowed++;
                curl_easy_cleanup(easy);
                continue;
            }
        }
        if (rc == CURLE_OK && response == 206
            && rangeProbe->contentRangeStart == 0 && rangeProbe->contentRangeEnd == 0) {
            *rangeSupported = true;
            if (rangeProbe->contentRangeTotal > 0) {
                total = rangeProbe->contentRangeTotal;
            }
            if (!rangeProbe->responseEntityTag.isEmpty()) {
                *entityTag = rangeProbe->responseEntityTag;
            }
            if (!rangeProbe->responseLastModified.isEmpty()) {
                *lastModified = rangeProbe->responseLastModified;
            }
        }
        curl_easy_cleanup(easy);
        break;
    }
    return total;
}

void CurlEpollDownloader::applyControlRequests()
{
    QHash<QString, DownloadStatus> incoming;
    {
        QMutexLocker lock(&controlMutex_);
        incoming.swap(controls_);
    }
    for (auto it = incoming.cbegin(); it != incoming.cend(); ++it) {
        deferredControls_.insert(it.key(), it.value());
    }
    if (deferredControls_.isEmpty()) {
        return;
    }

    for (auto it = transfers_.begin(); it != transfers_.end();) {
        const auto batch = it->second->batch;
        if (!deferredControls_.contains(batch->id)) {
            ++it;
            continue;
        }
        auto* easy = it->first;
        const auto host = hostKey(batch->request.url);
        curl_multi_remove_handle(multi_, easy);
        curl_easy_cleanup(easy);
        auto& segment = batch->segments[it->second->segmentIndex];
        segment.status = deferredControls_.value(batch->id);
        batch->activeSegments--;
        it = transfers_.erase(it);
        emit hostConnectionCountChanged(host, activeConnectionsForHost(host));
    }

    for (auto controlIt = deferredControls_.begin(); controlIt != deferredControls_.end();) {
        const auto id = controlIt.key();
        const auto requestedStatus = controlIt.value();
        const auto batch = batches_.value(id);
        if (!batch) {
            if (isPendingOrProbing(id)) {
                ++controlIt;
            } else {
                {
                    QMutexLocker lock(&controlMutex_);
                    canceledIds_.remove(id);
                }
                controlIt = deferredControls_.erase(controlIt);
            }
            continue;
        }
        batch->writer->close();
        for (auto& segment : batch->segments) {
            segment.durableWritten = segment.written;
            if (segment.status != DownloadStatus::Completed) {
                segment.status = requestedStatus;
            }
        }
        emit segmentsChanged(batch->id, batch->segments);
        batch->reportedStatus = requestedStatus;
        emit statusChanged(batch->id, requestedStatus, {});
        for (int index = retries_.size() - 1; index >= 0; --index) {
            if (retries_[index].batch->id == batch->id) {
                retries_.removeAt(index);
            }
        }
        batches_.remove(batch->id);
        batchOrder_.removeAll(batch->id);
        {
            QMutexLocker lock(&controlMutex_);
            canceledIds_.remove(id);
        }
        controlIt = deferredControls_.erase(controlIt);
    }
}

void CurlEpollDownloader::failBatch(const std::shared_ptr<DownloadBatch>& batch, const QString& message)
{
    if (!batch || batch->failed) {
        return;
    }
    batch->failed = true;

    const auto batchHost = hostKey(batch->request.url);
    bool hostCountChanged = false;
    for (auto it = transfers_.begin(); it != transfers_.end();) {
        if (it->second->batch == batch) {
            auto* otherEasy = it->first;
            const int segIdx = it->second->segmentIndex;
            curl_multi_remove_handle(multi_, otherEasy);
            curl_easy_cleanup(otherEasy);
            if (segIdx >= 0 && segIdx < batch->segments.size()
                && batch->segments[segIdx].status != DownloadStatus::Completed) {
                batch->segments[segIdx].status = DownloadStatus::Failed;
            }
            it = transfers_.erase(it);
            hostCountChanged = true;
        } else {
            ++it;
        }
    }
    if (hostCountChanged) {
        emit hostConnectionCountChanged(batchHost, activeConnectionsForHost(batchHost));
    }

    for (int i = retries_.size() - 1; i >= 0; --i) {
        if (retries_[i].batch == batch) {
            retries_.removeAt(i);
        }
    }

    for (auto& seg : batch->segments) {
        if (seg.status != DownloadStatus::Completed) {
            seg.status = DownloadStatus::Failed;
        }
        seg.durableWritten = seg.written;
    }
    batch->activeSegments = 0;
    batch->writer->sync();
    if (persistenceHandler_) {
        persistenceHandler_(batch->id, batch->segments);
    }
    emit segmentsChanged(batch->id, batch->segments);
    batch->writer->close();

    if (!batch->failureReported) {
        batch->failureReported = true;
        batch->reportedStatus = DownloadStatus::Failed;
        emit statusChanged(batch->id, DownloadStatus::Failed, message);
    }
    batches_.remove(batch->id);
    batchOrder_.removeAll(batch->id);
    deferredControls_.remove(batch->id);
}

void CurlEpollDownloader::checkCompleted()
{
    int pending = 0;
    while (auto* message = curl_multi_info_read(multi_, &pending)) {
        if (message->msg != CURLMSG_DONE) {
            continue;
        }
        auto* easy = message->easy_handle;
        auto transferIt = transfers_.find(easy);
        if (transferIt == transfers_.end()) {
            continue;
        }
        auto transfer = std::move(transferIt->second);
        const auto host = hostKey(transfer->batch->request.url);
        transfers_.erase(transferIt);
        emit hostConnectionCountChanged(host, activeConnectionsForHost(host));
        extractCookies(easy, &transfer->batch->request.serverCookies);
        long response = 0;
        curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &response);
        char* redirectUrlStr = nullptr;
        if (response == 301 || response == 302 || response == 303 || response == 307 || response == 308) {
            curl_easy_getinfo(easy, CURLINFO_REDIRECT_URL, &redirectUrlStr);
        }
        curl_multi_remove_handle(multi_, easy);
        curl_easy_cleanup(easy);

        auto batch = transfer->batch;
        auto& segment = batch->segments[transfer->segmentIndex];
        batch->activeSegments--;

        if (response == 301 || response == 302 || response == 303 || response == 307 || response == 308) {
            const auto location = (redirectUrlStr && *redirectUrlStr)
                ? QString::fromUtf8(redirectUrlStr) : transfer->responseLocation;
            if (!location.isEmpty() && transfer->attempt < qMax(0, batch->request.maximumRedirects)) {
                const QUrl oldUrl = batch->request.url;
                const QUrl newUrl = oldUrl.resolved(QUrl(location));
                QString redirectError;
                QString newPinnedIp;
                if (!validateRedirectUrl(oldUrl, newUrl, batch->request.allowPrivateNetworkRedirects, &redirectError, &newPinnedIp)) {
                    segment.status = DownloadStatus::Failed;
                    transfer->validationError = redirectError;
                    failBatch(batch, redirectError);
                    refillQueuedSegments();
                    continue;
                }
                const bool crossOrigin = (newUrl.scheme().toLower() != oldUrl.scheme().toLower()
                                       || newUrl.host().toLower() != oldUrl.host().toLower()
                                       || newUrl.port() != oldUrl.port());
                batch->request.url = newUrl;
                batch->request.pinnedIp = newPinnedIp;
                if (crossOrigin) {
                    applyCrossOriginHeaderPolicy(&batch->request.headers, &batch->request.username, &batch->request.password);
                }
                batch->activeSegments++;
                addSegmentTransfer(batch, transfer->segmentIndex, transfer->attempt + 1, transfer->forceHttp1);
                continue;
            }
        }
        const bool retryWouldRepeatPartialPost = batch->request.httpMethod == QStringLiteral("POST")
            && transfer->written > transfer->initialWritten;
        if (!batch->failed && !retryWouldRepeatPartialPost && isRetryable(message->data.result, response)
            && transfer->attempt < qMax(0, batch->request.maxRetries)) {
            const auto errCode = message->data.result;
            if (errCode == CURLE_HTTP2 || errCode == CURLE_HTTP2_STREAM
                || errCode == CURLE_OPERATION_TIMEDOUT) {
                transfer->forceHttp1 = true;
            }
            batch->activeSegments++;
            retryTransfer(std::move(transfer), response, message->data.result);
            continue;
        }
        const bool completeDeclaredRange = !transfer->requiresHttpPartialResponse
            || (transfer->contentRangeEnd >= transfer->contentRangeStart
                && transfer->written - transfer->initialWritten
                    == transfer->contentRangeEnd - transfer->contentRangeStart + 1);
        if (message->data.result == CURLE_OK && !completeDeclaredRange
            && transfer->validationError.isEmpty()) {
            transfer->validationError = QStringLiteral("response ended before its declared Content-Range was complete");
        }
        const bool responseAccepted = message->data.result == CURLE_OK && response < 400
            && (!transfer->requiresHttpPartialResponse || response == 206)
            && completeDeclaredRange
            && transfer->validationError.isEmpty();
        if (responseAccepted) {
            if (batch->total < 0 && batch->segments.size() == 1) {
                batch->total = transfer->written;
                segment.end = transfer->written > 0 ? segment.start + transfer->written - 1 : -1;
                if (!batch->writer->setExpectedSize(batch->total)) {
                    transfer->validationError = batch->writer->lastError();
                } else {
                    emit metadataChanged(batch->id, batch->total,
                                         batch->request.entityTag, batch->request.lastModified);
                }
            }
            const bool exactSegmentLength = segment.end < segment.start
                ? batch->total == 0 && transfer->written == 0
                : transfer->written == segment.end - segment.start + 1;
            if (transfer->validationError.isEmpty() && exactSegmentLength) {
                segment.status = DownloadStatus::Completed;
            } else {
                segment.status = DownloadStatus::Failed;
                if (transfer->validationError.isEmpty()) {
                    transfer->validationError = QStringLiteral("response ended before the segment was complete");
                }
                failBatch(batch, transfer->validationError);
                refillQueuedSegments();
                continue;
            }
        } else {
            const bool isRangeCorruption = (transfer->validationError == QStringLiteral("response exceeds its declared Content-Range")
                || (transfer->validationError == QStringLiteral("server returned an invalid Content-Range")
                    && transfer->initialWritten == 0));
            const bool canFallbackToSingleStream = isRangeCorruption
                && !batch->rangeFallbackAttempted
                && batch->request.resumeSegments.isEmpty()
                && transfer->initialWritten == 0
                && batch->request.httpMethod != QStringLiteral("POST")
                && (batch->segments.size() > 1 || batch->connectionLimit > 1 || batch->request.dynamicSegmentation);

            if (canFallbackToSingleStream) {
                batch->rangeFallbackAttempted = true;
                const auto batchHost = hostKey(batch->request.url);

                for (auto it = transfers_.begin(); it != transfers_.end();) {
                    if (it->second->batch == batch) {
                        auto* otherEasy = it->first;
                        curl_multi_remove_handle(multi_, otherEasy);
                        curl_easy_cleanup(otherEasy);
                        it = transfers_.erase(it);
                    } else {
                        ++it;
                    }
                }
                emit hostConnectionCountChanged(batchHost, activeConnectionsForHost(batchHost));

                for (int i = retries_.size() - 1; i >= 0; --i) {
                    if (retries_[i].batch == batch) {
                        retries_.removeAt(i);
                    }
                }

                batch->writer->close();
                batch->writer->open(batch->stagingPath, -1, false);

                batch->total = -1;
                batch->lastReceived = 0;
                batch->failed = false;
                batch->failureReported = false;
                batch->connectionLimit = 1;
                batch->request.segments = 1;
                batch->request.dynamicSegmentation = false;
                batch->segments.clear();

                SegmentInfo singleSeg;
                singleSeg.index = 0;
                singleSeg.start = 0;
                singleSeg.end = -1;
                singleSeg.written = 0;
                singleSeg.durableWritten = 0;
                singleSeg.status = DownloadStatus::Downloading;
                batch->segments.append(singleSeg);
                batch->activeSegments = 1;

                if (persistenceHandler_) {
                    persistenceHandler_(batch->id, batch->segments);
                }

                emit segmentsChanged(batch->id, batch->segments);
                batch->reportedStatus = DownloadStatus::Downloading;
                emit statusChanged(batch->id, DownloadStatus::Downloading,
                    QStringLiteral("Server byte-range response corrupted; falling back to single-stream download"));

                addSegmentTransfer(batch, 0, 0, false);
                continue;
            }

            segment.status = DownloadStatus::Failed;
            QString messageText;
            if (!transfer->validationError.isEmpty()) {
                messageText = transfer->validationError;
            } else if (batch->quotaExceeded) {
                messageText = QStringLiteral("session data limit exceeded");
            } else if (response == 401 || response == 403) {
                messageText = QStringLiteral("HTTP %1: server denied the request; the browser session headers or signed URL may have expired. Reload playback and capture it again.")
                                  .arg(response);
            } else if (transfer->requiresHttpPartialResponse && response > 0 && response < 400 && response != 206) {
                messageText = QStringLiteral("server did not honor requested byte range");
            } else if (response >= 400) {
                messageText = QStringLiteral("HTTP %1: %2")
                                  .arg(response)
                                  .arg(QString::fromUtf8(curl_easy_strerror(message->data.result)));
            } else {
                messageText = QString::fromUtf8(curl_easy_strerror(message->data.result));
            }
            failBatch(batch, messageText);
            refillQueuedSegments();
            continue;
        }

        emit segmentsChanged(batch->id, batch->segments);
        refillQueuedSegments();
        if (!batch->failed && batch->activeSegments > 0 && batch->reportedStatus != DownloadStatus::Connecting) {
            const bool hasActiveTransfers = std::any_of(transfers_.cbegin(), transfers_.cend(),
                [&batch](const auto& entry) { return entry.second->batch == batch; });
            const bool hasPendingRetry = std::any_of(retries_.cbegin(), retries_.cend(),
                [&batch](const auto& retry) { return retry.batch == batch; });
            if (!hasActiveTransfers && hasPendingRetry) {
                batch->reportedStatus = DownloadStatus::Connecting;
                emit statusChanged(batch->id, DownloadStatus::Connecting, {});
            }
        }
        if (batch->activeSegments == 0) {
            batch->writer->sync();
            for (auto& seg : batch->segments) {
                seg.durableWritten = seg.written;
            }
            if (persistenceHandler_) {
                persistenceHandler_(batch->id, batch->segments);
            }
            emit segmentsChanged(batch->id, batch->segments);
            batch->writer->close();
            const auto received = batchReceived(*batch);
            QString verificationError;
            if (!batch->failed && !verifyCompletedDownload(batch, &verificationError)) {
                batch->failed = true;
            }
            if (!batch->failed && !publishCompletedDownload(batch, &verificationError)) {
                batch->failed = true;
            }
            emit progressChanged(batch->id, received, batch->total, 0.0);
            if (!batch->failed) {
                batch->reportedStatus = DownloadStatus::Completed;
                emit statusChanged(batch->id, DownloadStatus::Completed, {});
            } else if (!batch->failureReported) {
                batch->failureReported = true;
                batch->reportedStatus = DownloadStatus::Failed;
                emit statusChanged(batch->id, DownloadStatus::Failed,
                                   verificationError.isEmpty() ? QStringLiteral("Download failed") : verificationError);
            }
            batches_.remove(batch->id);
            batchOrder_.removeAll(batch->id);
            deferredControls_.remove(batch->id);
        }
    }
}

void CurlEpollDownloader::emitBatchProgress()
{
    const auto now = QDateTime::currentMSecsSinceEpoch();
    for (const auto& batch : std::as_const(batches_)) {
        if (batch->failed || now - batch->lastTick < 250) {
            continue;
        }
        const auto received = batchReceived(*batch);
        const auto deltaBytes = received - batch->lastReceived;
        const auto deltaMs = now - batch->lastTick;
        const double bps = deltaMs > 0 ? (deltaBytes * 1000.0 / deltaMs) : 0.0;
        batch->lastReceived = received;
        batch->lastTick = now;
        emit progressChanged(batch->id, received, batch->total, bps);

        // Periodic durable checkpoint every 2000 ms
        if (now - batch->lastCheckpointMs >= 2000) {
            if (batch->writer->sync()) {
                batch->lastCheckpointMs = now;
                auto candidateSegments = batch->segments;
                for (auto& seg : candidateSegments) {
                    seg.durableWritten = seg.written;
                }
                if (persistenceHandler_) {
                    if (persistenceHandler_(batch->id, candidateSegments)) {
                        for (auto& seg : batch->segments) {
                            seg.durableWritten = seg.written;
                        }
                    }
                }
                emit segmentsChanged(batch->id, candidateSegments);
                continue;
            }
        }
        emit segmentsChanged(batch->id, batch->segments);
    }
}

}
