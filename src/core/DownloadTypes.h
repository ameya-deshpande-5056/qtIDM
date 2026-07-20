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
    DownloadStatus status = DownloadStatus::Queued;
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
    int priority = 0;
    int repeatIntervalSeconds = 0;
    FileConflictPolicy fileConflictPolicy = FileConflictPolicy::AutoRename;
    qint64 speedLimitBytesPerSecond = 0;
    qint64 sessionDataLimitBytes = 0;
    qint64 expectedTotalBytes = -1;
    QString entityTag;
    QString lastModified;
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
