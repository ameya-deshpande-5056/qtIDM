#include "core/DownloadTypes.h"

#include <QtTest/QtTest>

class DownloadTypesTest final : public QObject {
    Q_OBJECT
private slots:
    void statusRoundTrips()
    {
        const QList<qtidm::DownloadStatus> statuses {
            qtidm::DownloadStatus::Queued,
            qtidm::DownloadStatus::Connecting,
            qtidm::DownloadStatus::Downloading,
            qtidm::DownloadStatus::Paused,
            qtidm::DownloadStatus::Completed,
            qtidm::DownloadStatus::Failed,
            qtidm::DownloadStatus::Canceled
        };
        for (const auto status : statuses) {
            QCOMPARE(static_cast<int>(qtidm::downloadStatusFromStorage(qtidm::toStorageValue(status))), static_cast<int>(status));
        }
        QCOMPARE(static_cast<int>(qtidm::downloadStatusFromStorage(QStringLiteral("unknown"))), static_cast<int>(qtidm::DownloadStatus::Failed));
    }

    void requestJsonRoundTripsAllFields()
    {
        qtidm::DownloadRequest request;
        request.scheduleId = QStringLiteral("schedule-123");
        request.existingId = QStringLiteral("abc");
        request.url = QUrl(QStringLiteral("https://example.com/file.bin"));
        request.targetPath = QStringLiteral("/tmp/file.bin");
        request.category = QStringLiteral("Programs");
        request.queueName = QStringLiteral("Night");
        request.username = QStringLiteral("user");
        request.password = QStringLiteral("pass");
        request.proxyUrl = QStringLiteral("http://proxy:8080");
        request.httpMethod = QStringLiteral("POST");
        request.requestBody = QByteArray("report=weekly&format=csv");
        request.checksumAlgorithm = QStringLiteral("sha512");
        request.expectedChecksum = QStringLiteral("abcdef");
        request.completionCommand = QStringLiteral("/usr/bin/notify-send \"Finished file\"");
        request.completionMoveDirectory = QStringLiteral("/tmp/finished");
        request.archiveDestination = QStringLiteral("/tmp/extracted");
        request.scheduledAt = QDateTime::fromString(QStringLiteral("2026-07-17T01:02:03.004Z"), Qt::ISODateWithMs);
        request.windowStart = QTime(22, 0);
        request.windowEnd = QTime(6, 0);
        request.allowedWeekdays = 0x1f;
        request.headers.insert(QStringLiteral("Cookie"), QStringLiteral("a=b"));
        request.headers.insert(QStringLiteral("User-Agent"), QStringLiteral("qtIDM-test"));
        request.segments = 32;
        request.perHostConnectionLimit = 9;
        request.dynamicSegmentation = false;
        request.removeRecordOnCompletion = true;
        request.extractArchive = true;
        request.deleteArchiveAfterExtraction = true;
        request.maxRetries = 7;
        request.retryBaseDelayMs = 250;
        request.connectTimeoutSeconds = 45;
        request.lowSpeedTimeoutSeconds = 90;
        request.maximumRedirects = 12;
        request.allowPrivateNetworkRedirects = true;
        request.priority = 42;
        request.repeatIntervalSeconds = 3600;
        request.fileConflictPolicy = qtidm::FileConflictPolicy::Skip;
        request.speedLimitBytesPerSecond = 4096;
        request.sessionDataLimitBytes = 1024 * 1024;
        request.expectedTotalBytes = 123456;
        request.entityTag = QStringLiteral("\"fixture-etag\"");
        request.lastModified = QStringLiteral("Sun, 20 Jul 2026 12:00:00 GMT");
        request.cookies.append(qtidm::CookieRecord(
            QStringLiteral("session_token"), QStringLiteral("xyz123"),
            QStringLiteral(".example.com"), false, QStringLiteral("/app"),
            true, true, false, 1893456000));

        request.url = QUrl(QStringLiteral(
            "https://example.com/file.bin?"
            "response-content-disposition=attachment%3B%20filename%3DqtIDM.bin"));
        const auto json = qtidm::requestToJson(request);
        QCOMPARE(json.value(QStringLiteral("url")).toString(),
                 request.url.toString(QUrl::FullyEncoded));
        QVERIFY(!json.value(QStringLiteral("url")).toString().contains(QLatin1Char(' ')));
        const auto copy = qtidm::requestFromJson(json);

        QCOMPARE(copy.scheduleId, request.scheduleId);
        QCOMPARE(copy.existingId, request.existingId);
        QCOMPARE(copy.url, request.url);
        QCOMPARE(copy.targetPath, request.targetPath);
        QCOMPARE(copy.category, request.category);
        QCOMPARE(copy.queueName, request.queueName);
        QCOMPARE(copy.username, request.username);
        QCOMPARE(copy.password, request.password);
        QCOMPARE(copy.proxyUrl, request.proxyUrl);
        QCOMPARE(copy.httpMethod, QStringLiteral("POST"));
        QCOMPARE(copy.requestBody, request.requestBody);
        QCOMPARE(copy.checksumAlgorithm, request.checksumAlgorithm);
        QCOMPARE(copy.expectedChecksum, request.expectedChecksum);
        QCOMPARE(copy.completionCommand, request.completionCommand);
        QCOMPARE(copy.completionMoveDirectory, request.completionMoveDirectory);
        QCOMPARE(copy.archiveDestination, request.archiveDestination);
        QCOMPARE(copy.scheduledAt, request.scheduledAt);
        QCOMPARE(copy.windowStart, request.windowStart);
        QCOMPARE(copy.windowEnd, request.windowEnd);
        QCOMPARE(copy.allowedWeekdays, 0x1f);
        QCOMPARE(copy.headers.value(QStringLiteral("Cookie")).toString(), QStringLiteral("a=b"));
        QCOMPARE(copy.headers.value(QStringLiteral("User-Agent")).toString(), QStringLiteral("qtIDM-test"));
        QCOMPARE(copy.cookies.size(), 1);
        QCOMPARE(copy.cookies[0].name, QStringLiteral("session_token"));
        QCOMPARE(copy.cookies[0].value, QStringLiteral("xyz123"));
        QCOMPARE(copy.cookies[0].domain, QStringLiteral(".example.com"));
        QCOMPARE(copy.cookies[0].hostOnly, false);
        QCOMPARE(copy.cookies[0].path, QStringLiteral("/app"));
        QCOMPARE(copy.cookies[0].secure, true);
        QCOMPARE(copy.cookies[0].httpOnly, true);
        QCOMPARE(copy.cookies[0].session, false);
        QCOMPARE(copy.cookies[0].expirationDate, 1893456000);
        QCOMPARE(copy.segments, 32);
        QCOMPARE(copy.perHostConnectionLimit, 9);
        QCOMPARE(copy.dynamicSegmentation, false);
        QCOMPARE(copy.removeRecordOnCompletion, true);
        QCOMPARE(copy.extractArchive, true);
        QCOMPARE(copy.deleteArchiveAfterExtraction, true);
        QCOMPARE(copy.maxRetries, 7);
        QCOMPARE(copy.retryBaseDelayMs, 250);
        QCOMPARE(copy.connectTimeoutSeconds, 45);
        QCOMPARE(copy.lowSpeedTimeoutSeconds, 90);
        QCOMPARE(copy.maximumRedirects, 12);
        QCOMPARE(copy.allowPrivateNetworkRedirects, true);
        QCOMPARE(copy.priority, 42);
        QCOMPARE(copy.repeatIntervalSeconds, 3600);
        QCOMPARE(static_cast<int>(copy.fileConflictPolicy), static_cast<int>(qtidm::FileConflictPolicy::Skip));
        QCOMPARE(copy.speedLimitBytesPerSecond, qint64(4096));
        QCOMPARE(copy.sessionDataLimitBytes, qint64(1024 * 1024));
        QCOMPARE(copy.expectedTotalBytes, qint64(123456));
        QCOMPARE(copy.entityTag, QStringLiteral("\"fixture-etag\""));
        QCOMPARE(copy.lastModified, QStringLiteral("Sun, 20 Jul 2026 12:00:00 GMT"));
    }

    void vaultBackedPasswordIsNotSerialized()
    {
        qtidm::DownloadRequest request;
        request.url = QUrl(QStringLiteral("https://secure.example.test/file"));
        request.username = QStringLiteral("user");
        request.password = QStringLiteral("must-not-persist");
        request.credentialVaultKey = QStringLiteral("vault-key");

        const auto json = qtidm::requestToJson(request);
        QCOMPARE(json.value(QStringLiteral("password")).toString(), QString {});
        const auto copy = qtidm::requestFromJson(json);
        QCOMPARE(copy.credentialVaultKey, QStringLiteral("vault-key"));
        QVERIFY(copy.password.isEmpty());
    }

    void legacySha256MigratesToGenericChecksum()
    {
        QJsonObject legacy {
            { QStringLiteral("url"), QStringLiteral("https://example.com/file.bin") },
            { QStringLiteral("expectedSha256"), QString(64, QLatin1Char('a')) }
        };
        const auto request = qtidm::requestFromJson(legacy);
        QCOMPARE(request.checksumAlgorithm, QStringLiteral("sha256"));
        QCOMPARE(request.expectedChecksum, QString(64, QLatin1Char('a')));
    }

    void recordJsonRoundTripsSegments()
    {
        qtidm::DownloadRecord record;
        record.id = QStringLiteral("id");
        record.url = QUrl(QStringLiteral("https://example.com/a.zip"));
        record.targetPath = QStringLiteral("/tmp/a.zip");
        record.category = QStringLiteral("Compressed");
        record.totalBytes = 200;
        record.completedBytes = 150;
        record.status = qtidm::DownloadStatus::Paused;
        record.createdAt = QDateTime::fromString(QStringLiteral("2026-07-17T01:02:03.004Z"), Qt::ISODateWithMs);
        record.updatedAt = QDateTime::fromString(QStringLiteral("2026-07-17T02:02:03.004Z"), Qt::ISODateWithMs);
        record.segments = {
            { 0, 0, 99, 75, qtidm::DownloadStatus::Paused },
            { 1, 100, 199, 75, qtidm::DownloadStatus::Downloading }
        };
        record.request.url = record.url;
        record.request.targetPath = record.targetPath;
        record.request.proxyUrl = QStringLiteral("http://proxy:8080");

        const auto copy = qtidm::recordFromJson(qtidm::recordToJson(record));

        QCOMPARE(copy.id, record.id);
        QCOMPARE(copy.url, record.url);
        QCOMPARE(copy.totalBytes, qint64(200));
        QCOMPARE(copy.completedBytes, qint64(150));
        QCOMPARE(static_cast<int>(copy.status), static_cast<int>(qtidm::DownloadStatus::Paused));
        QCOMPARE(copy.segments.size(), 2);
        QCOMPARE(copy.segments[0].written, qint64(75));
        QCOMPARE(static_cast<int>(copy.segments[1].status), static_cast<int>(qtidm::DownloadStatus::Downloading));
        QCOMPARE(copy.request.proxyUrl, record.request.proxyUrl);
    }

    void mimeTypeMapsToCorrectExtension()
    {
        QCOMPARE(qtidm::extensionFromMime(QStringLiteral("audio/opus")), QStringLiteral("opus"));
        QCOMPARE(qtidm::extensionFromMime(QStringLiteral("audio/x-opus")), QStringLiteral("opus"));
        QCOMPARE(qtidm::extensionFromMime(QStringLiteral("audio/webm")), QStringLiteral("webm"));
        QCOMPARE(qtidm::extensionFromMime(QStringLiteral("audio/webm; codecs=\"opus\"")), QStringLiteral("webm"));
        QCOMPARE(qtidm::extensionFromMime(QStringLiteral("audio/x-m4a")), QStringLiteral("m4a"));
        QCOMPARE(qtidm::extensionFromMime(QStringLiteral("audio/mp4")), QStringLiteral("m4a"));
        QCOMPARE(qtidm::extensionFromMime(QStringLiteral("audio/aac")), QStringLiteral("aac"));
        QCOMPARE(qtidm::extensionFromMime(QStringLiteral("audio/flac")), QStringLiteral("flac"));
        QCOMPARE(qtidm::extensionFromMime(QStringLiteral("audio/ogg")), QStringLiteral("ogg"));
        QCOMPARE(qtidm::extensionFromMime(QStringLiteral("audio/mpeg")), QStringLiteral("mp3"));
        QCOMPARE(qtidm::extensionFromMime(QStringLiteral("video/webm")), QStringLiteral("webm"));
        QCOMPARE(qtidm::extensionFromMime(QStringLiteral("video/webm; codecs=\"vp9\"")), QStringLiteral("webm"));
        QCOMPARE(qtidm::extensionFromMime(QStringLiteral("video/mp4")), QStringLiteral("mp4"));
        QCOMPARE(qtidm::extensionFromMime(QStringLiteral("video/quicktime")), QStringLiteral("mov"));
        QCOMPARE(qtidm::extensionFromMime(QStringLiteral("video/x-matroska")), QStringLiteral("mkv"));
        QCOMPARE(qtidm::extensionFromMime(QStringLiteral("application/dash+xml")), QStringLiteral("mpd"));
        QCOMPARE(qtidm::extensionFromMime(QStringLiteral("application/vnd.apple.mpegurl")), QStringLiteral("m3u8"));
        QCOMPARE(qtidm::extensionFromMime(QStringLiteral("text/vtt")), QStringLiteral("vtt"));
    }

    void resolveDownloadFilenamePreservesAccurateFormat()
    {
        // 1. Audio stream with no URL extension resolves to Opus from MIME, not mp3
        const QUrl opusUrl(QStringLiteral("https://example.com/stream?id=audio-track"));
        const auto opusName = qtidm::resolveDownloadFilename(
            QStringLiteral("Podcast Episode 10"), opusUrl, QStringLiteral("Audio"), false, QStringLiteral("audio/opus"));
        QCOMPARE(opusName, QStringLiteral("Podcast Episode 10.opus"));

        // 2. WebM video with no URL extension resolves to .webm, not .mp4
        const QUrl webmUrl(QStringLiteral("https://example.com/videoplayback?expire=123"));
        const auto webmName = qtidm::resolveDownloadFilename(
            QStringLiteral("Tutorial Video"), webmUrl, QStringLiteral("Video"), false, QStringLiteral("video/webm; codecs=\"vp9\""));
        QCOMPARE(webmName, QStringLiteral("Tutorial Video.webm"));

        // 3. FLAC audio with authoritative URL path extension
        const QUrl flacUrl(QStringLiteral("https://example.com/music/masterpiece.flac"));
        const auto flacName = qtidm::resolveDownloadFilename(
            QStringLiteral("Symphony No 5"), flacUrl, QStringLiteral("Audio"), false, QStringLiteral("audio/flac"));
        QCOMPARE(flacName, QStringLiteral("Symphony No 5.flac"));

        // 4. Script URL (.php) with audio/ogg MIME resolves to .ogg, not .php or .mp3
        const QUrl phpUrl(QStringLiteral("https://example.com/get_audio.php?track=99"));
        const auto oggName = qtidm::resolveDownloadFilename(
            QStringLiteral("Interview"), phpUrl, QStringLiteral("Audio"), false, QStringLiteral("audio/ogg"));
        QCOMPARE(oggName, QStringLiteral("Interview.ogg"));

        // 5. Title had mismatched .mp4 but stream is audio/opus: corrects to .opus to prevent corruption
        const auto fixedOpus = qtidm::resolveDownloadFilename(
            QStringLiteral("Song.mp4"), opusUrl, QStringLiteral("Audio"), false, QStringLiteral("audio/opus"));
        QCOMPARE(fixedOpus, QStringLiteral("Song.opus"));

        // 6. Title had mismatched .mp4 but stream is video/webm: corrects to .webm
        const auto fixedWebm = qtidm::resolveDownloadFilename(
            QStringLiteral("Movie.mp4"), webmUrl, QStringLiteral("Video"), false, QStringLiteral("video/webm"));
        QCOMPARE(fixedWebm, QStringLiteral("Movie.webm"));

        // 7. Adaptive HLS/DASH always packages into universal container .mkv
        const QUrl dashUrl(QStringLiteral("https://example.com/manifest.mpd"));
        const auto dashName = qtidm::resolveDownloadFilename(
            QStringLiteral("Big Stream"), dashUrl, QStringLiteral("DASH"), true, QStringLiteral("application/dash+xml"));
        QCOMPARE(dashName, QStringLiteral("Big Stream.mkv"));
    }
};

QTEST_MAIN(DownloadTypesTest)
#include "DownloadTypesTest.moc"
