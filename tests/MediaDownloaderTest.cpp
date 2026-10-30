#include "core/MediaDownloader.h"
#include "core/MediaStreamProxy.h"

#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest/QtTest>

class MediaDownloaderTest final : public QObject {
    Q_OBJECT
private slots:
    void recognizesAdaptiveManifests()
    {
        QVERIFY(qtidm::MediaDownloader::supports(QUrl(QStringLiteral("https://example.test/master.m3u8?token=1"))));
        QVERIFY(qtidm::MediaDownloader::supports(QUrl(QStringLiteral("https://example.test/video.mpd"))));
        QVERIFY(!qtidm::MediaDownloader::supports(QUrl(QStringLiteral("https://example.test/video.mp4"))));
        QVERIFY(!qtidm::MediaDownloader::supports(
            QUrl(QStringLiteral("https://uwu.m3u8vault-10.example.test/extensionless-manifest"))));
        QVERIFY(qtidm::MediaDownloader::supports(
            QUrl(QStringLiteral("https://cdn.example.test/extensionless-manifest")), QStringLiteral("HLS")));
        QVERIFY(!qtidm::MediaDownloader::supports(QUrl(QStringLiteral("javascript:alert(1);//master.m3u8"))));
        QVERIFY(!qtidm::MediaDownloader::supports(QUrl(QStringLiteral("data:text/plain;base64,x.m3u8"))));
    }

    void detectsUnsupportedDrmDeclarations()
    {
        QVERIFY(qtidm::MediaDownloader::declaresUnsupportedDrm(
            "<ContentProtection schemeIdUri=\"urn:uuid:edef8ba9-79d6-4ace-a3c8-27dcd51d21ed\"/>"));
        QVERIFY(qtidm::MediaDownloader::declaresUnsupportedDrm(
            "#EXT-X-KEY:METHOD=SAMPLE-AES,KEYFORMAT=\"com.apple.streamingkeydelivery\""));
        QVERIFY(!qtidm::MediaDownloader::declaresUnsupportedDrm(
            "#EXT-X-KEY:METHOD=AES-128,URI=\"https://example.test/key\""));
    }

    void buildsProtectedHttpInputContext()
    {
        const QVariantMap headers {
            { QStringLiteral("User-Agent"), QStringLiteral("Mozilla/5.0 qtIDM-test") },
            { QStringLiteral("referer"), QStringLiteral("https://embed.example.test/player") },
            { QStringLiteral("Cookie"), QStringLiteral("session=browser-cookie") },
            { QStringLiteral("Origin"), QStringLiteral("https://embed.example.test") }
        };
        const auto arguments = qtidm::MediaDownloader::httpInputArguments(headers);
        const auto userAgent = arguments.indexOf(QStringLiteral("-user_agent"));
        const auto referer = arguments.indexOf(QStringLiteral("-referer"));
        const auto customHeaders = arguments.indexOf(QStringLiteral("-headers"));
        const auto analyzeDuration = arguments.indexOf(QStringLiteral("-analyzeduration"));
        const auto probeSize = arguments.indexOf(QStringLiteral("-probesize"));
        QVERIFY(userAgent >= 0);
        QVERIFY(referer >= 0);
        QVERIFY(customHeaders >= 0);
        QVERIFY(analyzeDuration >= 0);
        QVERIFY(probeSize >= 0);
        QCOMPARE(arguments.at(userAgent + 1), QStringLiteral("Mozilla/5.0 qtIDM-test"));
        QCOMPARE(arguments.at(referer + 1), QStringLiteral("https://embed.example.test/player"));
        QCOMPARE(arguments.at(analyzeDuration + 1), QStringLiteral("10000000"));
        QCOMPARE(arguments.at(probeSize + 1), QStringLiteral("10000000"));
        const auto headerText = arguments.at(customHeaders + 1);
        QVERIFY(headerText.contains(QStringLiteral("Cookie: session=browser-cookie\r\n")));
        QVERIFY(headerText.contains(QStringLiteral("Origin: https://embed.example.test\r\n")));
        QVERIFY(!headerText.contains(QStringLiteral("User-Agent"), Qt::CaseInsensitive));
        QVERIFY(!headerText.contains(QStringLiteral("Referer"), Qt::CaseInsensitive));
    }

    void downloadsAndMuxesSeparateDashTracks()
    {
        const auto ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
        if (ffmpeg.isEmpty()) {
            QSKIP("FFmpeg is not installed");
        }

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto manifest = dir.path() + QStringLiteral("/stream.mpd");
        const auto output = dir.path() + QStringLiteral("/output.mp4");

        QProcess generator;
        generator.setProgram(ffmpeg);
        generator.setArguments({
            QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
            QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"), QStringLiteral("testsrc=size=64x64:rate=10"),
            QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"), QStringLiteral("sine=frequency=1000:sample_rate=48000"),
            QStringLiteral("-t"), QStringLiteral("1"),
            QStringLiteral("-c:v"), QStringLiteral("mpeg4"),
            QStringLiteral("-c:a"), QStringLiteral("aac"),
            QStringLiteral("-f"), QStringLiteral("dash"), manifest
        });
        generator.start();
        QVERIFY(generator.waitForFinished(15000));
        QCOMPARE(generator.exitCode(), 0);
        QVERIFY(QFile::exists(manifest));

        qRegisterMetaType<qtidm::DownloadStatus>("qtidm::DownloadStatus");
        qtidm::MediaDownloader downloader;
        QSignalSpy statusSpy(&downloader, &qtidm::MediaDownloader::statusChanged);
        const auto id = downloader.enqueue(QUrl::fromLocalFile(manifest), output, {});

        bool completed = false;
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            for (const auto& event : statusSpy) {
                if (event.at(0).toString() == id
                    && event.at(1).value<qtidm::DownloadStatus>() == qtidm::DownloadStatus::Completed) {
                    completed = true;
                    return true;
                }
            }
            return false;
        })(), 15000);
        QVERIFY(completed);
        QVERIFY(QFileInfo(output).size() > 0);
        QVERIFY(!QFileInfo::exists(dir.path() + QStringLiteral("/output.part.mp4")));

        const auto ffprobe = QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
        QVERIFY(!ffprobe.isEmpty());
        QProcess probe;
        probe.start(ffprobe, {
            QStringLiteral("-v"), QStringLiteral("error"),
            QStringLiteral("-show_entries"), QStringLiteral("stream=codec_type"),
            QStringLiteral("-of"), QStringLiteral("csv=p=0"),
            output
        });
        QVERIFY(probe.waitForFinished(10000));
        QCOMPARE(probe.exitCode(), 0);
        const auto streams = probe.readAllStandardOutput();
        QVERIFY(streams.contains("video"));
        QVERIFY(streams.contains("audio"));
    }

    void rejectsLocalFileBoundaryViolationsAndEmbeddedCredentials()
    {
        // Non-existent local file must be rejected by supports()
        const QUrl nonExistentLocal(QStringLiteral("file:///nonexistent/path/stream.m3u8"));
        QVERIFY(!qtidm::MediaDownloader::supports(nonExistentLocal));

        // Embedded credentials must be rejected by supports()
        const QUrl userPassUrl(QStringLiteral("https://user:pass@example.com/stream.m3u8"));
        QVERIFY(!qtidm::MediaDownloader::supports(userPassUrl));

        // Cloud metadata IP SSRF target must be rejected
        const QUrl metadataUrl(QStringLiteral("http://169.254.169.254/latest/meta-data/stream.m3u8"));
        QVERIFY(!qtidm::MediaDownloader::supports(metadataUrl));

        // Existing local file is supported
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto realFile = dir.path() + QStringLiteral("/real.m3u8");
        {
            QFile file(realFile);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("#EXTM3U\n#EXT-X-VERSION:3\n");
        }
        const auto localUrl = QUrl::fromLocalFile(realFile);
        QVERIFY(qtidm::MediaDownloader::supports(localUrl));

        // Protocol whitelisting enforcement
        QCOMPARE(qtidm::MediaDownloader::protocolWhitelist(QUrl(QStringLiteral("https://example.com/playlist.m3u8"))),
                 QStringLiteral("http,https,tcp,tls,crypto"));
        QCOMPARE(qtidm::MediaDownloader::protocolWhitelist(localUrl),
                 QStringLiteral("file,crypto"));
        QVERIFY(qtidm::MediaDownloader::protocolWhitelist(QUrl(QStringLiteral("ftp://example.com/media.mpd"))).isEmpty());

        // Enqueueing an invalid/inaccessible URL fails immediately with status Failed
        qRegisterMetaType<qtidm::DownloadStatus>("qtidm::DownloadStatus");
        qtidm::MediaDownloader downloader;
        QSignalSpy statusSpy(&downloader, &qtidm::MediaDownloader::statusChanged);
        const auto failedId = downloader.enqueue(nonExistentLocal, dir.path() + QStringLiteral("/out.mp4"), {});
        QVERIFY(!failedId.isEmpty());

        bool failed = false;
        for (const auto& event : statusSpy) {
            if (event.at(0).toString() == failedId
                && event.at(1).value<qtidm::DownloadStatus>() == qtidm::DownloadStatus::Failed) {
                failed = true;
                break;
            }
        }
        QVERIFY(failed);
    }

    void sanitizesSuggestedFilenamePreservingDotsAndFullTitle()
    {
        QCOMPARE(qtidm::MediaDownloader::suggestedFilename(
            QStringLiteral("Steel Ball Run: JoJo's Bizarre Adventure Ep. 7")),
            QStringLiteral("Steel Ball Run - JoJo's Bizarre Adventure Ep. 7.mkv"));

        QCOMPARE(qtidm::MediaDownloader::suggestedFilename(
            QStringLiteral("Dr. Stone Season 3 Ep. 12")),
            QStringLiteral("Dr. Stone Season 3 Ep. 12.mkv"));

        QCOMPARE(qtidm::MediaDownloader::suggestedFilename(
            QStringLiteral("Fate/stay night [Unlimited Blade Works] - Ep. 01")),
            QStringLiteral("Fate - stay night [Unlimited Blade Works] - Ep. 01.mkv"));

        QCOMPARE(qtidm::MediaDownloader::suggestedFilename(
            QStringLiteral("uwu.m3u8")),
            QStringLiteral("uwu.mkv"));

        QCOMPARE(qtidm::MediaDownloader::suggestedFilename(
            QStringLiteral("ep.07.m3u8")),
            QStringLiteral("ep.07.mkv"));

        QCOMPARE(qtidm::MediaDownloader::suggestedFilename(
            QStringLiteral("master.m3u8")),
            QStringLiteral("master.mkv"));

        QCOMPARE(qtidm::MediaDownloader::suggestedFilename(
            QStringLiteral("video.mpd")),
            QStringLiteral("video.mkv"));

        QCOMPARE(qtidm::MediaDownloader::suggestedFilename(
            QStringLiteral("Episode 1.")),
            QStringLiteral("Episode 1.mkv"));

        QCOMPARE(qtidm::MediaDownloader::suggestedFilename(
            QStringLiteral("")),
            QStringLiteral("stream.mkv"));

        QCOMPARE(qtidm::MediaDownloader::mediaStagingPath(
            QStringLiteral("/downloads/Steel Ball Run - JoJo's Bizarre Adventure Ep. 7.mkv")),
            QStringLiteral("/downloads/Steel Ball Run - JoJo's Bizarre Adventure Ep. 7.part.mkv"));
    }

    void proxiesAndRewritesHlsStreamsForHttp2()
    {
        qtidm::MediaStreamProxy proxy;
        const QUrl root(QStringLiteral("https://vault-14.example.test/stream/uwu.m3u8"));
        QVariantMap headers;
        headers.insert(QStringLiteral("Referer"), QStringLiteral("https://kwik.cx/"));
        headers.insert(QStringLiteral("User-Agent"), QStringLiteral("TestBrowser/1.0"));

        QVERIFY(proxy.start(root, headers));
        QVERIFY(proxy.isRunning());
        QVERIFY(proxy.port() > 0);
        const auto proxyUrl = proxy.proxyUrl();
        QCOMPARE(proxyUrl.scheme(), QStringLiteral("http"));
        QCOMPARE(proxyUrl.host(), QStringLiteral("127.0.0.1"));
        QCOMPARE(proxyUrl.port(), proxy.port());
        QCOMPARE(proxyUrl.path(), QStringLiteral("/start.m3u8"));

        proxy.stop();
        QVERIFY(!proxy.isRunning());
    }
};

QTEST_MAIN(MediaDownloaderTest)
#include "MediaDownloaderTest.moc"
