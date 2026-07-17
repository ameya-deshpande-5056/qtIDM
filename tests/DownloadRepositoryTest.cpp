#include "core/DownloadTypes.h"
#include "storage/DownloadRepository.h"

#include <QTemporaryDir>
#include <QtTest/QtTest>

class DownloadRepositoryTest final : public QObject {
    Q_OBJECT
private slots:
    void storesAndListsDownloads()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        qtidm::DownloadRepository repository;
        QVERIFY(repository.open(dir.path() + QStringLiteral("/downloads.sqlite3")));

        qtidm::DownloadRecord record;
        record.id = QStringLiteral("abc");
        record.url = QUrl(QStringLiteral("https://example.com/file.zip"));
        record.targetPath = dir.path() + QStringLiteral("/file.zip");
        record.category = QStringLiteral("Compressed");
        record.status = qtidm::DownloadStatus::Queued;
        record.createdAt = QDateTime::currentDateTimeUtc();
        record.updatedAt = record.createdAt;

        QVERIFY(repository.upsertDownload(record));
        const auto records = repository.listDownloads();
        QCOMPARE(records.size(), 1);
        QCOMPARE(records.first().id, QStringLiteral("abc"));
        QCOMPARE(records.first().category, QStringLiteral("Compressed"));
    }

    void persistsSegmentsProgressStatusAndRemoval()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        qtidm::DownloadRepository repository;
        QVERIFY(repository.open(dir.path() + QStringLiteral("/downloads.sqlite3")));

        qtidm::DownloadRecord record;
        record.id = QStringLiteral("segmented");
        record.url = QUrl(QStringLiteral("https://example.com/archive.zip"));
        record.targetPath = dir.path() + QStringLiteral("/archive.zip");
        record.category = QStringLiteral("Compressed");
        record.totalBytes = 300;
        record.completedBytes = 0;
        record.status = qtidm::DownloadStatus::Downloading;
        record.createdAt = QDateTime::currentDateTimeUtc();
        record.updatedAt = record.createdAt;
        record.segments = {
            { 0, 0, 99, 10, qtidm::DownloadStatus::Downloading },
            { 1, 100, 199, 0, qtidm::DownloadStatus::Queued },
            { 2, 200, 299, 100, qtidm::DownloadStatus::Completed }
        };

        QVERIFY(repository.upsertDownload(record));
        QVERIFY(repository.updateProgress(record.id, 110, 300, qtidm::DownloadStatus::Paused));
        record.segments[0].written = 50;
        record.segments[0].status = qtidm::DownloadStatus::Paused;
        QVERIFY(repository.updateSegments(record.id, record.segments));

        auto records = repository.listDownloads();
        QCOMPARE(records.size(), 1);
        QCOMPARE(records.first().completedBytes, 110);
        QCOMPARE(records.first().totalBytes, 300);
        QCOMPARE(static_cast<int>(records.first().status), static_cast<int>(qtidm::DownloadStatus::Paused));
        QCOMPARE(records.first().segments.size(), 3);
        QCOMPARE(records.first().segments[0].written, 50);
        QCOMPARE(static_cast<int>(records.first().segments[2].status), static_cast<int>(qtidm::DownloadStatus::Completed));

        QVERIFY(repository.updateStatus(record.id, qtidm::DownloadStatus::Canceled));
        records = repository.listDownloads();
        QCOMPARE(static_cast<int>(records.first().status), static_cast<int>(qtidm::DownloadStatus::Canceled));

        record.url = QUrl(QStringLiteral("https://mirror.example/archive.zip?refreshed=1"));
        record.category = QStringLiteral("Documents");
        record.status = qtidm::DownloadStatus::Paused;
        record.completedBytes = 110;
        QVERIFY(repository.upsertDownload(record));
        records = repository.listDownloads();
        QCOMPARE(records.first().url, record.url);
        QCOMPARE(records.first().category, QStringLiteral("Documents"));
        QCOMPARE(records.first().segments.size(), 3);

        QVERIFY(repository.removeDownload(record.id));
        QVERIFY(repository.listDownloads().isEmpty());
    }
};

QTEST_MAIN(DownloadRepositoryTest)
#include "DownloadRepositoryTest.moc"
