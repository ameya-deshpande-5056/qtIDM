#include "core/MediaDownloader.h"

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
};

QTEST_MAIN(MediaDownloaderTest)
#include "MediaDownloaderTest.moc"
