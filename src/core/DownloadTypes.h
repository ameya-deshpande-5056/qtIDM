#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QString>
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

struct SegmentInfo {
    int index = 0;
    std::int64_t start = 0;
    std::int64_t end = 0;
    std::int64_t written = 0;
    DownloadStatus status = DownloadStatus::Queued;
};

struct DownloadRequest {
    QString existingId;
    QUrl url;
    QString targetPath;
    QString category;
    QString username;
    QString password;
    QString proxyUrl;
    QDateTime scheduledAt;
    QVariantMap headers;
    int segments = 8;
    qint64 speedLimitBytesPerSecond = 0;
    qint64 expectedTotalBytes = -1;
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
};

QString toStorageValue(DownloadStatus status);
DownloadStatus downloadStatusFromStorage(QStringView value);
QJsonObject requestToJson(const DownloadRequest& request);
DownloadRequest requestFromJson(const QJsonObject& object);
QJsonObject recordToJson(const DownloadRecord& record);
DownloadRecord recordFromJson(const QJsonObject& object);

}

Q_DECLARE_METATYPE(qtidm::DownloadStatus)
Q_DECLARE_METATYPE(qtidm::DownloadRecord)
Q_DECLARE_METATYPE(qtidm::SegmentInfo)
Q_DECLARE_METATYPE(QVector<qtidm::SegmentInfo>)
