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
        request.existingId = QStringLiteral("abc");
        request.url = QUrl(QStringLiteral("https://example.com/file.bin"));
        request.targetPath = QStringLiteral("/tmp/file.bin");
        request.category = QStringLiteral("Programs");
        request.username = QStringLiteral("user");
        request.password = QStringLiteral("pass");
        request.proxyUrl = QStringLiteral("http://proxy:8080");
        request.scheduledAt = QDateTime::fromString(QStringLiteral("2026-07-17T01:02:03.004Z"), Qt::ISODateWithMs);
        request.headers.insert(QStringLiteral("Cookie"), QStringLiteral("a=b"));
        request.headers.insert(QStringLiteral("User-Agent"), QStringLiteral("qtIDM-test"));
        request.segments = 32;
        request.speedLimitBytesPerSecond = 4096;
        request.expectedTotalBytes = 123456;

        const auto copy = qtidm::requestFromJson(qtidm::requestToJson(request));

        QCOMPARE(copy.existingId, request.existingId);
        QCOMPARE(copy.url, request.url);
        QCOMPARE(copy.targetPath, request.targetPath);
        QCOMPARE(copy.category, request.category);
        QCOMPARE(copy.username, request.username);
        QCOMPARE(copy.password, request.password);
        QCOMPARE(copy.proxyUrl, request.proxyUrl);
        QCOMPARE(copy.scheduledAt, request.scheduledAt);
        QCOMPARE(copy.headers.value(QStringLiteral("Cookie")).toString(), QStringLiteral("a=b"));
        QCOMPARE(copy.headers.value(QStringLiteral("User-Agent")).toString(), QStringLiteral("qtIDM-test"));
        QCOMPARE(copy.segments, 32);
        QCOMPARE(copy.speedLimitBytesPerSecond, qint64(4096));
        QCOMPARE(copy.expectedTotalBytes, qint64(123456));
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

        const auto copy = qtidm::recordFromJson(qtidm::recordToJson(record));

        QCOMPARE(copy.id, record.id);
        QCOMPARE(copy.url, record.url);
        QCOMPARE(copy.totalBytes, qint64(200));
        QCOMPARE(copy.completedBytes, qint64(150));
        QCOMPARE(static_cast<int>(copy.status), static_cast<int>(qtidm::DownloadStatus::Paused));
        QCOMPARE(copy.segments.size(), 2);
        QCOMPARE(copy.segments[0].written, qint64(75));
        QCOMPARE(static_cast<int>(copy.segments[1].status), static_cast<int>(qtidm::DownloadStatus::Downloading));
    }
};

QTEST_MAIN(DownloadTypesTest)
#include "DownloadTypesTest.moc"
