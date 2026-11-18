#include "storage/DownloadRepository.h"

#include <QDateTime>
#include <QJsonDocument>

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

bool columnExists(sqlite3* db, const char* table, const char* column)
{
    const auto sql = QByteArray("PRAGMA table_info(") + table + ");";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.constData(), -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    bool found = false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const auto* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        if (name && QByteArray(name) == column) {
            found = true;
            break;
        }
    }
    sqlite3_finalize(stmt);
    return found;
}
}

DownloadRepository::~DownloadRepository()
{
    QMutexLocker lock(&dbMutex_);
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool DownloadRepository::open(const QString& databasePath)
{
    QMutexLocker lock(&dbMutex_);
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
    if (sqlite3_open(databasePath.toUtf8().constData(), &db_) != SQLITE_OK) {
        lastError_ = QString::fromUtf8(sqlite3_errmsg(db_));
        return false;
    }
    sqlite3_busy_timeout(db_, 5000);
    return migrate();
}

bool DownloadRepository::migrate()
{
    QMutexLocker lock(&dbMutex_);
    const bool created = execSql(db_, "PRAGMA journal_mode=WAL;", &lastError_)
        && execSql(db_, "PRAGMA synchronous=FULL;", &lastError_)
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
            "updated_at TEXT NOT NULL,"
            "request_json TEXT NOT NULL DEFAULT '{}'"
            ");",
            &lastError_)
        && execSql(db_,
            "CREATE TABLE IF NOT EXISTS segments ("
            "download_id TEXT NOT NULL,"
            "segment_index INTEGER NOT NULL,"
            "range_start INTEGER NOT NULL,"
            "range_end INTEGER NOT NULL,"
            "written INTEGER NOT NULL DEFAULT 0,"
            "durable_written INTEGER NOT NULL DEFAULT 0,"
            "status TEXT NOT NULL,"
            "PRIMARY KEY(download_id, segment_index),"
            "FOREIGN KEY(download_id) REFERENCES downloads(id) ON DELETE CASCADE"
            ");",
            &lastError_)
        && execSql(db_,
            "CREATE TABLE IF NOT EXISTS domain_rules ("
            "domain TEXT PRIMARY KEY,"
            "include_subdomains INTEGER NOT NULL DEFAULT 1,"
            "max_segments INTEGER NOT NULL DEFAULT 0,"
            "max_concurrent_downloads INTEGER NOT NULL DEFAULT 0,"
            "speed_limit_bps INTEGER NOT NULL DEFAULT 0,"
            "custom_user_agent TEXT NOT NULL DEFAULT '',"
            "custom_headers_json TEXT NOT NULL DEFAULT '{}'"
            ");",
            &lastError_);
    if (!created) {
        return false;
    }
    if (!columnExists(db_, "downloads", "request_json")
        && !execSql(db_, "ALTER TABLE downloads ADD COLUMN request_json TEXT NOT NULL DEFAULT '{}';", &lastError_)) {
        return false;
    }
    if (!columnExists(db_, "segments", "durable_written")
        && !execSql(db_, "ALTER TABLE segments ADD COLUMN durable_written INTEGER NOT NULL DEFAULT 0;", &lastError_)) {
        return false;
    }
    return true;
}

bool DownloadRepository::upsertDownload(const DownloadRecord& record)
{
    QMutexLocker lock(&dbMutex_);
    if (!execSql(db_, "BEGIN IMMEDIATE;", &lastError_)) {
        return false;
    }
    constexpr auto sql =
        "INSERT INTO downloads(id,url,target_path,category,total_bytes,completed_bytes,status,created_at,updated_at,request_json) "
        "VALUES(?,?,?,?,?,?,?,?,?,?) "
        "ON CONFLICT(id) DO UPDATE SET "
        "url=excluded.url,target_path=excluded.target_path,category=excluded.category,total_bytes=excluded.total_bytes,"
        "completed_bytes=excluded.completed_bytes,status=excluded.status,updated_at=excluded.updated_at,"
        "request_json=excluded.request_json;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        lastError_ = QString::fromUtf8(sqlite3_errmsg(db_));
        execSql(db_, "ROLLBACK;", nullptr);
        return false;
    }
    sqlite3_bind_text(stmt, 1, record.id.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    const auto encodedUrl = record.url.toString(QUrl::FullyEncoded).toUtf8();
    sqlite3_bind_text(stmt, 2, encodedUrl.constData(), encodedUrl.size(), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, record.targetPath.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, record.category.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 5, record.totalBytes);
    sqlite3_bind_int64(stmt, 6, record.completedBytes);
    sqlite3_bind_text(stmt, 7, toStorageValue(record.status).toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 8, record.createdAt.toUTC().toString(Qt::ISODateWithMs).toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 9, record.updatedAt.toUTC().toString(Qt::ISODateWithMs).toUtf8().constData(), -1, SQLITE_TRANSIENT);
    const auto requestJson = QJsonDocument(requestToJson(record.request)).toJson(QJsonDocument::Compact);
    sqlite3_bind_text(stmt, 10, requestJson.constData(), requestJson.size(), SQLITE_TRANSIENT);
    const auto rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        lastError_ = QString::fromUtf8(sqlite3_errmsg(db_));
        execSql(db_, "ROLLBACK;", nullptr);
        return false;
    }
    sqlite3_stmt* deleteStmt = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM segments WHERE download_id=?;", -1, &deleteStmt, nullptr) != SQLITE_OK) {
        lastError_ = QString::fromUtf8(sqlite3_errmsg(db_));
        execSql(db_, "ROLLBACK;", nullptr);
        return false;
    }
    sqlite3_bind_text(deleteStmt, 1, record.id.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(deleteStmt) != SQLITE_DONE) {
        lastError_ = QString::fromUtf8(sqlite3_errmsg(db_));
        sqlite3_finalize(deleteStmt);
        execSql(db_, "ROLLBACK;", nullptr);
        return false;
    }
    sqlite3_finalize(deleteStmt);

    constexpr auto segmentSql =
        "INSERT INTO segments(download_id,segment_index,range_start,range_end,written,durable_written,status) VALUES(?,?,?,?,?,?,?);";
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
        sqlite3_bind_int64(segmentStmt, 6, segment.durableWritten);
        sqlite3_bind_text(segmentStmt, 7, toStorageValue(segment.status).toUtf8().constData(), -1, SQLITE_TRANSIENT);
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
    QMutexLocker lock(&dbMutex_);
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
    QMutexLocker lock(&dbMutex_);
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
    QMutexLocker lock(&dbMutex_);
    if (!execSql(db_, "BEGIN IMMEDIATE;", &lastError_)) {
        return false;
    }
    sqlite3_stmt* deleteStmt = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM segments WHERE download_id=?;", -1, &deleteStmt, nullptr) != SQLITE_OK) {
        lastError_ = QString::fromUtf8(sqlite3_errmsg(db_));
        execSql(db_, "ROLLBACK;", nullptr);
        return false;
    }
    sqlite3_bind_text(deleteStmt, 1, id.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(deleteStmt) != SQLITE_DONE) {
        lastError_ = QString::fromUtf8(sqlite3_errmsg(db_));
        sqlite3_finalize(deleteStmt);
        execSql(db_, "ROLLBACK;", nullptr);
        return false;
    }
    sqlite3_finalize(deleteStmt);

    constexpr auto sql = "INSERT INTO segments(download_id,segment_index,range_start,range_end,written,durable_written,status) VALUES(?,?,?,?,?,?,?);";
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
        sqlite3_bind_int64(stmt, 6, segment.durableWritten);
        sqlite3_bind_text(stmt, 7, toStorageValue(segment.status).toUtf8().constData(), -1, SQLITE_TRANSIENT);
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

bool DownloadRepository::checkpoint(const QString& id, const QVector<SegmentInfo>& segments)
{
    QMutexLocker lock(&dbMutex_);
    if (!updateSegments(id, segments)) {
        return false;
    }
    int logSize = 0;
    int checkpointedFrames = 0;
    const int rc = sqlite3_wal_checkpoint_v2(db_, nullptr, SQLITE_CHECKPOINT_FULL, &logSize, &checkpointedFrames);
    if (rc != SQLITE_OK && rc != SQLITE_BUSY) {
        lastError_ = QString::fromUtf8(sqlite3_errmsg(db_));
        return false;
    }
    return true;
}

bool DownloadRepository::removeDownload(const QString& id)
{
    QMutexLocker lock(&dbMutex_);
    if (!execSql(db_, "BEGIN IMMEDIATE;", &lastError_)) {
        return false;
    }
    sqlite3_stmt* segmentStmt = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM segments WHERE download_id=?;", -1, &segmentStmt, nullptr) != SQLITE_OK) {
        lastError_ = QString::fromUtf8(sqlite3_errmsg(db_));
        execSql(db_, "ROLLBACK;", nullptr);
        return false;
    }
    sqlite3_bind_text(segmentStmt, 1, id.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(segmentStmt) != SQLITE_DONE) {
        lastError_ = QString::fromUtf8(sqlite3_errmsg(db_));
        sqlite3_finalize(segmentStmt);
        execSql(db_, "ROLLBACK;", nullptr);
        return false;
    }
    sqlite3_finalize(segmentStmt);

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM downloads WHERE id=?;", -1, &stmt, nullptr) != SQLITE_OK) {
        lastError_ = QString::fromUtf8(sqlite3_errmsg(db_));
        execSql(db_, "ROLLBACK;", nullptr);
        return false;
    }
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
    QMutexLocker lock(&dbMutex_);
    QList<DownloadRecord> records;
    constexpr auto sql = "SELECT id,url,target_path,category,total_bytes,completed_bytes,status,created_at,updated_at,request_json FROM downloads ORDER BY created_at DESC;";
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
        const auto requestJson = QByteArray(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9)));
        record.request = requestFromJson(QJsonDocument::fromJson(requestJson).object());
        if (!record.request.url.isValid()) {
            record.request.url = record.url;
            record.request.targetPath = record.targetPath;
            record.request.category = record.category;
        }
        records.append(record);
    }
    sqlite3_finalize(stmt);
    constexpr auto segmentSql = "SELECT segment_index,range_start,range_end,written,durable_written,status FROM segments WHERE download_id=? ORDER BY segment_index;";
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
            segment.durableWritten = sqlite3_column_int64(segmentStmt, 4);
            segment.status = downloadStatusFromStorage(QString::fromUtf8(reinterpret_cast<const char*>(sqlite3_column_text(segmentStmt, 5))));
            record.segments.append(segment);
        }
        sqlite3_finalize(segmentStmt);
    }
    return records;
}

bool DownloadRepository::upsertDomainRule(const DomainRule& rule)
{
    QMutexLocker lock(&dbMutex_);
    const QString normDomain = DomainRule::normalizeDomain(rule.domain);
    if (normDomain.isEmpty()) {
        lastError_ = QStringLiteral("domain rule requires a non-empty domain");
        return false;
    }
    constexpr auto sql =
        "INSERT INTO domain_rules(domain,include_subdomains,max_segments,max_concurrent_downloads,speed_limit_bps,custom_user_agent,custom_headers_json) "
        "VALUES(?,?,?,?,?,?,?) "
        "ON CONFLICT(domain) DO UPDATE SET "
        "include_subdomains=excluded.include_subdomains,"
        "max_segments=excluded.max_segments,"
        "max_concurrent_downloads=excluded.max_concurrent_downloads,"
        "speed_limit_bps=excluded.speed_limit_bps,"
        "custom_user_agent=excluded.custom_user_agent,"
        "custom_headers_json=excluded.custom_headers_json;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        lastError_ = QString::fromUtf8(sqlite3_errmsg(db_));
        return false;
    }
    QJsonObject headersObj;
    for (auto it = rule.customHeaders.cbegin(); it != rule.customHeaders.cend(); ++it) {
        headersObj.insert(it.key(), QJsonValue::fromVariant(it.value()));
    }
    const QByteArray domainUtf8 = normDomain.toUtf8();
    const QByteArray uaUtf8 = rule.customUserAgent.trimmed().toUtf8();
    const QByteArray headersJson = QJsonDocument(headersObj).toJson(QJsonDocument::Compact);

    sqlite3_bind_text(stmt, 1, domainUtf8.constData(), domainUtf8.size(), SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, rule.includeSubdomains ? 1 : 0);
    sqlite3_bind_int(stmt, 3, qBound(0, rule.maxSegments, 32));
    sqlite3_bind_int(stmt, 4, qMax(0, rule.maxConcurrentDownloads));
    sqlite3_bind_int64(stmt, 5, qMax<qint64>(0, rule.speedLimitBytesPerSecond));
    sqlite3_bind_text(stmt, 6, uaUtf8.constData(), uaUtf8.size(), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, headersJson.constData(), headersJson.size(), SQLITE_TRANSIENT);
    const auto rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        lastError_ = QString::fromUtf8(sqlite3_errmsg(db_));
        return false;
    }
    return true;
}

bool DownloadRepository::removeDomainRule(const QString& domain)
{
    QMutexLocker lock(&dbMutex_);
    const QString normDomain = DomainRule::normalizeDomain(domain);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM domain_rules WHERE domain=?;", -1, &stmt, nullptr) != SQLITE_OK) {
        lastError_ = QString::fromUtf8(sqlite3_errmsg(db_));
        return false;
    }
    const QByteArray domainUtf8 = normDomain.toUtf8();
    sqlite3_bind_text(stmt, 1, domainUtf8.constData(), domainUtf8.size(), SQLITE_TRANSIENT);
    const auto rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        lastError_ = QString::fromUtf8(sqlite3_errmsg(db_));
        return false;
    }
    return true;
}

QVector<DomainRule> DownloadRepository::listDomainRules() const
{
    QMutexLocker lock(&dbMutex_);
    QVector<DomainRule> rules;
    constexpr auto sql =
        "SELECT domain,include_subdomains,max_segments,max_concurrent_downloads,speed_limit_bps,custom_user_agent,custom_headers_json "
        "FROM domain_rules ORDER BY domain ASC;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        lastError_ = QString::fromUtf8(sqlite3_errmsg(db_));
        return rules;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        DomainRule rule;
        rule.domain = QString::fromUtf8(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
        rule.includeSubdomains = sqlite3_column_int(stmt, 1) != 0;
        rule.maxSegments = qBound(0, sqlite3_column_int(stmt, 2), 32);
        rule.maxConcurrentDownloads = qMax(0, sqlite3_column_int(stmt, 3));
        rule.speedLimitBytesPerSecond = qMax<qint64>(0, sqlite3_column_int64(stmt, 4));
        const auto* rawUa = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        rule.customUserAgent = rawUa ? QString::fromUtf8(rawUa) : QString {};
        const auto* rawHeaders = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        if (rawHeaders) {
            const auto headersObj = QJsonDocument::fromJson(QByteArray(rawHeaders)).object();
            for (auto it = headersObj.begin(); it != headersObj.end(); ++it) {
                rule.customHeaders.insert(it.key(), it.value().toVariant());
            }
        }
        rules.append(std::move(rule));
    }
    sqlite3_finalize(stmt);
    return rules;
}

std::optional<DomainRule> DownloadRepository::matchDomainRule(const QString& host) const
{
    const auto rules = listDomainRules();
    const auto* best = DomainRule::findBestMatch(rules, host);
    return best ? std::optional<DomainRule>(*best) : std::nullopt;
}

QString DownloadRepository::lastError() const
{
    QMutexLocker lock(&dbMutex_);
    return lastError_;
}

}
