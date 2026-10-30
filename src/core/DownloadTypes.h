#pragma once

#include <QDateTime>
#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QTime>
#include <QUrl>
#include <QVariantMap>
#include <QVector>
#include <cstdint>

namespace qtidm {

enum class DownloadStatus {
    Queued,
    Connecting,
    Downloading,
    Paused,
    Completed,
    Failed,
    Canceled
};

enum class FileConflictPolicy {
    AutoRename,
    Overwrite,
    Skip
};

struct SegmentInfo {
    int index = 0;
    std::int64_t start = 0;
    std::int64_t end = 0;
    std::int64_t written = 0;
    std::int64_t durableWritten = 0;
    DownloadStatus status = DownloadStatus::Queued;

    SegmentInfo() = default;
    SegmentInfo(int idx, std::int64_t st, std::int64_t en, std::int64_t wr, DownloadStatus stt)
        : index(idx), start(st), end(en), written(wr), durableWritten(wr), status(stt) {}
    SegmentInfo(int idx, std::int64_t st, std::int64_t en, std::int64_t wr, std::int64_t dwr, DownloadStatus stt)
        : index(idx), start(st), end(en), written(wr), durableWritten(dwr), status(stt) {}
};

struct CookieRecord {
    QString name;
    QString value;
    QString domain;
    bool hostOnly = false;
    QString path = QStringLiteral("/");
    bool secure = false;
    bool httpOnly = false;
    bool session = false;
    qint64 expirationDate = 0; // 0 denotes session cookie or no expiration

    CookieRecord() = default;
    CookieRecord(QString n, QString v, QString d, bool ho = false, QString p = QStringLiteral("/"),
                 bool sec = false, bool ho_only = false, bool sess = false, qint64 exp = 0)
        : name(std::move(n)), value(std::move(v)), domain(std::move(d)), hostOnly(ho),
          path(std::move(p)), secure(sec), httpOnly(ho_only), session(sess), expirationDate(exp) {}
};

struct DownloadRequest {
    QString scheduleId;
    QString existingId;
    QUrl url;
    QString targetPath;
    QString category;
    QString queueName = QStringLiteral("Main");
    QString username;
    QString password;
    QString proxyUrl;
    QString checksumAlgorithm = QStringLiteral("sha256");
    QString expectedChecksum;
    // Kept for compatibility with queue files created before generic checksums.
    QString expectedSha256;
    QString completionCommand;
    QString completionMoveDirectory;
    QString archiveDestination;
    QString credentialVaultKey;
    // Browser-originated POST downloads need their method and body preserved.
    // Empty requestBody always denotes the normal GET transfer.
    QString httpMethod;
    QByteArray requestBody;
    QDateTime scheduledAt;
    QTime windowStart;
    QTime windowEnd;
    int allowedWeekdays = 0x7f;
    QVariantMap headers;
    int segments = 8;
    int perHostConnectionLimit = 16;
    bool dynamicSegmentation = true;
    bool removeRecordOnCompletion = false;
    bool extractArchive = false;
    bool deleteArchiveAfterExtraction = false;
    int maxRetries = 5;
    int retryBaseDelayMs = 500;
    int connectTimeoutSeconds = 15;
    int lowSpeedTimeoutSeconds = 60;
    int maximumRedirects = 20;
    bool allowPrivateNetworkRedirects = false;
    bool insecureSkipSslVerification = false;
    QString caCertificatePath;
    QString pinnedIp;
    int priority = 0;
    int repeatIntervalSeconds = 0;
    FileConflictPolicy fileConflictPolicy = FileConflictPolicy::AutoRename;
    qint64 speedLimitBytesPerSecond = 0;
    qint64 sessionDataLimitBytes = 0;
    qint64 expectedTotalBytes = -1;
    QString entityTag;
    QString lastModified;
    QStringList serverCookies;
    QVector<CookieRecord> cookies;
    QVector<SegmentInfo> resumeSegments;
};

struct DownloadRecord {
    QString id;
    QUrl url;
    QString targetPath;
    QString category;
    std::int64_t totalBytes = -1;
    std::int64_t completedBytes = 0;
    DownloadStatus status = DownloadStatus::Queued;
    QDateTime createdAt;
    QDateTime updatedAt;
    QVector<SegmentInfo> segments;
    DownloadRequest request;
};

QString toStorageValue(DownloadStatus status);
DownloadStatus downloadStatusFromStorage(QStringView value);
QString toStorageValue(FileConflictPolicy policy);
FileConflictPolicy fileConflictPolicyFromStorage(QStringView value);
QJsonObject requestToJson(const DownloadRequest& request);
DownloadRequest requestFromJson(const QJsonObject& object);
QJsonObject recordToJson(const DownloadRecord& record);
DownloadRecord recordFromJson(const QJsonObject& object);

}

Q_DECLARE_METATYPE(qtidm::DownloadStatus)
Q_DECLARE_METATYPE(qtidm::DownloadRequest)
Q_DECLARE_METATYPE(qtidm::DownloadRecord)
Q_DECLARE_METATYPE(qtidm::SegmentInfo)
Q_DECLARE_METATYPE(QVector<qtidm::SegmentInfo>)
Q_DECLARE_METATYPE(qtidm::CookieRecord)
Q_DECLARE_METATYPE(QVector<qtidm::CookieRecord>)
