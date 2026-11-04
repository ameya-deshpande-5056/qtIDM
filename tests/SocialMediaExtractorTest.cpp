#include "integration/SocialMediaExtractor.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>

class SocialMediaExtractorTest final : public QObject {
    Q_OBJECT
private slots:
    void recognizesSupportedSites()
    {
        QVERIFY(qtidm::SocialMediaExtractor::supports(
            QUrl(QStringLiteral("https://www.instagram.com/reel/example/"))));
        QVERIFY(qtidm::SocialMediaExtractor::supports(
            QUrl(QStringLiteral("https://mobile.twitter.com/user/status/1"))));
        QVERIFY(qtidm::SocialMediaExtractor::supports(
            QUrl(QStringLiteral("https://www.youtube.com/watch?v=example"))));
        QVERIFY(qtidm::SocialMediaExtractor::supports(
            QUrl(QStringLiteral("https://youtu.be/example"))));
        QVERIFY(qtidm::SocialMediaExtractor::supports(
            QUrl(QStringLiteral("https://www.bilibili.com/video/BV1xx411c7mD"))));
        QVERIFY(qtidm::SocialMediaExtractor::supports(
            QUrl(QStringLiteral("https://www.bilibili.com/en/video/BV1xx411c7mD"))));
        QVERIFY(qtidm::SocialMediaExtractor::supports(
            QUrl(QStringLiteral("https://b23.tv/example"))));
        QVERIFY(!qtidm::SocialMediaExtractor::supports(
            QUrl(QStringLiteral("https://unsupported.example.test/watch"))));

        QCOMPARE(qtidm::SocialMediaExtractor::normalizeUrl(
                     QUrl(QStringLiteral("https://www.bilibili.com/en/video/BV1QHhd6WEtU/?param=1"))),
                 QUrl(QStringLiteral("https://www.bilibili.com/video/BV1QHhd6WEtU/?param=1")));
        QCOMPARE(qtidm::SocialMediaExtractor::normalizeUrl(
                     QUrl(QStringLiteral("https://www.bilibili.com/video/BV1QHhd6WEtU/"))),
                 QUrl(QStringLiteral("https://www.bilibili.com/video/BV1QHhd6WEtU/")));
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

    void parsesSeparateVideoAndAudioFormatsAndFiltersStoryboards()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QFile tool(dir.path() + QStringLiteral("/yt-dlp"));
        QVERIFY(tool.open(QIODevice::WriteOnly));
        tool.write(
            "#!/bin/sh\n"
            "for arg in \"$@\"; do\n"
            "  if [ \"$arg\" = \"--format\" ]; then\n"
            "    seen_format=1\n"
            "  elif [ \"$seen_format\" = \"1\" ]; then\n"
            "    format_val=\"$arg\"\n"
            "    seen_format=0\n"
            "  fi\n"
            "done\n"
            "if [ \"$format_val\" != \"all\" ]; then\n"
            "  printf 'Expected --format all, got %s\\n' \"$format_val\" >&2\n"
            "  exit 1\n"
            "fi\n"
            "printf '%s' '{\"title\":\"Bilibili Video\",\"extractor_key\":\"BiliBili\","
            "\"http_headers\":{\"Referer\":\"https://www.bilibili.com/\"},"
            "\"formats\":["
            "{\"format_id\":\"30280\",\"ext\":\"m4a\",\"vcodec\":\"none\",\"acodec\":\"mp4a.40.2\","
            "\"url\":\"https://cdn.example.test/audio.m4a\"},"
            "{\"format_id\":\"30032\",\"height\":480,\"ext\":\"mp4\",\"vcodec\":\"avc1\",\"acodec\":\"none\","
            "\"url\":\"https://cdn.example.test/video.mp4\"},"
            "{\"format_id\":\"sb0\",\"format_note\":\"storyboard\",\"ext\":\"mhtml\",\"vcodec\":\"none\",\"acodec\":\"none\","
            "\"url\":\"https://cdn.example.test/storyboard.mhtml\"}]}'\n");
        tool.close();
        QVERIFY(tool.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));

        qtidm::SocialMediaExtractor extractor(tool.fileName());
        QString error;
        const auto result = extractor.extract(
            QUrl(QStringLiteral("https://www.bilibili.com/video/BV1qNnY66EUY")), {}, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(result.formats.size(), 3);
        QCOMPARE(result.formats[0].id, QStringLiteral("30032+30280"));
        QCOMPARE(result.formats[0].label, QStringLiteral("480p · MP4 · video + audio"));
        QVERIFY(result.formats[0].isCombined);
        QCOMPARE(result.formats[0].audioUrl, QUrl(QStringLiteral("https://cdn.example.test/audio.m4a")));
        QCOMPARE(result.formats[1].id, QStringLiteral("30280"));
        QCOMPARE(result.formats[1].label, QStringLiteral("M4A · audio only"));
        QVERIFY(!result.formats[1].isCombined);
        QCOMPARE(result.formats[2].id, QStringLiteral("30032"));
        QCOMPARE(result.formats[2].label, QStringLiteral("480p · MP4 · video only"));
        QVERIFY(!result.formats[2].isCombined);
    }

    void selectsLatestYtDlpBinaryAcrossCandidates()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        // Mock older apt package (e.g. 2024.04.09)
        QFile aptTool(dir.path() + QStringLiteral("/apt_ytdlp"));
        QVERIFY(aptTool.open(QIODevice::WriteOnly));
        aptTool.write("#!/bin/sh\nif [ \"$1\" = \"--version\" ]; then echo '2024.04.09'; exit 0; fi\nexit 1\n");
        aptTool.close();
        QVERIFY(aptTool.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));

        // Mock newer pip package (e.g. 2026.10.26)
        QFile pipTool(dir.path() + QStringLiteral("/pip_ytdlp"));
        QVERIFY(pipTool.open(QIODevice::WriteOnly));
        pipTool.write("#!/bin/sh\nif [ \"$1\" = \"--version\" ]; then echo '2026.10.26'; exit 0; fi\nexit 1\n");
        pipTool.close();
        QVERIFY(pipTool.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));

        QList<qtidm::SocialMediaExtractor::CommandInvocation> candidates {
            {aptTool.fileName(), {}},
            {pipTool.fileName(), {}}
        };

        const auto selected = qtidm::SocialMediaExtractor::selectLatestCommand(candidates);
        QCOMPARE(selected.program, pipTool.fileName());

        // Reverse order must also pick the latest pip candidate
        QList<qtidm::SocialMediaExtractor::CommandInvocation> reverseCandidates {
            {pipTool.fileName(), {}},
            {aptTool.fileName(), {}}
        };
        const auto selectedReverse = qtidm::SocialMediaExtractor::selectLatestCommand(reverseCandidates);
        QCOMPARE(selectedReverse.program, pipTool.fileName());
    }
};

QTEST_MAIN(SocialMediaExtractorTest)
#include "SocialMediaExtractorTest.moc"
