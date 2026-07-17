#include "storage/DownloadRepository.h"

#include <QDateTime>

namespace qtidm {

namespace {
bool execSql(sqlite3* db, const char* sql, QString* error)
{
    char* rawError = nullptr;
    const auto rc = sqlite3_exec(db, sql, nullptr, nullptr, &rawError);
    if (rc == SQLITE_OK) {
        return true;
    }
    if (error) {
        *error = rawError ? QString::fromUtf8(rawError) : QString::fromUtf8(sqlite3_errmsg(db));
    }
    sqlite3_free(rawError);
    return false;
}
}

DownloadRepository::~DownloadRepository()
{
    if (db_) {
        sqlite3_close(db_);
    }
}

bool DownloadRepository::open(const QString& databasePath)
{
    if (sqlite3_open(databasePath.toUtf8().constData(), &db_) != SQLITE_OK) {
        lastError_ = QString::fromUtf8(sqlite3_errmsg(db_));
        return false;
    }
    sqlite3_busy_timeout(db_, 5000);
    return migrate();
}

bool DownloadRepository::migrate()
{
    return execSql(db_, "PRAGMA journal_mode=WAL;", &lastError_)
        && execSql(db_, "PRAGMA synchronous=NORMAL;", &lastError_)
        && execSql(db_,
            "CREATE TABLE IF NOT EXISTS downloads ("
            "id TEXT PRIMARY KEY,"
            "url TEXT NOT NULL,"
            "target_path TEXT NOT NULL,"
            "category TEXT NOT NULL DEFAULT '',"
            "total_bytes INTEGER NOT NULL DEFAULT -1,"
            "completed_bytes INTEGER NOT NULL DEFAULT 0,"
            "status TEXT NOT NULL,"
            "created_at TEXT NOT NULL,"
            "updated_at TEXT NOT NULL"
            ");",
            &lastError_)
        && execSql(db_,
            "CREATE TABLE IF NOT EXISTS segments ("
            "download_id TEXT NOT NULL,"
            "segment_index INTEGER NOT NULL,"
            "range_start INTEGER NOT NULL,"
            "range_end INTEGER NOT NULL,"
            "written INTEGER NOT NULL DEFAULT 0,"
            "status TEXT NOT NULL,"
            "PRIMARY KEY(download_id, segment_index),"
            "FOREIGN KEY(download_id) REFERENCES downloads(id) ON DELETE CASCADE"
            ");",
            &lastError_);
}

bool DownloadRepository::upsertDownload(const DownloadRecord& record)
{
    if (!execSql(db_, "BEGIN IMMEDIATE;", &lastError_)) {
        return false;
    }
    constexpr auto sql =
        "INSERT INTO downloads(id,url,target_path,category,total_bytes,completed_bytes,status,created_at,updated_at) "
        "VALUES(?,?,?,?,?,?,?,?,?) "
        "ON CONFLICT(id) DO UPDATE SET "
        "url=excluded.url,target_path=excluded.target_path,category=excluded.category,total_bytes=excluded.total_bytes,"
        "completed_bytes=excluded.completed_bytes,status=excluded.status,updated_at=excluded.updated_at;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        lastError_ = QString::fromUtf8(sqlite3_errmsg(db_));
        return false;
    }
    sqlite3_bind_text(stmt, 1, record.id.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, record.url.toString().toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, record.targetPath.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, record.category.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 5, record.totalBytes);
    sqlite3_bind_int64(stmt, 6, record.completedBytes);
    sqlite3_bind_text(stmt, 7, toStorageValue(record.status).toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 8, record.createdAt.toUTC().toString(Qt::ISODateWithMs).toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 9, record.updatedAt.toUTC().toString(Qt::ISODateWithMs).toUtf8().constData(), -1, SQLITE_TRANSIENT);
    const auto rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        lastError_ = QString::fromUtf8(sqlite3_errmsg(db_));
        execSql(db_, "ROLLBACK;", nullptr);
        return false;
    }
    sqlite3_stmt* deleteStmt = nullptr;
    sqlite3_prepare_v2(db_, "DELETE FROM segments WHERE download_id=?;", -1, &deleteStmt, nullptr);
    sqlite3_bind_text(deleteStmt, 1, record.id.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_step(deleteStmt);
    sqlite3_finalize(deleteStmt);

    constexpr auto segmentSql =
        "INSERT INTO segments(download_id,segment_index,range_start,range_end,written,status) VALUES(?,?,?,?,?,?);";
    for (const auto& segment : record.segments) {
        sqlite3_stmt* segmentStmt = nullptr;
        if (sqlite3_prepare_v2(db_, segmentSql, -1, &segmentStmt, nullptr) != SQLITE_OK) {
            lastError_ = QString::fromUtf8(sqlite3_errmsg(db_));
            execSql(db_, "ROLLBACK;", nullptr);
            return false;
        }
        sqlite3_bind_text(segmentStmt, 1, record.id.toUtf8().constData(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(segmentStmt, 2, segment.index);
        sqlite3_bind_int64(segmentStmt, 3, segment.start);
        sqlite3_bind_int64(segmentStmt, 4, segment.end);
        sqlite3_bind_int64(segmentStmt, 5, segment.written);
        sqlite3_bind_text(segmentStmt, 6, toStorageValue(segment.status).toUtf8().constData(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(segmentStmt) != SQLITE_DONE) {
            lastError_ = QString::fromUtf8(sqlite3_errmsg(db_));
            sqlite3_finalize(segmentStmt);
            execSql(db_, "ROLLBACK;", nullptr);
            return false;
        }
        sqlite3_finalize(segmentStmt);
    }
    if (!execSql(db_, "COMMIT;", &lastError_)) {
        return false;
    }
    return true;
}

bool DownloadRepository::updateStatus(const QString& id, DownloadStatus status)
{
    constexpr auto sql = "UPDATE downloads SET status=?, updated_at=? WHERE id=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        lastError_ = QString::fromUtf8(sqlite3_errmsg(db_));
        return false;
    }
    sqlite3_bind_text(stmt, 1, toStorageValue(status).toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs).toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, id.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    const auto rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        lastError_ = QString::fromUtf8(sqlite3_errmsg(db_));
        return false;
    }
    return true;
}

bool DownloadRepository::updateProgress(const QString& id, qint64 completedBytes, qint64 totalBytes, DownloadStatus status)
{
    constexpr auto sql = "UPDATE downloads SET completed_bytes=?, total_bytes=?, status=?, updated_at=? WHERE id=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        lastError_ = QString::fromUtf8(sqlite3_errmsg(db_));
        return false;
    }
    sqlite3_bind_int64(stmt, 1, completedBytes);
    sqlite3_bind_int64(stmt, 2, totalBytes);
    sqlite3_bind_text(stmt, 3, toStorageValue(status).toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs).toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, id.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    const auto rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        lastError_ = QString::fromUtf8(sqlite3_errmsg(db_));
        return false;
    }
    return true;
}

bool DownloadRepository::updateSegments(const QString& id, const QVector<SegmentInfo>& segments)
{
    if (!execSql(db_, "BEGIN IMMEDIATE;", &lastError_)) {
        return false;
    }
    sqlite3_stmt* deleteStmt = nullptr;
    sqlite3_prepare_v2(db_, "DELETE FROM segments WHERE download_id=?;", -1, &deleteStmt, nullptr);
    sqlite3_bind_text(deleteStmt, 1, id.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_step(deleteStmt);
    sqlite3_finalize(deleteStmt);

    constexpr auto sql = "INSERT INTO segments(download_id,segment_index,range_start,range_end,written,status) VALUES(?,?,?,?,?,?);";
    for (const auto& segment : segments) {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            lastError_ = QString::fromUtf8(sqlite3_errmsg(db_));
            execSql(db_, "ROLLBACK;", nullptr);
            return false;
        }
        sqlite3_bind_text(stmt, 1, id.toUtf8().constData(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, segment.index);
        sqlite3_bind_int64(stmt, 3, segment.start);
        sqlite3_bind_int64(stmt, 4, segment.end);
        sqlite3_bind_int64(stmt, 5, segment.written);
        sqlite3_bind_text(stmt, 6, toStorageValue(segment.status).toUtf8().constData(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            lastError_ = QString::fromUtf8(sqlite3_errmsg(db_));
            sqlite3_finalize(stmt);
            execSql(db_, "ROLLBACK;", nullptr);
            return false;
        }
        sqlite3_finalize(stmt);
    }
    return execSql(db_, "COMMIT;", &lastError_);
}

bool DownloadRepository::removeDownload(const QString& id)
{
    if (!execSql(db_, "BEGIN IMMEDIATE;", &lastError_)) {
        return false;
    }
    sqlite3_stmt* segmentStmt = nullptr;
    sqlite3_prepare_v2(db_, "DELETE FROM segments WHERE download_id=?;", -1, &segmentStmt, nullptr);
    sqlite3_bind_text(segmentStmt, 1, id.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_step(segmentStmt);
    sqlite3_finalize(segmentStmt);

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_, "DELETE FROM downloads WHERE id=?;", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, id.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    const auto rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        lastError_ = QString::fromUtf8(sqlite3_errmsg(db_));
        execSql(db_, "ROLLBACK;", nullptr);
        return false;
    }
    return execSql(db_, "COMMIT;", &lastError_);
}

QList<DownloadRecord> DownloadRepository::listDownloads() const
{
    QList<DownloadRecord> records;
    constexpr auto sql = "SELECT id,url,target_path,category,total_bytes,completed_bytes,status,created_at,updated_at FROM downloads ORDER BY created_at DESC;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        lastError_ = QString::fromUtf8(sqlite3_errmsg(db_));
        return records;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        DownloadRecord record;
        record.id = QString::fromUtf8(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
        record.url = QUrl(QString::fromUtf8(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1))));
        record.targetPath = QString::fromUtf8(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)));
        record.category = QString::fromUtf8(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3)));
        record.totalBytes = sqlite3_column_int64(stmt, 4);
        record.completedBytes = sqlite3_column_int64(stmt, 5);
        record.status = downloadStatusFromStorage(QString::fromUtf8(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6))));
        record.createdAt = QDateTime::fromString(QString::fromUtf8(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7))), Qt::ISODateWithMs);
        record.updatedAt = QDateTime::fromString(QString::fromUtf8(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8))), Qt::ISODateWithMs);
        records.append(record);
    }
    sqlite3_finalize(stmt);
    constexpr auto segmentSql = "SELECT segment_index,range_start,range_end,written,status FROM segments WHERE download_id=? ORDER BY segment_index;";
    for (auto& record : records) {
        sqlite3_stmt* segmentStmt = nullptr;
        if (sqlite3_prepare_v2(db_, segmentSql, -1, &segmentStmt, nullptr) != SQLITE_OK) {
            lastError_ = QString::fromUtf8(sqlite3_errmsg(db_));
            return records;
        }
        sqlite3_bind_text(segmentStmt, 1, record.id.toUtf8().constData(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(segmentStmt) == SQLITE_ROW) {
            SegmentInfo segment;
            segment.index = sqlite3_column_int(segmentStmt, 0);
            segment.start = sqlite3_column_int64(segmentStmt, 1);
            segment.end = sqlite3_column_int64(segmentStmt, 2);
            segment.written = sqlite3_column_int64(segmentStmt, 3);
            segment.status = downloadStatusFromStorage(QString::fromUtf8(reinterpret_cast<const char*>(sqlite3_column_text(segmentStmt, 4))));
            record.segments.append(segment);
        }
        sqlite3_finalize(segmentStmt);
    }
    return records;
}

QString DownloadRepository::lastError() const
{
    return lastError_;
}

}
