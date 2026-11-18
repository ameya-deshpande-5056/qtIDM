#include "core/BrowserCamouflage.h"
#include "core/CurlEpollDownloader.h"
#include "core/DownloadScheduler.h"
#include "core/DownloadTypes.h"
#include "core/FileNaming.h"
#include "integration/sites/GoFileAdapter.h"
#include "integration/sites/GoogleDriveAdapter.h"
#include "integration/sites/MediaFireAdapter.h"
#include "integration/sites/SiteAdapterRegistry.h"
#include "storage/DownloadRepository.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>
#include <arpa/inet.h>
#include <atomic>
#include <mutex>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace qtidm;

namespace {
class LocalTestHttpServer final {
public:
    using Handler = std::function<QByteArray(const QByteArray& method, const QByteArray& path, const QByteArray& rawHeaders)>;

    explicit LocalTestHttpServer(Handler handler)
        : handler_(std::move(handler))
    {
        listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int yes = 1;
        ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0
            && ::listen(listenFd_, 32) == 0) {
            socklen_t len = sizeof(addr);
            if (::getsockname(listenFd_, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
                port_ = ntohs(addr.sin_port);
            }
        }
        running_.store(true);
        thread_ = std::thread([this]() { serveLoop(); });
    }

    ~LocalTestHttpServer()
    {
        running_.store(false);
        if (listenFd_ >= 0) {
            ::shutdown(listenFd_, SHUT_RDWR);
            ::close(listenFd_);
            listenFd_ = -1;
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    quint16 port() const { return port_; }

private:
    void serveLoop()
    {
        while (running_.load()) {
            sockaddr_in clientAddr {};
            socklen_t len = sizeof(clientAddr);
            const int clientFd = ::accept(listenFd_, reinterpret_cast<sockaddr*>(&clientAddr), &len);
            if (clientFd < 0) {
                if (!running_.load()) break;
                continue;
            }
            std::thread([this, clientFd]() {
                QByteArray req;
                char buf[4096];
                while (req.indexOf("\r\n\r\n") < 0 && req.size() < 65536) {
                    const auto n = ::recv(clientFd, buf, sizeof(buf), 0);
                    if (n <= 0) break;
                    req.append(buf, static_cast<qsizetype>(n));
                }
                const int lineEnd = req.indexOf("\r\n");
                if (lineEnd > 0) {
                    const auto firstLine = req.left(lineEnd).split(' ');
                    const QByteArray method = firstLine.size() > 0 ? firstLine[0] : QByteArray("GET");
                    const QByteArray path = firstLine.size() > 1 ? firstLine[1] : QByteArray("/");
                    const QByteArray resp = handler_(method, path, req);
                    qsizetype sent = 0;
                    while (sent < resp.size()) {
                        const auto rc = ::send(clientFd, resp.constData() + sent, static_cast<size_t>(resp.size() - sent), MSG_NOSIGNAL);
                        if (rc <= 0) break;
                        sent += rc;
                    }
                }
                ::shutdown(clientFd, SHUT_WR);
                ::close(clientFd);
            }).detach();
        }
    }

    Handler handler_;
    int listenFd_ = -1;
    quint16 port_ = 0;
    std::atomic_bool running_ = false;
    std::thread thread_;
};
} // namespace

class NetworkResilienceTest final : public QObject {
    Q_OBJECT

private slots:
    // --- D1: Site Adapters Unit Tests ---
    void mediaFireAdapterInfersOriginAndRecoversDownloadButton()
    {
        MediaFireAdapter adapter;
        const QUrl directUrl(QStringLiteral("https://download1502.mediafire.com/oldticket999/abc12345def/release.zip"));
        QVERIFY(adapter.matchesUrl(directUrl));
        QVERIFY(!adapter.matchesUrl(QUrl(QStringLiteral("https://mediafire.com.evil.test/file/abc12345def"))));
        QVERIFY(!adapter.matchesUrl(QUrl(QStringLiteral("https://notmediafire.com/file/abc12345def"))));

        const QUrl inferred = adapter.inferRefreshTarget(directUrl);
        QCOMPARE(inferred, QUrl(QStringLiteral("https://www.mediafire.com/file/abc12345def")));

        QVERIFY(adapter.indicatesExpired(directUrl, 302, {},
            QUrl(QStringLiteral("https://www.mediafire.com/download_repair.php?qkey=abc12345def")), {}));
        QVERIFY(adapter.indicatesExpired(directUrl, 200, QStringLiteral("text/html; charset=utf-8"), {}, {}));

        SiteHttpFetcher mockFetch = [](const QUrl& url, const QVariantMap&, const QString&, const QByteArray&) {
            SiteHttpResponse r;
            if (url.path() == QStringLiteral("/file/abc12345def")) {
                r.statusCode = 200;
                r.finalUrl = url;
                r.body = "<html><body><a class=\"input\" id=\"downloadButton\" href=\"https://download2099.mediafire.com/freshticket777/abc12345def/release.zip\">Download</a></body></html>";
            }
            return r;
        };

        const auto recovered = adapter.recover(directUrl, {}, {}, {}, mockFetch);
        QVERIFY(recovered.recovered);
        QCOMPARE(recovered.refreshedUrl, QUrl(QStringLiteral("https://download2099.mediafire.com/freshticket777/abc12345def/release.zip")));
        QCOMPARE(recovered.updatedRefreshTarget, QUrl(QStringLiteral("https://www.mediafire.com/file/abc12345def")));
    }

    void googleDriveAdapterHandlesVirusScanConfirmAndOAuthRefresh()
    {
        GoogleDriveAdapter adapter;
        const QUrl shareUrl(QStringLiteral("https://drive.google.com/file/d/1AbCdEfGhIjKlMnOpQrStUvWxYz/view"));
        QVERIFY(adapter.matchesUrl(shareUrl));
        QCOMPARE(GoogleDriveAdapter::extractFileId(shareUrl), QStringLiteral("1AbCdEfGhIjKlMnOpQrStUvWxYz"));
        QCOMPARE(adapter.inferRefreshTarget(shareUrl),
            QUrl(QStringLiteral("https://drive.google.com/uc?export=download&id=1AbCdEfGhIjKlMnOpQrStUvWxYz")));

        // 1. Virus-scan confirmation form extraction
        SiteHttpFetcher confirmFetcher = [](const QUrl& url, const QVariantMap&, const QString&, const QByteArray&) {
            SiteHttpResponse r;
            r.statusCode = 200;
            r.finalUrl = url;
            r.contentType = QStringLiteral("text/html");
            r.body = "<html><body><form id=\"download-form\" action=\"https://drive.usercontent.google.com/download\" method=\"get\">"
                     "<input type=\"hidden\" name=\"id\" value=\"1AbCdEfGhIjKlMnOpQrStUvWxYz\">"
                     "<input type=\"hidden\" name=\"export\" value=\"download\">"
                     "<input type=\"hidden\" name=\"confirm\" value=\"t\">"
                     "<input type=\"hidden\" name=\"uuid\" value=\"uuid-token-999\">"
                     "</form></body></html>";
            return r;
        };
        const auto confirmRes = adapter.recover(shareUrl, {}, {}, {}, confirmFetcher);
        QVERIFY(confirmRes.recovered);
        QVERIFY(confirmRes.refreshedUrl.toString().contains(QStringLiteral("drive.usercontent.google.com/download")));
        QVERIFY(confirmRes.refreshedUrl.toString().contains(QStringLiteral("confirm=t")));
        QVERIFY(confirmRes.refreshedUrl.toString().contains(QStringLiteral("uuid=uuid-token-999")));

        // 2. OAuth 2.0 refresh token exchange
        SiteHttpFetcher oauthFetcher = [](const QUrl& url, const QVariantMap&, const QString& method, const QByteArray& body) {
            SiteHttpResponse r;
            if (method == QStringLiteral("POST") && url.host() == QStringLiteral("oauth2.googleapis.com")
                && body.contains("refresh_token=ref-secret-1")) {
                r.statusCode = 200;
                r.body = "{\"access_token\":\"ya29.fresh-access-token\",\"expires_in\":3600}";
            }
            return r;
        };
        const QString oauthJson = QStringLiteral("{\"client_id\":\"cid-1\",\"client_secret\":\"csec-1\",\"refresh_token\":\"ref-secret-1\"}");
        const auto oauthRes = adapter.recover(shareUrl, {}, oauthJson, {}, oauthFetcher);
        QVERIFY(oauthRes.recovered);
        QCOMPARE(oauthRes.refreshedUrl, QUrl(QStringLiteral("https://www.googleapis.com/drive/v3/files/1AbCdEfGhIjKlMnOpQrStUvWxYz?alt=media")));
        QCOMPARE(oauthRes.extraHeaders.value(QStringLiteral("Authorization")).toString(), QStringLiteral("Bearer ya29.fresh-access-token"));
    }

    void goFileAdapterRequeriesApiWithTokenAndMatchesFile()
    {
        GoFileAdapter adapter;
        const QUrl expiredCdn(QStringLiteral("https://store1.gofile.io/download/web/old/dataset.tar.gz"));
        const QUrl origin(QStringLiteral("https://gofile.io/d/XyZ789"));
        QVERIFY(adapter.matchesUrl(expiredCdn));
        QVERIFY(adapter.indicatesExpired(expiredCdn, 403, {}, {}, {}));

        SiteHttpFetcher apiFetcher = [](const QUrl& url, const QVariantMap& headers, const QString& method, const QByteArray&) {
            SiteHttpResponse r;
            if (method == QStringLiteral("GET") && url.path() == QStringLiteral("/contents/XyZ789")
                && headers.value(QStringLiteral("Authorization")).toString() == QStringLiteral("Bearer tok-abc")) {
                r.statusCode = 200;
                r.body = "{\"status\":\"ok\",\"data\":{\"children\":{"
                         "\"id1\":{\"type\":\"file\",\"name\":\"readme.txt\",\"link\":\"https://store9.gofile.io/download/web/new/readme.txt\"},"
                         "\"id2\":{\"type\":\"file\",\"name\":\"dataset.tar.gz\",\"link\":\"https://store9.gofile.io/download/web/new/dataset.tar.gz\"}"
                         "}}}";
            }
            return r;
        };
        const auto res = adapter.recover(expiredCdn, origin, QStringLiteral("tok-abc"), {}, apiFetcher);
        QVERIFY(res.recovered);
        QCOMPARE(res.refreshedUrl, QUrl(QStringLiteral("https://store9.gofile.io/download/web/new/dataset.tar.gz")));
        QCOMPARE(res.extraHeaders.value(QStringLiteral("Cookie")).toString(), QStringLiteral("accountToken=tok-abc"));
        QCOMPARE(res.suggestedFilename, QStringLiteral("dataset.tar.gz"));
    }

    // --- D2: Per-Domain Rules in SQLite & Scheduler ---
    void domainRulesPersistInSqliteAndMatchSubdomainsDeterministically()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString dbPath = tempDir.filePath(QStringLiteral("rules.sqlite3"));

        {
            DownloadRepository repo;
            QVERIFY(repo.open(dbPath));

            DomainRule archiveRule;
            archiveRule.domain = QStringLiteral("*.ARCHIVE.ORG");
            archiveRule.includeSubdomains = true;
            archiveRule.maxSegments = 2;
            archiveRule.maxConcurrentDownloads = 2;
            archiveRule.customUserAgent = QStringLiteral("ArchivePoliteBot/1.0");
            archiveRule.customHeaders.insert(QStringLiteral("X-Archive-Policy"), QStringLiteral("gentle"));
            QVERIFY(repo.upsertDomainRule(archiveRule));

            DomainRule specificSub;
            specificSub.domain = QStringLiteral("ia800.us.archive.org");
            specificSub.includeSubdomains = false;
            specificSub.maxSegments = 1;
            QVERIFY(repo.upsertDomainRule(specificSub));

            DomainRule rapidRule;
            rapidRule.domain = QStringLiteral("rapidgator.net");
            rapidRule.includeSubdomains = true;
            rapidRule.maxConcurrentDownloads = 1;
            rapidRule.speedLimitBytesPerSecond = 512 * 1024;
            QVERIFY(repo.upsertDomainRule(rapidRule));
        }

        // Reopen database to verify SQLite durability
        DownloadRepository reopened;
        QVERIFY(reopened.open(dbPath));
        QCOMPARE(reopened.listDomainRules().size(), 3);

        // Exact subdomain match beats parent wildcard rule
        const auto exact = reopened.matchDomainRule(QStringLiteral("ia800.us.archive.org"));
        QVERIFY(exact.has_value());
        QCOMPARE(exact->maxSegments, 1);

        // Another subdomain inherits archive.org parent rule
        const auto sub = reopened.matchDomainRule(QStringLiteral("dn7200.ca.archive.org"));
        QVERIFY(sub.has_value());
        QCOMPARE(sub->maxSegments, 2);
        QCOMPARE(sub->customUserAgent, QStringLiteral("ArchivePoliteBot/1.0"));
        QCOMPARE(sub->customHeaders.value(QStringLiteral("X-Archive-Policy")).toString(), QStringLiteral("gentle"));

        // Look-alike host must NOT match
        QVERIFY(!reopened.matchDomainRule(QStringLiteral("notarchive.org")).has_value());
        QVERIFY(!reopened.matchDomainRule(QStringLiteral("archive.org.evil.com")).has_value());

        QVERIFY(reopened.removeDomainRule(QStringLiteral("rapidgator.net")));
        QCOMPARE(reopened.listDomainRules().size(), 2);
    }

    // --- D3: Magic-Byte Sniffing & Semantic Naming ---
    void magicBytesAndSemanticNamingResolveAllCoreFormats()
    {
        QCOMPARE(FileNaming::sniffExtensionFromMagicBytes(QByteArray("\x1a\x45\xdf\xa3\x9f\x42\x86\x81\x01matroska", 17)), QStringLiteral("mkv"));
        QCOMPARE(FileNaming::sniffExtensionFromMagicBytes(QByteArray("\x1a\x45\xdf\xa3\x9f\x42\x82\x84webm", 13)), QStringLiteral("webm"));
        QCOMPARE(FileNaming::sniffExtensionFromMagicBytes(QByteArray("\x00\x00\x00\x18" "ftypisom\x00\x00\x02\x00", 16)), QStringLiteral("mp4"));
        QCOMPARE(FileNaming::sniffExtensionFromMagicBytes(QByteArray("\x00\x00\x00\x18" "ftypM4V \x00\x00\x00\x00", 16)), QStringLiteral("m4v"));
        QCOMPARE(FileNaming::sniffExtensionFromMagicBytes(QByteArray("PK\x03\x04\x14\x00\x00\x00", 8)), QStringLiteral("zip"));
        QCOMPARE(FileNaming::sniffExtensionFromMagicBytes(QByteArray("7z\xbc\xaf\x27\x1c\x00\x04", 8)), QStringLiteral("7z"));
        QCOMPARE(FileNaming::sniffExtensionFromMagicBytes(QByteArray("Rar!\x1a\x07\x01\x00", 8)), QStringLiteral("rar"));
        QCOMPARE(FileNaming::sniffExtensionFromMagicBytes(QByteArray("%PDF-1.7\n", 9)), QStringLiteral("pdf"));
        QCOMPARE(FileNaming::sniffExtensionFromMagicBytes(QByteArray("\x1f\x8b\x08\x00", 4)), QStringLiteral("gz"));
        QCOMPARE(FileNaming::sniffExtensionFromMagicBytes(QByteArray("\x89PNG\r\n\x1a\n", 8)), QStringLiteral("png"));
        QCOMPARE(FileNaming::sniffExtensionFromMagicBytes(QByteArray("fLaC\x00\x00\x00\x22", 8)), QStringLiteral("flac"));

        QVERIFY(FileNaming::isMeaninglessFilename(QStringLiteral("videoplayback")));
        QVERIFY(FileNaming::isMeaninglessFilename(QStringLiteral("download.bin")));
        QVERIFY(FileNaming::isMeaninglessFilename(QStringLiteral("download.php")));
        QVERIFY(FileNaming::isMeaninglessFilename(QStringLiteral("550e8400-e29b-41d4-a716-446655440000")));
        QVERIFY(FileNaming::isMeaninglessFilename(QStringLiteral("a3f9c02d8e4b1f7a9c02112233445566")));
        QVERIFY(!FileNaming::isMeaninglessFilename(QStringLiteral("Ubuntu_24.04_Desktop.iso")));
        QVERIFY(!FileNaming::isMeaninglessFilename(QStringLiteral("split_success.bin")));

        // Meaningless URL name + browser page title + MP4 magic bytes -> semantic name
        const QByteArray mp4Magic("\x00\x00\x00\x18" "ftypmp42\x00\x00\x00\x00", 16);
        QCOMPARE(FileNaming::resolveSemanticFilename(
                     QStringLiteral("videoplayback"),
                     QStringLiteral("4K Nature Documentary - YouTube"),
                     mp4Magic),
                 QStringLiteral("4K Nature Documentary.mp4"));

        // Specialized ZIP container (.docx) must NOT be downgraded to .zip
        const QByteArray zipMagic("PK\x03\x04\x14\x00\x00\x00", 8);
        QCOMPARE(FileNaming::resolveSemanticFilename(
                     QStringLiteral("Quarterly_Report.docx"),
                     QStringLiteral("Drive"),
                     zipMagic),
                 QStringLiteral("Quarterly_Report.docx"));
    }

    // --- D4: Browser-Like TLS & Strict Header Ordering ---
    void browserCamouflageOrdersHeadersAndConfiguresTlsProfiles()
    {
        QCOMPARE(BrowserCamouflage::resolveProfile(BrowserTlsProfile::Auto,
                     QStringLiteral("Mozilla/5.0 (X11; Linux x86_64; rv:136.0) Gecko/20100101 Firefox/136.0")),
                 BrowserTlsProfile::Firefox);
        QCOMPARE(BrowserCamouflage::resolveProfile(BrowserTlsProfile::Auto,
                     QStringLiteral("Mozilla/5.0 Chrome/134.0.0.0 Safari/537.36")),
                 BrowserTlsProfile::Chrome);
        QVERIFY(BrowserCamouflage::tls12CipherList(BrowserTlsProfile::Chrome).contains("ECDHE-ECDSA-AES128-GCM-SHA256"));
        QVERIFY(BrowserCamouflage::tls13CipherSuites(BrowserTlsProfile::Firefox).startsWith("TLS_AES_128_GCM_SHA256:TLS_CHACHA20_POLY1305_SHA256"));

        QVariantMap rawHeaders;
        rawHeaders.insert(QStringLiteral("Accept-Language"), QStringLiteral("en-US,en;q=0.9"));
        rawHeaders.insert(QStringLiteral("Referer"), QStringLiteral("https://example.test/watch"));
        rawHeaders.insert(QStringLiteral("Accept"), QStringLiteral("*/*"));
        rawHeaders.insert(QStringLiteral("User-Agent"), QStringLiteral("Mozilla/5.0 Chrome/134.0.0.0"));
        rawHeaders.insert(QStringLiteral("sec-ch-ua"), QStringLiteral("\"Chromium\";v=\"134\""));
        rawHeaders.insert(QStringLiteral("Sec-Fetch-Site"), QStringLiteral("same-origin"));
        rawHeaders.insert(QStringLiteral("_qtidmInternal"), QStringLiteral("ignore-me"));
        rawHeaders.insert(QStringLiteral("Cookie"), QStringLiteral("sid=123"));

        const auto chromeOrder = BrowserCamouflage::buildOrderedHeaders(
            rawHeaders, BrowserTlsProfile::Chrome, QUrl(QStringLiteral("https://example.test/file.bin")));
        QStringList chromeKeys;
        for (const auto& [k, _] : chromeOrder) chromeKeys.append(k.toLower());
        QVERIFY(!chromeKeys.contains(QStringLiteral("_qtidminternal")));
        QVERIFY(!chromeKeys.contains(QStringLiteral("cookie")));
        QVERIFY(chromeKeys.indexOf(QStringLiteral("sec-ch-ua")) < chromeKeys.indexOf(QStringLiteral("user-agent")));
        QVERIFY(chromeKeys.indexOf(QStringLiteral("user-agent")) < chromeKeys.indexOf(QStringLiteral("accept")));
        QVERIFY(chromeKeys.indexOf(QStringLiteral("accept")) < chromeKeys.indexOf(QStringLiteral("sec-fetch-site")));
        QVERIFY(chromeKeys.indexOf(QStringLiteral("sec-fetch-site")) < chromeKeys.indexOf(QStringLiteral("referer")));
        QVERIFY(chromeKeys.indexOf(QStringLiteral("referer")) < chromeKeys.indexOf(QStringLiteral("accept-language")));

        const auto ffOrder = BrowserCamouflage::buildOrderedHeaders(
            rawHeaders, BrowserTlsProfile::Firefox, QUrl(QStringLiteral("https://example.test/file.bin")));
        QStringList ffKeys;
        for (const auto& [k, _] : ffOrder) ffKeys.append(k.toLower());
        QVERIFY(ffKeys.indexOf(QStringLiteral("user-agent")) < ffKeys.indexOf(QStringLiteral("accept")));
        QVERIFY(ffKeys.indexOf(QStringLiteral("accept")) < ffKeys.indexOf(QStringLiteral("accept-language")));
        QVERIFY(ffKeys.indexOf(QStringLiteral("accept-language")) < ffKeys.indexOf(QStringLiteral("referer")));

        // Applying TLS profile to a real CURL handle must succeed cleanly
        CURL* easy = curl_easy_init();
        QVERIFY(easy != nullptr);
        BrowserCamouflage::applyTlsProfile(easy, BrowserTlsProfile::Chrome);
        BrowserCamouflage::applyTlsProfile(easy, BrowserTlsProfile::Firefox);
        curl_easy_cleanup(easy);
    }

    // --- Live HTTP End-to-End Integration: D1 + D2 + D3 + D4 Together! ---
    void liveHttpRecoveryDomainRuleMagicRenameAndHeaderCamouflage()
    {
        QByteArray zipPayload("PK\x03\x04\x14\x00\x00\x00", 8);
        zipPayload.append(QByteArray(32768, 'Z'));

        std::mutex wireMutex;
        QByteArray capturedFreshHeaders;
        std::atomic<int> repairHits = 0;
        std::atomic<int> originHits = 0;

        quint16 serverPort = 0;
        LocalTestHttpServer server([&](const QByteArray& method, const QByteArray& path, const QByteArray& rawHeaders) -> QByteArray {
            if (path == "/expired-mf") {
                repairHits++;
                return "HTTP/1.1 302 Found\r\n"
                       "Location: /download_repair.php?qkey=abc123456789\r\n"
                       "Content-Length: 0\r\n"
                       "Connection: close\r\n\r\n";
            }
            if (path == "/file/abc123456789") {
                originHits++;
                const QByteArray html = QStringLiteral(
                    "<html><body><a id=\"downloadButton\" href=\"http://127.0.0.1:%1/fresh-archive\">Download</a></body></html>")
                    .arg(serverPort).toUtf8();
                return "HTTP/1.1 200 OK\r\n"
                       "Content-Type: text/html\r\n"
                       "Content-Length: " + QByteArray::number(html.size()) + "\r\n"
                       "Connection: close\r\n\r\n" + html;
            }
            if (path == "/fresh-archive") {
                {
                    std::lock_guard<std::mutex> lock(wireMutex);
                    capturedFreshHeaders = rawHeaders;
                }
                if (method == "HEAD") {
                    return "HTTP/1.1 200 OK\r\n"
                           "Content-Length: " + QByteArray::number(zipPayload.size()) + "\r\n"
                           "Connection: close\r\n\r\n";
                }
                return "HTTP/1.1 200 OK\r\n"
                       "Content-Type: application/octet-stream\r\n"
                       "Content-Length: " + QByteArray::number(zipPayload.size()) + "\r\n"
                       "Connection: close\r\n\r\n" + zipPayload;
            }
            return "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        });
        serverPort = server.port();
        QVERIFY(serverPort > 0);

        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        CurlEpollDownloader downloader;
        DomainRule localRule;
        localRule.domain = QStringLiteral("127.0.0.1");
        localRule.maxSegments = 2;
        localRule.customUserAgent = QStringLiteral("Mozilla/5.0 Chrome/134.0.0.0 DomainRuleTest");
        localRule.customHeaders.insert(QStringLiteral("X-Domain-Policy"), QStringLiteral("enforced"));
        downloader.setDomainRules({ localRule });

        QString finalRenamedPath;
        connect(&downloader, &CurlEpollDownloader::targetPathChanged,
            [&finalRenamedPath](const QString&, const QString& newPath) {
                finalRenamedPath = newPath;
            });

        QSignalSpy statusSpy(&downloader, &CurlEpollDownloader::statusChanged);
        downloader.start();

        DownloadRequest req;
        req.url = QUrl(QStringLiteral("http://127.0.0.1:%1/expired-mf").arg(serverPort));
        req.refreshTarget = QUrl(QStringLiteral("http://127.0.0.1:%1/file/abc123456789").arg(serverPort));
        req.siteAdapterHint = QStringLiteral("mediafire");
        req.targetPath = dir.filePath(QStringLiteral("videoplayback"));
        req.pageTitle = QStringLiteral("My Recovered Archive - MediaFire");
        req.segments = 8; // Must be capped to 2 by DomainRule!
        req.allowPrivateNetworkRedirects = true;

        const QString id = downloader.enqueue(req);
        bool completed = false;
        const auto deadline = QDateTime::currentMSecsSinceEpoch() + 10000;
        while (QDateTime::currentMSecsSinceEpoch() < deadline) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
            for (const auto& args : statusSpy) {
                if (args[0].toString() == id && args[1].value<DownloadStatus>() == DownloadStatus::Completed) {
                    completed = true;
                    break;
                }
            }
            if (completed) break;
        }
        downloader.stop();

        QVERIFY2(completed, "Expired MediaFire URL did not recover and complete");
        QVERIFY(repairHits.load() >= 1);
        QVERIFY(originHits.load() >= 1);

        // D3: Magic-byte sniffing (PK\x03\x04 -> .zip) + pageTitle renamed "videoplayback" -> "My Recovered Archive.zip"
        QCOMPARE(QFileInfo(finalRenamedPath).fileName(), QStringLiteral("My Recovered Archive.zip"));
        QFile outFile(finalRenamedPath);
        QVERIFY(outFile.open(QIODevice::ReadOnly));
        QCOMPARE(outFile.readAll(), zipPayload);

        // D2 + D4: Custom DomainRule User-Agent & Header appeared on the wire in browser order!
        QByteArray wire;
        {
            std::lock_guard<std::mutex> lock(wireMutex);
            wire = capturedFreshHeaders;
        }
        QVERIFY(wire.contains("User-Agent: Mozilla/5.0 Chrome/134.0.0.0 DomainRuleTest"));
        QVERIFY(wire.contains("X-Domain-Policy: enforced"));
        QVERIFY(wire.indexOf("User-Agent:") < wire.indexOf("Accept:"));
    }

    void downloadRecordRoundTripsRefreshTargetInSqlite()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString dbPath = tempDir.filePath(QStringLiteral("records.sqlite3"));

        {
            DownloadRepository repo;
            QVERIFY(repo.open(dbPath));
            DownloadRecord rec;
            rec.id = QStringLiteral("rec-d1-001");
            rec.url = QUrl(QStringLiteral("https://download123.mediafire.com/tok/abc12345def/archive.7z"));
            rec.targetPath = tempDir.filePath(QStringLiteral("archive.7z"));
            rec.status = DownloadStatus::Paused;
            rec.createdAt = QDateTime::currentDateTimeUtc();
            rec.updatedAt = rec.createdAt;
            rec.request.url = rec.url;
            rec.request.targetPath = rec.targetPath;
            rec.request.refreshTarget = QUrl(QStringLiteral("https://www.mediafire.com/file/abc12345def"));
            rec.request.siteAdapterToken = QStringLiteral("tok-secret-99");
            rec.request.siteAdapterHint = QStringLiteral("mediafire");
            rec.request.pageTitle = QStringLiteral("Backup Archive - MediaFire");
            rec.request.browserTlsProfile = QStringLiteral("firefox");
            rec.request.enableSemanticNaming = true;
            QVERIFY(repo.upsertDownload(rec));
        }

        DownloadRepository reopened;
        QVERIFY(reopened.open(dbPath));
        const auto loaded = reopened.listDownloads();
        QCOMPARE(loaded.size(), 1);
        QCOMPARE(loaded[0].request.refreshTarget, QUrl(QStringLiteral("https://www.mediafire.com/file/abc12345def")));
        QCOMPARE(loaded[0].request.siteAdapterToken, QStringLiteral("tok-secret-99"));
        QCOMPARE(loaded[0].request.siteAdapterHint, QStringLiteral("mediafire"));
        QCOMPARE(loaded[0].request.pageTitle, QStringLiteral("Backup Archive - MediaFire"));
        QCOMPARE(loaded[0].request.browserTlsProfile, QStringLiteral("firefox"));
        QVERIFY(loaded[0].request.enableSemanticNaming);
    }

    void liveHttpGoogleDriveVirusScanConfirmRecovery()
    {
        QByteArray mkvPayload("\x1a\x45\xdf\xa3\x9f\x42\x86\x81\x01matroska", 17);
        mkvPayload.append(QByteArray(49152, 'M'));

        std::atomic<int> interstitialHits = 0;
        std::atomic<int> confirmedHits = 0;
        quint16 serverPort = 0;

        LocalTestHttpServer server([&](const QByteArray& method, const QByteArray& path, const QByteArray&) -> QByteArray {
            if (path.startsWith("/uc?")) {
                interstitialHits++;
                const QByteArray html = QStringLiteral(
                    "<html><body><form id=\"download-form\" action=\"http://127.0.0.1:%1/gdrive-confirmed\" method=\"get\">"
                    "<input type=\"hidden\" name=\"id\" value=\"1AbCdEfGhIjKlMnOpQrStUvWxYz\">"
                    "<input type=\"hidden\" name=\"confirm\" value=\"t\">"
                    "<input type=\"hidden\" name=\"uuid\" value=\"scan-token-42\">"
                    "</form></body></html>")
                    .arg(serverPort).toUtf8();
                if (method == "HEAD") {
                    return "HTTP/1.1 200 OK\r\n"
                           "Content-Type: text/html; charset=utf-8\r\n"
                           "Content-Length: " + QByteArray::number(html.size()) + "\r\n"
                           "Connection: close\r\n\r\n";
                }
                return "HTTP/1.1 200 OK\r\n"
                       "Content-Type: text/html; charset=utf-8\r\n"
                       "Content-Length: " + QByteArray::number(html.size()) + "\r\n"
                       "Connection: close\r\n\r\n" + html;
            }
            if (path.startsWith("/gdrive-confirmed?") && path.contains("confirm=t") && path.contains("uuid=scan-token-42")) {
                confirmedHits++;
                return "HTTP/1.1 200 OK\r\n"
                       "Content-Type: application/octet-stream\r\n"
                       "Content-Length: " + QByteArray::number(mkvPayload.size()) + "\r\n"
                       "Connection: close\r\n\r\n" + mkvPayload;
            }
            return "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        });
        serverPort = server.port();
        QVERIFY(serverPort > 0);

        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        CurlEpollDownloader downloader;
        QString finalRenamedPath;
        connect(&downloader, &CurlEpollDownloader::targetPathChanged,
            [&finalRenamedPath](const QString&, const QString& newPath) {
                finalRenamedPath = newPath;
            });
        QSignalSpy statusSpy(&downloader, &CurlEpollDownloader::statusChanged);
        downloader.start();

        DownloadRequest req;
        req.url = QUrl(QStringLiteral("http://127.0.0.1:%1/uc?export=download&id=1AbCdEfGhIjKlMnOpQrStUvWxYz").arg(serverPort));
        req.refreshTarget = req.url;
        req.siteAdapterHint = QStringLiteral("google-drive");
        req.targetPath = dir.filePath(QStringLiteral("download.bin"));
        req.pageTitle = QStringLiteral("Keynote Talk - Google Drive");
        req.allowPrivateNetworkRedirects = true;

        const QString id = downloader.enqueue(req);
        bool completed = false;
        const auto deadline = QDateTime::currentMSecsSinceEpoch() + 10000;
        while (QDateTime::currentMSecsSinceEpoch() < deadline) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
            for (const auto& args : statusSpy) {
                if (args[0].toString() == id && args[1].value<DownloadStatus>() == DownloadStatus::Completed) {
                    completed = true;
                    break;
                }
            }
            if (completed) break;
        }
        downloader.stop();

        QVERIFY2(completed, "Google Drive HTML virus-scan prompt did not recover and complete");
        QVERIFY(interstitialHits.load() >= 1);
        QVERIFY(confirmedHits.load() >= 1);
        QCOMPARE(QFileInfo(finalRenamedPath).fileName(), QStringLiteral("Keynote Talk.mkv"));
        QFile out(finalRenamedPath);
        QVERIFY(out.open(QIODevice::ReadOnly));
        QCOMPARE(out.readAll(), mkvPayload);
    }

    void downloadSchedulerEnforcesPerDomainConcurrencyLimit()
    {
        const QByteArray payload(16384, 'R');
        std::atomic<int> inFlightGets = 0;
        std::atomic<int> maxConcurrentGets = 0;

        LocalTestHttpServer server([&](const QByteArray& method, const QByteArray&, const QByteArray&) -> QByteArray {
            if (method == "HEAD") {
                return "HTTP/1.1 200 OK\r\n"
                       "Content-Length: " + QByteArray::number(payload.size()) + "\r\n"
                       "Connection: close\r\n\r\n";
            }
            const int cur = ++inFlightGets;
            int prev = maxConcurrentGets.load();
            while (cur > prev && !maxConcurrentGets.compare_exchange_weak(prev, cur)) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            --inFlightGets;
            return "HTTP/1.1 200 OK\r\n"
                   "Content-Type: application/octet-stream\r\n"
                   "Content-Length: " + QByteArray::number(payload.size()) + "\r\n"
                   "Connection: close\r\n\r\n" + payload;
        });
        QVERIFY(server.port() > 0);

        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        CurlEpollDownloader downloader;
        DownloadScheduler scheduler(downloader);
        scheduler.setQueueConcurrency(QStringLiteral("Main"), 4); // Global queue allows 4

        DomainRule limitOne;
        limitOne.domain = QStringLiteral("127.0.0.1");
        limitOne.maxConcurrentDownloads = 1; // Domain rule caps to 1!
        limitOne.maxSegments = 1;
        scheduler.setDomainRules({ limitOne });

        int completedCount = 0;
        connect(&downloader, &CurlEpollDownloader::statusChanged,
            [&completedCount](const QString&, DownloadStatus status, const QString&) {
                if (status == DownloadStatus::Completed) {
                    completedCount++;
                }
            });

        downloader.start();

        DownloadRequest r1;
        r1.url = QUrl(QStringLiteral("http://127.0.0.1:%1/file1.bin").arg(server.port()));
        r1.targetPath = dir.filePath(QStringLiteral("file1.bin"));
        r1.segments = 4;
        r1.enableSemanticNaming = false;
        r1.allowPrivateNetworkRedirects = true;

        DownloadRequest r2 = r1;
        r2.url = QUrl(QStringLiteral("http://127.0.0.1:%1/file2.bin").arg(server.port()));
        r2.targetPath = dir.filePath(QStringLiteral("file2.bin"));

        scheduler.schedule(r1);
        scheduler.schedule(r2);

        const auto deadline = QDateTime::currentMSecsSinceEpoch() + 10000;
        while (completedCount < 2 && QDateTime::currentMSecsSinceEpoch() < deadline) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        }
        downloader.stop();

        QCOMPARE(completedCount, 2);
        QCOMPARE(maxConcurrentGets.load(), 1);
    }
};

QTEST_MAIN(NetworkResilienceTest)
#include "NetworkResilienceTest.moc"
