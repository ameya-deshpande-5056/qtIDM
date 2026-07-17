#include "core/DownloadTypes.h"

#include <QJsonArray>
#include <QJsonValue>

namespace qtidm {

QString toStorageValue(DownloadStatus status)
{
    switch (status) {
    case DownloadStatus::Queued: return QStringLiteral("queued");
    case DownloadStatus::Connecting: return QStringLiteral("connecting");
    case DownloadStatus::Downloading: return QStringLiteral("downloading");
    case DownloadStatus::Paused: return QStringLiteral("paused");
    case DownloadStatus::Completed: return QStringLiteral("completed");
    case DownloadStatus::Failed: return QStringLiteral("failed");
    case DownloadStatus::Canceled: return QStringLiteral("canceled");
    }
    return QStringLiteral("failed");
}

DownloadStatus downloadStatusFromStorage(QStringView value)
{
    if (value == u"queued") return DownloadStatus::Queued;
    if (value == u"connecting") return DownloadStatus::Connecting;
    if (value == u"downloading") return DownloadStatus::Downloading;
    if (value == u"paused") return DownloadStatus::Paused;
    if (value == u"completed") return DownloadStatus::Completed;
    if (value == u"canceled") return DownloadStatus::Canceled;
    return DownloadStatus::Failed;
}

QJsonObject requestToJson(const DownloadRequest& request)
{
    QJsonObject headers;
    for (auto it = request.headers.cbegin(); it != request.headers.cend(); ++it) {
        headers.insert(it.key(), QJsonValue::fromVariant(it.value()));
    }
    return {
        { QStringLiteral("url"), request.url.toString() },
        { QStringLiteral("existingId"), request.existingId },
        { QStringLiteral("targetPath"), request.targetPath },
        { QStringLiteral("category"), request.category },
        { QStringLiteral("username"), request.username },
        { QStringLiteral("password"), request.password },
        { QStringLiteral("proxyUrl"), request.proxyUrl },
        { QStringLiteral("scheduledAt"), request.scheduledAt.toUTC().toString(Qt::ISODateWithMs) },
        { QStringLiteral("segments"), request.segments },
        { QStringLiteral("speedLimitBytesPerSecond"), QString::number(request.speedLimitBytesPerSecond) },
        { QStringLiteral("expectedTotalBytes"), QString::number(request.expectedTotalBytes) },
        { QStringLiteral("headers"), headers }
    };
}

DownloadRequest requestFromJson(const QJsonObject& object)
{
    DownloadRequest request;
    request.url = QUrl(object.value(QStringLiteral("url")).toString());
    request.existingId = object.value(QStringLiteral("existingId")).toString();
    request.targetPath = object.value(QStringLiteral("targetPath")).toString();
    request.category = object.value(QStringLiteral("category")).toString();
    request.username = object.value(QStringLiteral("username")).toString();
    request.password = object.value(QStringLiteral("password")).toString();
    request.proxyUrl = object.value(QStringLiteral("proxyUrl")).toString();
    request.scheduledAt = QDateTime::fromString(object.value(QStringLiteral("scheduledAt")).toString(), Qt::ISODateWithMs);
    request.segments = object.value(QStringLiteral("segments")).toInt(8);
    request.speedLimitBytesPerSecond = object.value(QStringLiteral("speedLimitBytesPerSecond")).toString().toLongLong();
    request.expectedTotalBytes = object.contains(QStringLiteral("expectedTotalBytes"))
        ? object.value(QStringLiteral("expectedTotalBytes")).toString().toLongLong()
        : -1;
    const auto headers = object.value(QStringLiteral("headers")).toObject();
    for (auto it = headers.begin(); it != headers.end(); ++it) {
        request.headers.insert(it.key(), it.value().toVariant());
    }
    return request;
}

QJsonObject recordToJson(const DownloadRecord& record)
{
    QJsonArray segments;
    for (const auto& segment : record.segments) {
        segments.append(QJsonObject {
            { QStringLiteral("index"), segment.index },
            { QStringLiteral("start"), QString::number(segment.start) },
            { QStringLiteral("end"), QString::number(segment.end) },
            { QStringLiteral("written"), QString::number(segment.written) },
            { QStringLiteral("status"), toStorageValue(segment.status) }
        });
    }
    return {
        { QStringLiteral("id"), record.id },
        { QStringLiteral("url"), record.url.toString() },
        { QStringLiteral("targetPath"), record.targetPath },
        { QStringLiteral("category"), record.category },
        { QStringLiteral("totalBytes"), QString::number(record.totalBytes) },
        { QStringLiteral("completedBytes"), QString::number(record.completedBytes) },
        { QStringLiteral("status"), toStorageValue(record.status) },
        { QStringLiteral("createdAt"), record.createdAt.toUTC().toString(Qt::ISODateWithMs) },
        { QStringLiteral("updatedAt"), record.updatedAt.toUTC().toString(Qt::ISODateWithMs) },
        { QStringLiteral("segments"), segments }
    };
}

DownloadRecord recordFromJson(const QJsonObject& object)
{
    DownloadRecord record;
    record.id = object.value(QStringLiteral("id")).toString();
    record.url = QUrl(object.value(QStringLiteral("url")).toString());
    record.targetPath = object.value(QStringLiteral("targetPath")).toString();
    record.category = object.value(QStringLiteral("category")).toString();
    record.totalBytes = object.value(QStringLiteral("totalBytes")).toString().toLongLong();
    record.completedBytes = object.value(QStringLiteral("completedBytes")).toString().toLongLong();
    record.status = downloadStatusFromStorage(object.value(QStringLiteral("status")).toString());
    record.createdAt = QDateTime::fromString(object.value(QStringLiteral("createdAt")).toString(), Qt::ISODateWithMs);
    record.updatedAt = QDateTime::fromString(object.value(QStringLiteral("updatedAt")).toString(), Qt::ISODateWithMs);
    const auto segments = object.value(QStringLiteral("segments")).toArray();
    for (const auto& value : segments) {
        const auto item = value.toObject();
        SegmentInfo segment;
        segment.index = item.value(QStringLiteral("index")).toInt();
        segment.start = item.value(QStringLiteral("start")).toString().toLongLong();
        segment.end = item.value(QStringLiteral("end")).toString().toLongLong();
        segment.written = item.value(QStringLiteral("written")).toString().toLongLong();
        segment.status = downloadStatusFromStorage(item.value(QStringLiteral("status")).toString());
        record.segments.append(segment);
    }
    return record;
}

}
