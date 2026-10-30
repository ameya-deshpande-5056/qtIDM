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

    void atomicallyPersistsSegmentSplits()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        qtidm::DownloadRepository repository;
        QVERIFY(repository.open(dir.path() + QStringLiteral("/split_test.sqlite3")));

        qtidm::DownloadRecord record;
        record.id = QStringLiteral("split-download");
        record.url = QUrl(QStringLiteral("https://example.com/large.bin"));
        record.targetPath = dir.path() + QStringLiteral("/large.bin");
        record.totalBytes = 10000;
        record.completedBytes = 2000;
        record.status = qtidm::DownloadStatus::Downloading;
        record.createdAt = QDateTime::currentDateTimeUtc();
        record.updatedAt = record.createdAt;
        record.segments = {
            { 0, 0, 4999, 2000, qtidm::DownloadStatus::Downloading },
            { 1, 5000, 9999, 0, qtidm::DownloadStatus::Queued }
        };

        QVERIFY(repository.upsertDownload(record));
        auto records = repository.listDownloads();
        QCOMPARE(records.first().segments.size(), 2);

        // Simulate dynamic split of segment 0 into [0, 3499] and [3500, 4999]
        // Vector expands from 2 segments to 3 segments:
        QVector<qtidm::SegmentInfo> splitSegments = {
            { 0, 0, 3499, 2000, qtidm::DownloadStatus::Downloading },
            { 1, 3500, 4999, 0, qtidm::DownloadStatus::Queued },
            { 2, 5000, 9999, 0, qtidm::DownloadStatus::Queued }
        };

        QVERIFY(repository.updateSegments(record.id, splitSegments));
        records = repository.listDownloads();
        QCOMPARE(records.first().segments.size(), 3);
        QCOMPARE(records.first().segments[0].end, 3499);
        QCOMPARE(records.first().segments[1].start, 3500);
        QCOMPARE(records.first().segments[1].end, 4999);
        QCOMPARE(records.first().segments[2].start, 5000);
        QCOMPARE(records.first().segments[2].end, 9999);
    }

    void rollbackPreservesPreviousSegmentsOnInsertFailure()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        qtidm::DownloadRepository repository;
        const auto dbPath = dir.path() + QStringLiteral("/rollback_test.sqlite3");
        QVERIFY(repository.open(dbPath));

        qtidm::DownloadRecord record;
        record.id = QStringLiteral("rollback-download");
        record.url = QUrl(QStringLiteral("https://example.com/data.bin"));
        record.targetPath = dir.path() + QStringLiteral("/data.bin");
        record.totalBytes = 20000;
        record.completedBytes = 5000;
        record.status = qtidm::DownloadStatus::Downloading;
        record.createdAt = QDateTime::currentDateTimeUtc();
        record.updatedAt = record.createdAt;
        record.segments = {
            { 0, 0, 9999, 5000, qtidm::DownloadStatus::Downloading },
            { 1, 10000, 19999, 0, qtidm::DownloadStatus::Queued }
        };

        QVERIFY(repository.upsertDownload(record));
        auto records = repository.listDownloads();
        QCOMPARE(records.size(), 1);
        QCOMPARE(records.first().segments.size(), 2);
        QCOMPARE(records.first().segments[0].written, 5000);

        // 1. Injected failure via duplicate segment_index (PRIMARY KEY constraint violation)
        QVector<qtidm::SegmentInfo> invalidSplitSegments = {
            { 0, 0, 4999, 5000, qtidm::DownloadStatus::Downloading },
            { 0, 5000, 9999, 0, qtidm::DownloadStatus::Queued }
        };

        QVERIFY(!repository.updateSegments(record.id, invalidSplitSegments));
        QVERIFY(repository.lastError().contains(QStringLiteral("UNIQUE constraint failed"), Qt::CaseInsensitive)
                || repository.lastError().contains(QStringLiteral("constraint"), Qt::CaseInsensitive));

        // Verify transaction rolled back completely: original 2 segments remain unmodified
        records = repository.listDownloads();
        QCOMPARE(records.first().segments.size(), 2);
        QCOMPARE(records.first().segments[0].start, 0);
        QCOMPARE(records.first().segments[0].end, 9999);
        QCOMPARE(records.first().segments[0].written, 5000);
        QCOMPARE(records.first().segments[1].start, 10000);
        QCOMPARE(records.first().segments[1].end, 19999);

        // 2. Injected statement failure via SQLite trigger (simulating disk/IO error)
        sqlite3* rawDb = nullptr;
        QCOMPARE(sqlite3_open(dbPath.toUtf8().constData(), &rawDb), SQLITE_OK);
        const char* triggerSql =
            "CREATE TRIGGER inject_io_error BEFORE INSERT ON segments "
            "WHEN NEW.segment_index = 2 "
            "BEGIN "
            "  SELECT RAISE(ABORT, 'injected disk I/O failure during split commit'); "
            "END;";
        QCOMPARE(sqlite3_exec(rawDb, triggerSql, nullptr, nullptr, nullptr), SQLITE_OK);
        sqlite3_close(rawDb);

        QVector<qtidm::SegmentInfo> threeSegments = {
            { 0, 0, 4999, 5000, qtidm::DownloadStatus::Downloading },
            { 1, 5000, 9999, 0, qtidm::DownloadStatus::Queued },
            { 2, 10000, 19999, 0, qtidm::DownloadStatus::Queued }
        };

        QVERIFY(!repository.updateSegments(record.id, threeSegments));
        QVERIFY(repository.lastError().contains(QStringLiteral("injected disk I/O failure")));

        // Verify rollback again: original 2 segments remain intact
        records = repository.listDownloads();
        QCOMPARE(records.first().segments.size(), 2);
        QCOMPARE(records.first().segments[0].start, 0);
        QCOMPARE(records.first().segments[0].end, 9999);
        QCOMPARE(records.first().segments[0].written, 5000);
        QCOMPARE(records.first().segments[1].start, 10000);
        QCOMPARE(records.first().segments[1].end, 19999);
    }

    void persistsDurableWrittenAndExecutesWalCheckpoint()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        qtidm::DownloadRepository repository;
        const auto dbPath = dir.path() + QStringLiteral("/durable_checkpoint.sqlite3");
        QVERIFY(repository.open(dbPath));

        // Verify PRAGMA synchronous is FULL (2) for crash durability
        {
            sqlite3* rawDb = nullptr;
            QCOMPARE(sqlite3_open(dbPath.toUtf8().constData(), &rawDb), SQLITE_OK);
            sqlite3_stmt* checkStmt = nullptr;
            QCOMPARE(sqlite3_prepare_v2(rawDb, "PRAGMA synchronous;", -1, &checkStmt, nullptr), SQLITE_OK);
            QCOMPARE(sqlite3_step(checkStmt), SQLITE_ROW);
            QCOMPARE(sqlite3_column_int(checkStmt, 0), 2); // 2 == FULL
            sqlite3_finalize(checkStmt);
            sqlite3_close(rawDb);
        }

        qtidm::DownloadRecord record;
        record.id = QStringLiteral("durable-ckpt");
        record.url = QUrl(QStringLiteral("https://example.com/data.bin"));
        record.targetPath = dir.path() + QStringLiteral("/data.bin");
        record.totalBytes = 20000;
        record.completedBytes = 8000;
        record.status = qtidm::DownloadStatus::Downloading;
        record.createdAt = QDateTime::currentDateTimeUtc();
        record.updatedAt = record.createdAt;
        record.segments = {
            qtidm::SegmentInfo(0, 0, 9999, 5000, 4096, qtidm::DownloadStatus::Downloading),
            qtidm::SegmentInfo(1, 10000, 19999, 3000, 2048, qtidm::DownloadStatus::Downloading)
        };

        QVERIFY(repository.upsertDownload(record));

        auto records = repository.listDownloads();
        QCOMPARE(records.size(), 1);
        QCOMPARE(records.first().segments.size(), 2);
        QCOMPARE(records.first().segments[0].written, 5000);
        QCOMPARE(records.first().segments[0].durableWritten, 4096);
        QCOMPARE(records.first().segments[1].written, 3000);
        QCOMPARE(records.first().segments[1].durableWritten, 2048);

        // Advance durable checkpoint
        record.segments[0].written = 8000;
        record.segments[0].durableWritten = 8000;
        record.segments[1].written = 6000;
        record.segments[1].durableWritten = 5000;
        QVERIFY(repository.checkpoint(record.id, record.segments));

        records = repository.listDownloads();
        QCOMPARE(records.first().segments[0].written, 8000);
        QCOMPARE(records.first().segments[0].durableWritten, 8000);
        QCOMPARE(records.first().segments[1].written, 6000);
        QCOMPARE(records.first().segments[1].durableWritten, 5000);
    }
};

QTEST_MAIN(DownloadRepositoryTest)
#include "DownloadRepositoryTest.moc"
