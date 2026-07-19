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
#include <unistd.h>
#include <algorithm>
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
    bool failed = false;
    bool quotaExceeded = false;
    bool failureReported = false;
};

struct CurlEpollDownloader::SegmentTransfer {
    std::shared_ptr<DownloadBatch> batch;
    int segmentIndex = 0;
    qint64 start = 0;
    qint64 end = 0;
    qint64 written = 0;
    QByteArray urlBytes;
    QByteArray rangeBytes;
    QByteArray userPwdBytes;
    QByteArray proxyBytes;
    QByteArray requestBody;
    curl_slist* headers = nullptr;
    bool requiresHttpPartialResponse = false;
    int attempt = 0;
    qint64 retryAfterMs = 0;
    qint64 appliedSpeedLimit = -1;

    ~SegmentTransfer()
    {
        curl_slist_free_all(headers);
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
    const auto suffix = info.completeSuffix();
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

size_t headerCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    const auto bytes = size * nmemb;
    auto* transfer = static_cast<CurlEpollDownloader::SegmentTransfer*>(userdata);
    const QByteArray line(ptr, static_cast<qsizetype>(bytes));
    if (line.left(12).compare("Retry-After:", Qt::CaseInsensitive) == 0) {
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

void applyRequestOptions(CURL* easy, const DownloadRequest& request, CurlEpollDownloader::SegmentTransfer* transfer)
{
    transfer->urlBytes = request.url.toString(QUrl::FullyEncoded).toUtf8();
    curl_easy_setopt(easy, CURLOPT_URL, transfer->urlBytes.constData());
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, request.maximumRedirects > 0 ? 1L : 0L);
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
        const auto line = it.key().toUtf8() + ": " + it.value().toString().toUtf8();
        transfer->headers = curl_slist_append(transfer->headers, line.constData());
    }
    if (transfer->headers) {
        curl_easy_setopt(easy, CURLOPT_HTTPHEADER, transfer->headers);
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
    thread_ = std::jthread([this] { run(); });
}

void CurlEpollDownloader::stop()
{
    if (!running_.exchange(false)) {
        return;
    }
    wake();
    if (thread_.joinable()) {
        thread_.join();
    }
}

QString CurlEpollDownloader::enqueue(DownloadRequest request)
{
    const auto id = request.existingId.isEmpty() ? makeId(request.url, request.targetPath) : request.existingId;
    {
        QMutexLocker lock(&pendingMutex_);
        pending_.enqueue(QueuedRequest { id, std::move(request) });
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

int CurlEpollDownloader::timerCallback(CURLM*, long, void* userp)
{
    static_cast<CurlEpollDownloader*>(userp)->wake();
    return 0;
}

size_t CurlEpollDownloader::writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    const auto bytes = static_cast<qint64>(size * nmemb);
    auto* transfer = static_cast<SegmentTransfer*>(userdata);
    const auto sessionBytes = transfer->batch->owner->sessionBytesReceived_.fetch_add(bytes) + bytes;
    if (transfer->batch->request.sessionDataLimitBytes > 0
        && sessionBytes > transfer->batch->request.sessionDataLimitBytes) {
        transfer->batch->quotaExceeded = true;
        return 0;
    }
    const auto offset = transfer->start + transfer->written;
    if (!transfer->batch->writer->writeAt(offset, ptr, bytes)) {
        return 0;
    }
    transfer->written += bytes;
    auto& segment = transfer->batch->segments[transfer->segmentIndex];
    segment.written = transfer->written;
    segment.status = DownloadStatus::Downloading;
    return static_cast<size_t>(bytes);
}

int CurlEpollDownloader::progressCallback(void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    return 0;
}

void CurlEpollDownloader::run()
{
    curl_global_init(CURL_GLOBAL_DEFAULT);
    multi_ = curl_multi_init();
    epollFd_ = epoll_create1(EPOLL_CLOEXEC);
    wakeFd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

    curl_multi_setopt(multi_, CURLMOPT_SOCKETFUNCTION, &CurlEpollDownloader::socketCallback);
    curl_multi_setopt(multi_, CURLMOPT_SOCKETDATA, this);
    curl_multi_setopt(multi_, CURLMOPT_TIMERFUNCTION, &CurlEpollDownloader::timerCallback);
    curl_multi_setopt(multi_, CURLMOPT_TIMERDATA, this);

    epoll_event wakeEvent {};
    wakeEvent.events = EPOLLIN;
    wakeEvent.data.fd = wakeFd_;
    epoll_ctl(epollFd_, EPOLL_CTL_ADD, wakeFd_, &wakeEvent);

    int active = 0;
    while (running_) {
        addPendingTransfers();
        addDueRetries();
        applyRateLimits();
        applyControlRequests();
        epoll_event events[64] {};
        const int count = epoll_wait(epollFd_, events, 64, 1000);
        if (count == 0) {
            curl_multi_socket_action(multi_, CURL_SOCKET_TIMEOUT, 0, &active);
            checkCompleted();
            applyControlRequests();
            continue;
        }
        for (int i = 0; i < count; ++i) {
            if (events[i].data.fd == wakeFd_) {
                eventfd_t value = 0;
                eventfd_read(wakeFd_, &value);
                curl_multi_socket_action(multi_, CURL_SOCKET_TIMEOUT, 0, &active);
            } else {
                int action = 0;
                if (events[i].events & EPOLLIN) action |= CURL_CSELECT_IN;
                if (events[i].events & EPOLLOUT) action |= CURL_CSELECT_OUT;
                if (events[i].events & EPOLLERR) action |= CURL_CSELECT_ERR;
                curl_multi_socket_action(multi_, events[i].data.fd, action, &active);
            }
            checkCompleted();
            applyControlRequests();
        }
    }

    for (const auto& [easy, transfer] : transfers_) {
        curl_multi_remove_handle(multi_, easy);
        curl_easy_cleanup(easy);
    }
    transfers_.clear();
    batches_.clear();
    retries_.clear();
    curl_multi_cleanup(multi_);
    multi_ = nullptr;
    close(wakeFd_);
    close(epollFd_);
    curl_global_cleanup();
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
    const auto now = QDateTime::currentMSecsSinceEpoch();
    for (int index = retries_.size() - 1; index >= 0; --index) {
        if (retries_[index].dueAtMs > now) {
            continue;
        }
        const auto retry = retries_.takeAt(index);
        if (batches_.contains(retry.batch->id)) {
            addSegmentTransfer(retry.batch, retry.segmentIndex, retry.attempt);
        }
    }
}

void CurlEpollDownloader::wake()
{
    if (wakeFd_ >= 0) {
        eventfd_write(wakeFd_, 1);
    }
}

void CurlEpollDownloader::addPendingTransfers()
{
    QQueue<QueuedRequest> local;
    {
        QMutexLocker lock(&pendingMutex_);
        std::swap(local, pending_);
    }
    while (!local.isEmpty()) {
        addTransfer(local.dequeue());
    }
}

void CurlEpollDownloader::addTransfer(QueuedRequest queued)
{
    if (queued.request.resumeSegments.isEmpty() && pathOrStagingExists(queued.request.targetPath)) {
        if (queued.request.fileConflictPolicy == FileConflictPolicy::Skip) {
            emit statusChanged(queued.id, DownloadStatus::Canceled,
                               QStringLiteral("destination already exists; download skipped"));
            return;
        }
        if (queued.request.fileConflictPolicy == FileConflictPolicy::AutoRename) {
            const auto renamed = availableTargetPath(queued.request.targetPath);
            if (renamed.isEmpty()) {
                emit statusChanged(queued.id, DownloadStatus::Failed,
                                   QStringLiteral("could not find an available destination filename"));
                return;
            }
            queued.request.targetPath = renamed;
        }
    }
    bool rangeSupported = false;
    // Repeating a POST for probing or for independent byte ranges is unsafe:
    // it can create a second server-side action or return a different export.
    auto total = queued.request.httpMethod == QStringLiteral("POST")
        ? qint64(-1) : probeSize(queued.request, &rangeSupported);
    if (queued.request.expectedTotalBytes > 0 && total > 0 && queued.request.expectedTotalBytes != total) {
        emit statusChanged(queued.id, DownloadStatus::Failed, QStringLiteral("remote size changed; resume refused"));
        return;
    }
    if (total <= 0 && queued.request.expectedTotalBytes > 0) {
        total = queued.request.expectedTotalBytes;
    }
    const int requestedSegments = queued.request.httpMethod == QStringLiteral("POST")
        ? 1 : std::clamp(queued.request.segments, 1, 32);
    const bool canResumeSegments = rangeSupported && total > 0 && !queued.request.resumeSegments.isEmpty();
    constexpr qint64 minimumDynamicPartSize = 64 * 1024;
    const int dynamicParts = total > 0
        ? static_cast<int>(qMin<qint64>(requestedSegments * 4LL,
              qMax<qint64>(1, (total + minimumDynamicPartSize - 1) / minimumDynamicPartSize)))
        : requestedSegments;
    const int plannedSegments = queued.request.dynamicSegmentation
        ? qMax(requestedSegments, dynamicParts)
        : requestedSegments;
    const int segmentCount = canResumeSegments ? queued.request.resumeSegments.size()
        : (rangeSupported && total > 0 ? plannedSegments : 1);

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
    if (queued.request.sessionDataLimitBytes > 0) {
        const auto remainingQuota = queued.request.sessionDataLimitBytes - sessionBytesReceived_.load();
        const auto requiredBytes = total > 0 ? qMax<qint64>(0, total - QFileInfo(batch->stagingPath).size()) : 1;
        if (remainingQuota < requiredBytes) {
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
        emit statusChanged(batch->id, DownloadStatus::Failed,
                           QStringLiteral("could not migrate legacy partial download to %1").arg(batch->stagingPath));
        return;
    }
    if (!batch->writer->open(batch->stagingPath, total)) {
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
    batch->lastReceived = record.completedBytes;
    emit downloadAdded(record);
    emit statusChanged(batch->id, DownloadStatus::Downloading, {});

    for (const auto& segment : std::as_const(batch->segments)) {
        if (segment.end >= segment.start && segment.written >= segment.end - segment.start + 1) {
            batch->activeSegments--;
        }
    }
    startQueuedSegments(batch);
    emit segmentsChanged(batch->id, batch->segments);
    if (batch->activeSegments == 0) {
        batch->writer->close();
        QString verificationError;
        bool verified = verifyCompletedDownload(batch, &verificationError);
        if (verified) {
            verified = publishCompletedDownload(batch, &verificationError);
        }
        emit progressChanged(batch->id, batchReceived(*batch), batch->total, 0.0);
        emit statusChanged(batch->id, verified ? DownloadStatus::Completed : DownloadStatus::Failed, verificationError);
        batches_.remove(batch->id);
    }
}

void CurlEpollDownloader::startQueuedSegments(const std::shared_ptr<DownloadBatch>& batch)
{
    int runningForBatch = 0;
    int runningForHost = 0;
    const auto batchHost = hostKey(batch->request.url);
    for (const auto& [easy, transfer] : transfers_) {
        Q_UNUSED(easy);
        if (transfer->batch == batch) {
            runningForBatch++;
        }
        if (hostKey(transfer->batch->request.url) == batchHost) {
            runningForHost++;
        }
    }
    const int hostLimit = qBound(1, batch->request.perHostConnectionLimit, 128);
    for (auto& segment : batch->segments) {
        if (runningForBatch >= batch->connectionLimit || runningForHost >= hostLimit) {
            break;
        }
        if (segment.status == DownloadStatus::Queued
            || (segment.status == DownloadStatus::Paused && segment.written < segment.end - segment.start + 1)) {
            addSegmentTransfer(batch, segment.index);
            runningForBatch++;
            runningForHost++;
        }
    }
}

int CurlEpollDownloader::activeConnectionsForHost(const QString& host) const
{
    return static_cast<int>(std::count_if(transfers_.cbegin(), transfers_.cend(), [&host](const auto& entry) {
        return hostKey(entry.second->batch->request.url) == host;
    }));
}

void CurlEpollDownloader::refillQueuedSegments()
{
    for (const auto& batch : std::as_const(batches_)) {
        if (!batch->failed) {
            startQueuedSegments(batch);
        }
    }
}

bool CurlEpollDownloader::verifyCompletedDownload(const std::shared_ptr<DownloadBatch>& batch, QString* error) const
{
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

void CurlEpollDownloader::addSegmentTransfer(const std::shared_ptr<DownloadBatch>& batch, int segmentIndex, int attempt)
{
    auto& segment = batch->segments[segmentIndex];
    auto transfer = std::make_unique<SegmentTransfer>();
    transfer->batch = batch;
    transfer->segmentIndex = segmentIndex;
    transfer->start = segment.start;
    transfer->end = segment.end;
    transfer->written = segment.written;
    transfer->attempt = attempt;

    auto* easy = curl_easy_init();
    applyRequestOptions(easy, batch->request, transfer.get());
    const auto rangeStart = segment.start + segment.written;
    const bool resumed = segment.written > 0;
    if (segment.end >= rangeStart && (batch->segments.size() > 1 || resumed)) {
        transfer->rangeBytes = QByteArray::number(rangeStart) + "-" + QByteArray::number(segment.end);
        transfer->requiresHttpPartialResponse = batch->request.url.scheme().startsWith(QStringLiteral("http"));
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
    const int nextAttempt = transfer->attempt + 1;
    const qint64 exponentialDelay = static_cast<qint64>(qMax(100, batch->request.retryBaseDelayMs))
        * (qint64(1) << qMin(transfer->attempt, 9));
    const qint64 delayMs = qMin<qint64>(qMax(exponentialDelay, transfer->retryAfterMs), 300000);
    auto& segment = batch->segments[transfer->segmentIndex];
    segment.written = transfer->written;
    segment.status = DownloadStatus::Queued;
    emit segmentsChanged(batch->id, batch->segments);
    emit statusChanged(batch->id, DownloadStatus::Connecting,
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
        nextAttempt
    });
}

void CurlEpollDownloader::updateSocket(curl_socket_t socket, int action)
{
    epoll_event event {};
    event.events = epollEventsFromCurl(action) | EPOLLET;
    event.data.fd = socket;
    const bool known = sockets_.contains(socket);
    epoll_ctl(epollFd_, known ? EPOLL_CTL_MOD : EPOLL_CTL_ADD, socket, &event);
    sockets_[socket] = event.events;
}

void CurlEpollDownloader::removeSocket(curl_socket_t socket)
{
    epoll_ctl(epollFd_, EPOLL_CTL_DEL, socket, nullptr);
    sockets_.erase(socket);
}

qint64 CurlEpollDownloader::probeSize(const DownloadRequest& request, bool* rangeSupported)
{
    *rangeSupported = false;
    auto transfer = std::make_unique<SegmentTransfer>();
    auto* easy = curl_easy_init();
    applyRequestOptions(easy, request, transfer.get());
    curl_easy_setopt(easy, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, discardCallback);
    auto rc = curl_easy_perform(easy);
    curl_off_t contentLength = -1;
    curl_easy_getinfo(easy, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &contentLength);
    long response = 0;
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &response);
    curl_easy_cleanup(easy);
    if (rc != CURLE_OK || response >= 400) {
        return -1;
    }

    auto rangeProbe = std::make_unique<SegmentTransfer>();
    easy = curl_easy_init();
    applyRequestOptions(easy, request, rangeProbe.get());
    rangeProbe->rangeBytes = "0-0";
    curl_easy_setopt(easy, CURLOPT_RANGE, rangeProbe->rangeBytes.constData());
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, discardCallback);
    rc = curl_easy_perform(easy);
    response = 0;
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &response);
    curl_easy_cleanup(easy);
    *rangeSupported = rc == CURLE_OK && response == 206;
    return contentLength > 0 ? static_cast<qint64>(contentLength) : -1;
}

void CurlEpollDownloader::applyControlRequests()
{
    QHash<QString, DownloadStatus> controls;
    {
        QMutexLocker lock(&controlMutex_);
        controls.swap(controls_);
    }
    if (controls.isEmpty()) {
        return;
    }

    for (auto it = transfers_.begin(); it != transfers_.end();) {
        const auto batch = it->second->batch;
        if (!controls.contains(batch->id)) {
            ++it;
            continue;
        }
        auto* easy = it->first;
        const auto host = hostKey(batch->request.url);
        curl_multi_remove_handle(multi_, easy);
        curl_easy_cleanup(easy);
        auto& segment = batch->segments[it->second->segmentIndex];
        segment.status = controls.value(batch->id);
        batch->activeSegments--;
        it = transfers_.erase(it);
        emit hostConnectionCountChanged(host, activeConnectionsForHost(host));
    }

    for (auto controlIt = controls.cbegin(); controlIt != controls.cend(); ++controlIt) {
        const auto batch = batches_.value(controlIt.key());
        if (!batch) {
            continue;
        }
        batch->writer->close();
        for (auto& segment : batch->segments) {
            if (segment.status != DownloadStatus::Completed) {
                segment.status = controlIt.value();
            }
        }
        emit segmentsChanged(batch->id, batch->segments);
        emit statusChanged(batch->id, controlIt.value(), {});
        for (int index = retries_.size() - 1; index >= 0; --index) {
            if (retries_[index].batch->id == batch->id) {
                retries_.removeAt(index);
            }
        }
        batches_.remove(batch->id);
    }
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
        long response = 0;
        curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &response);
        curl_multi_remove_handle(multi_, easy);
        curl_easy_cleanup(easy);

        auto batch = transfer->batch;
        auto& segment = batch->segments[transfer->segmentIndex];
        batch->activeSegments--;
        if (isRetryable(message->data.result, response)
            && transfer->attempt < qMax(0, batch->request.maxRetries)) {
            batch->activeSegments++;
            retryTransfer(std::move(transfer), response, message->data.result);
            continue;
        }
        if (message->data.result == CURLE_OK && response < 400 && (!transfer->requiresHttpPartialResponse || response == 206)) {
            segment.status = DownloadStatus::Completed;
        } else {
            const bool firstFailure = !batch->failed;
            batch->failed = true;
            segment.status = DownloadStatus::Failed;
            if (firstFailure) {
                for (auto& queuedSegment : batch->segments) {
                    if (queuedSegment.status == DownloadStatus::Queued) {
                        queuedSegment.status = DownloadStatus::Failed;
                        batch->activeSegments--;
                    }
                }
            }
            QString messageText;
            if (batch->quotaExceeded) {
                messageText = QStringLiteral("session data limit exceeded");
            } else if (response == 401 || response == 403) {
                messageText = QStringLiteral("HTTP %1: server denied the request; the browser session headers or signed URL may have expired. Reload playback and capture it again.")
                                  .arg(response);
            } else if (transfer->requiresHttpPartialResponse && response != 206) {
                messageText = QStringLiteral("server did not honor requested byte range");
            } else if (response >= 400) {
                messageText = QStringLiteral("HTTP %1: %2")
                                  .arg(response)
                                  .arg(QString::fromUtf8(curl_easy_strerror(message->data.result)));
            } else {
                messageText = QString::fromUtf8(curl_easy_strerror(message->data.result));
            }
            if (!batch->failureReported) {
                batch->failureReported = true;
                emit statusChanged(batch->id, DownloadStatus::Failed, messageText);
            }
        }

        emit segmentsChanged(batch->id, batch->segments);
        refillQueuedSegments();
        if (batch->activeSegments == 0) {
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
                emit statusChanged(batch->id, DownloadStatus::Completed, {});
            } else if (!batch->failureReported) {
                emit statusChanged(batch->id, DownloadStatus::Failed,
                                   verificationError.isEmpty() ? QStringLiteral("Download failed") : verificationError);
            }
            batches_.remove(batch->id);
        }
    }

    const auto now = QDateTime::currentMSecsSinceEpoch();
    for (const auto& batch : std::as_const(batches_)) {
        if (now - batch->lastTick < 250) {
            continue;
        }
        const auto received = batchReceived(*batch);
        const auto deltaBytes = received - batch->lastReceived;
        const auto deltaMs = now - batch->lastTick;
        const double bps = deltaMs > 0 ? (deltaBytes * 1000.0 / deltaMs) : 0.0;
        batch->lastReceived = received;
        batch->lastTick = now;
        emit progressChanged(batch->id, received, batch->total, bps);
        emit segmentsChanged(batch->id, batch->segments);
    }
}

}
