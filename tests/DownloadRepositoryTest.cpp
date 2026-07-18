#include "core/DownloadTypes.h"
#include "storage/DownloadRepository.h"

#include <QTemporaryDir>
#include <QtTest/QtTest>

class DownloadRepositoryTest final : public QObject {
    Q_OBJECT
private slots:
    void migratesLegacyDatabaseForRequestOptions()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.path() + QStringLiteral("/legacy.sqlite3");
        sqlite3* db = nullptr;
        QCOMPARE(sqlite3_open(path.toUtf8().constData(), &db), SQLITE_OK);
        const char* schema =
            "CREATE TABLE downloads ("
            "id TEXT PRIMARY KEY,url TEXT NOT NULL,target_path TEXT NOT NULL,category TEXT NOT NULL DEFAULT '',"
            "total_bytes INTEGER NOT NULL DEFAULT -1,completed_bytes INTEGER NOT NULL DEFAULT 0,"
            "status TEXT NOT NULL,created_at TEXT NOT NULL,updated_at TEXT NOT NULL);";
        QCOMPARE(sqlite3_exec(db, schema, nullptr, nullptr, nullptr), SQLITE_OK);
        sqlite3_close(db);

        qtidm::DownloadRepository repository;
        QVERIFY(repository.open(path));
        qtidm::DownloadRecord record;
        record.id = QStringLiteral("legacy-migrated");
        record.url = QUrl(QStringLiteral("https://example.com/file.bin"));
        record.targetPath = dir.path() + QStringLiteral("/file.bin");
        record.status = qtidm::DownloadStatus::Paused;
        record.createdAt = QDateTime::currentDateTimeUtc();
        record.updatedAt = record.createdAt;
        record.request.url = record.url;
        record.request.targetPath = record.targetPath;
        record.request.maxRetries = 17;
        QVERIFY(repository.upsertDownload(record));
        QCOMPARE(repository.listDownloads().first().request.maxRetries, 17);
    }

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
        record.request.url = record.url;
        record.request.targetPath = record.targetPath;
        record.request.category = record.category;
        record.request.proxyUrl = QStringLiteral("socks5://127.0.0.1:1080");
        record.request.headers.insert(QStringLiteral("Referer"), QStringLiteral("https://example.com/"));

        QVERIFY(repository.upsertDownload(record));
        const auto records = repository.listDownloads();
        QCOMPARE(records.size(), 1);
        QCOMPARE(records.first().id, QStringLiteral("abc"));
        QCOMPARE(records.first().category, QStringLiteral("Compressed"));
        QCOMPARE(records.first().request.proxyUrl, record.request.proxyUrl);
        QCOMPARE(records.first().request.headers.value(QStringLiteral("Referer")).toString(),
            QStringLiteral("https://example.com/"));
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
