#include "integration/SocialMediaExtractor.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>

class SocialMediaExtractorTest final : public QObject {
    Q_OBJECT
private slots:
    void recognizesSupportedSitesButNotYouTube()
    {
        QVERIFY(qtidm::SocialMediaExtractor::supports(
            QUrl(QStringLiteral("https://www.instagram.com/reel/example/"))));
        QVERIFY(qtidm::SocialMediaExtractor::supports(
            QUrl(QStringLiteral("https://mobile.twitter.com/user/status/1"))));
        QVERIFY(!qtidm::SocialMediaExtractor::supports(
            QUrl(QStringLiteral("https://www.youtube.com/watch?v=example"))));
    }

    void parsesFormatsAndHeaders()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QFile tool(dir.path() + QStringLiteral("/yt-dlp"));
        QVERIFY(tool.open(QIODevice::WriteOnly));
        tool.write(
            "#!/bin/sh\n"
            "printf '%s' '{\"title\":\"A Post\",\"extractor_key\":\"Instagram\","
            "\"http_headers\":{\"Referer\":\"https://instagram.com/\"},"
            "\"formats\":[{\"format_id\":\"18\",\"format_note\":\"HD\",\"height\":720,"
            "\"ext\":\"mp4\",\"protocol\":\"m3u8_native\",\"vcodec\":\"h264\",\"acodec\":\"aac\","
            "\"url\":\"https://cdn.example.test/video.mp4\","
            "\"http_headers\":{\"User-Agent\":\"test-agent\"}}]}'\n");
        tool.close();
        QVERIFY(tool.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));

        qtidm::SocialMediaExtractor extractor(tool.fileName());
        QString error;
        const auto result = extractor.extract(
            QUrl(QStringLiteral("https://instagram.com/reel/example")), {}, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(result.title, QStringLiteral("A Post"));
        QCOMPARE(result.extractor, QStringLiteral("Instagram"));
        QCOMPARE(result.formats.size(), 1);
        QCOMPARE(result.formats[0].id, QStringLiteral("18"));
        QCOMPARE(result.formats[0].headers.value(QStringLiteral("Referer")).toString(),
            QStringLiteral("https://instagram.com/"));
        QCOMPARE(result.formats[0].headers.value(QStringLiteral("User-Agent")).toString(),
            QStringLiteral("test-agent"));
        QVERIFY(result.formats[0].adaptive);
    }

    void rejectsDrmMetadata()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QFile tool(dir.path() + QStringLiteral("/yt-dlp"));
        QVERIFY(tool.open(QIODevice::WriteOnly));
        tool.write("#!/bin/sh\nprintf '%s' '{\"title\":\"Protected\",\"is_drm\":true}'\n");
        tool.close();
        QVERIFY(tool.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));

        qtidm::SocialMediaExtractor extractor(tool.fileName());
        QString error;
        const auto result = extractor.extract(
            QUrl(QStringLiteral("https://facebook.com/watch/example")), {}, &error);
        QVERIFY(result.formats.isEmpty());
        QVERIFY(error.contains(QStringLiteral("DRM")));
    }

    void filtersPerFormatDrmMarkers()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QFile tool(dir.path() + QStringLiteral("/yt-dlp"));
        QVERIFY(tool.open(QIODevice::WriteOnly));
        tool.write(
            "#!/bin/sh\n"
            "printf '%s' '{\"title\":\"Mixed\", \"formats\":["
            "{\"format_id\":\"protected\",\"has_drm\":true,"
            "\"url\":\"https://cdn.example.test/protected.mpd\"},"
            "{\"format_id\":\"clear\",\"has_drm\":false,\"ext\":\"mp4\","
            "\"vcodec\":\"h264\",\"acodec\":\"aac\","
            "\"url\":\"https://cdn.example.test/clear.mp4\"}]}'\n");
        tool.close();
        QVERIFY(tool.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));

        qtidm::SocialMediaExtractor extractor(tool.fileName());
        QString error;
        const auto result = extractor.extract(
            QUrl(QStringLiteral("https://x.com/user/status/1")), {}, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(result.formats.size(), 1);
        QCOMPARE(result.formats[0].id, QStringLiteral("clear"));
    }
};

QTEST_MAIN(SocialMediaExtractorTest)
#include "SocialMediaExtractorTest.moc"
