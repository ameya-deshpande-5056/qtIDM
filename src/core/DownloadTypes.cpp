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

QString toStorageValue(FileConflictPolicy policy)
{
    switch (policy) {
    case FileConflictPolicy::AutoRename: return QStringLiteral("auto-rename");
    case FileConflictPolicy::Overwrite: return QStringLiteral("overwrite");
    case FileConflictPolicy::Skip: return QStringLiteral("skip");
    }
    return QStringLiteral("auto-rename");
}

FileConflictPolicy fileConflictPolicyFromStorage(QStringView value)
{
    if (value == u"overwrite") return FileConflictPolicy::Overwrite;
    if (value == u"skip") return FileConflictPolicy::Skip;
    return FileConflictPolicy::AutoRename;
}

QJsonObject requestToJson(const DownloadRequest& request)
{
    QJsonObject headers;
    for (auto it = request.headers.cbegin(); it != request.headers.cend(); ++it) {
        headers.insert(it.key(), QJsonValue::fromVariant(it.value()));
    }
    return {
        { QStringLiteral("url"), request.url.toString() },
        { QStringLiteral("scheduleId"), request.scheduleId },
        { QStringLiteral("existingId"), request.existingId },
        { QStringLiteral("targetPath"), request.targetPath },
        { QStringLiteral("category"), request.category },
        { QStringLiteral("queueName"), request.queueName },
        { QStringLiteral("username"), request.username },
        { QStringLiteral("password"), request.credentialVaultKey.isEmpty() ? request.password : QString {} },
        { QStringLiteral("proxyUrl"), request.proxyUrl },
        { QStringLiteral("checksumAlgorithm"), request.checksumAlgorithm },
        { QStringLiteral("expectedChecksum"), request.expectedChecksum },
        { QStringLiteral("expectedSha256"), request.expectedSha256 },
        { QStringLiteral("completionCommand"), request.completionCommand },
        { QStringLiteral("completionMoveDirectory"), request.completionMoveDirectory },
        { QStringLiteral("archiveDestination"), request.archiveDestination },
        { QStringLiteral("credentialVaultKey"), request.credentialVaultKey },
        { QStringLiteral("httpMethod"), request.httpMethod },
        { QStringLiteral("requestBody"), QString::fromLatin1(request.requestBody.toBase64()) },
        { QStringLiteral("scheduledAt"), request.scheduledAt.toUTC().toString(Qt::ISODateWithMs) },
        { QStringLiteral("windowStart"), request.windowStart.toString(QStringLiteral("HH:mm:ss")) },
        { QStringLiteral("windowEnd"), request.windowEnd.toString(QStringLiteral("HH:mm:ss")) },
        { QStringLiteral("allowedWeekdays"), request.allowedWeekdays },
        { QStringLiteral("segments"), request.segments },
        { QStringLiteral("perHostConnectionLimit"), request.perHostConnectionLimit },
        { QStringLiteral("dynamicSegmentation"), request.dynamicSegmentation },
        { QStringLiteral("removeRecordOnCompletion"), request.removeRecordOnCompletion },
        { QStringLiteral("extractArchive"), request.extractArchive },
        { QStringLiteral("deleteArchiveAfterExtraction"), request.deleteArchiveAfterExtraction },
        { QStringLiteral("maxRetries"), request.maxRetries },
        { QStringLiteral("retryBaseDelayMs"), request.retryBaseDelayMs },
        { QStringLiteral("connectTimeoutSeconds"), request.connectTimeoutSeconds },
        { QStringLiteral("lowSpeedTimeoutSeconds"), request.lowSpeedTimeoutSeconds },
        { QStringLiteral("maximumRedirects"), request.maximumRedirects },
        { QStringLiteral("priority"), request.priority },
        { QStringLiteral("repeatIntervalSeconds"), request.repeatIntervalSeconds },
        { QStringLiteral("fileConflictPolicy"), toStorageValue(request.fileConflictPolicy) },
        { QStringLiteral("speedLimitBytesPerSecond"), QString::number(request.speedLimitBytesPerSecond) },
        { QStringLiteral("sessionDataLimitBytes"), QString::number(request.sessionDataLimitBytes) },
        { QStringLiteral("expectedTotalBytes"), QString::number(request.expectedTotalBytes) },
        { QStringLiteral("headers"), headers }
    };
}

DownloadRequest requestFromJson(const QJsonObject& object)
{
    DownloadRequest request;
    request.url = QUrl(object.value(QStringLiteral("url")).toString());
    request.scheduleId = object.value(QStringLiteral("scheduleId")).toString();
    request.existingId = object.value(QStringLiteral("existingId")).toString();
    request.targetPath = object.value(QStringLiteral("targetPath")).toString();
    request.category = object.value(QStringLiteral("category")).toString();
    request.queueName = object.value(QStringLiteral("queueName")).toString(QStringLiteral("Main")).trimmed();
    if (request.queueName.isEmpty()) {
        request.queueName = QStringLiteral("Main");
    }
    request.username = object.value(QStringLiteral("username")).toString();
    request.password = object.value(QStringLiteral("password")).toString();
    request.proxyUrl = object.value(QStringLiteral("proxyUrl")).toString();
    request.checksumAlgorithm = object.value(QStringLiteral("checksumAlgorithm"))
                                    .toString(QStringLiteral("sha256")).trimmed().toLower();
    request.expectedChecksum = object.value(QStringLiteral("expectedChecksum")).toString().trimmed().toLower();
    request.expectedSha256 = object.value(QStringLiteral("expectedSha256")).toString().trimmed().toLower();
    if (request.expectedChecksum.isEmpty() && !request.expectedSha256.isEmpty()) {
        request.checksumAlgorithm = QStringLiteral("sha256");
        request.expectedChecksum = request.expectedSha256;
    }
    request.completionCommand = object.value(QStringLiteral("completionCommand")).toString().trimmed();
    request.completionMoveDirectory = object.value(QStringLiteral("completionMoveDirectory")).toString().trimmed();
    request.archiveDestination = object.value(QStringLiteral("archiveDestination")).toString().trimmed();
    request.credentialVaultKey = object.value(QStringLiteral("credentialVaultKey")).toString().trimmed();
    request.httpMethod = object.value(QStringLiteral("httpMethod")).toString().trimmed().toUpper();
    if (request.httpMethod != QStringLiteral("POST")) {
        request.httpMethod.clear();
    }
    request.requestBody = QByteArray::fromBase64(object.value(QStringLiteral("requestBody")).toString().toLatin1());
    request.scheduledAt = QDateTime::fromString(object.value(QStringLiteral("scheduledAt")).toString(), Qt::ISODateWithMs);
    request.windowStart = QTime::fromString(object.value(QStringLiteral("windowStart")).toString(), QStringLiteral("HH:mm:ss"));
    request.windowEnd = QTime::fromString(object.value(QStringLiteral("windowEnd")).toString(), QStringLiteral("HH:mm:ss"));
    request.allowedWeekdays = object.value(QStringLiteral("allowedWeekdays")).toInt(0x7f) & 0x7f;
    request.segments = object.value(QStringLiteral("segments")).toInt(8);
    request.perHostConnectionLimit = qBound(1, object.value(QStringLiteral("perHostConnectionLimit")).toInt(16), 128);
    request.dynamicSegmentation = object.value(QStringLiteral("dynamicSegmentation")).toBool(true);
    request.removeRecordOnCompletion = object.value(QStringLiteral("removeRecordOnCompletion")).toBool(false);
    request.extractArchive = object.value(QStringLiteral("extractArchive")).toBool(false);
    request.deleteArchiveAfterExtraction = object.value(QStringLiteral("deleteArchiveAfterExtraction")).toBool(false);
    request.maxRetries = object.value(QStringLiteral("maxRetries")).toInt(5);
    request.retryBaseDelayMs = object.value(QStringLiteral("retryBaseDelayMs")).toInt(500);
    request.connectTimeoutSeconds = qBound(1, object.value(QStringLiteral("connectTimeoutSeconds")).toInt(15), 3600);
    request.lowSpeedTimeoutSeconds = qBound(1, object.value(QStringLiteral("lowSpeedTimeoutSeconds")).toInt(60), 3600);
    request.maximumRedirects = qBound(0, object.value(QStringLiteral("maximumRedirects")).toInt(20), 100);
    request.priority = object.value(QStringLiteral("priority")).toInt(0);
    request.repeatIntervalSeconds = qMax(0, object.value(QStringLiteral("repeatIntervalSeconds")).toInt(0));
    request.fileConflictPolicy = fileConflictPolicyFromStorage(
        object.value(QStringLiteral("fileConflictPolicy")).toString(QStringLiteral("auto-rename")));
    request.speedLimitBytesPerSecond = object.value(QStringLiteral("speedLimitBytesPerSecond")).toString().toLongLong();
    request.sessionDataLimitBytes = qMax<qint64>(
        0, object.value(QStringLiteral("sessionDataLimitBytes")).toString().toLongLong());
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
        { QStringLiteral("segments"), segments },
        { QStringLiteral("request"), requestToJson(record.request) }
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
    record.request = requestFromJson(object.value(QStringLiteral("request")).toObject());
    if (!record.request.url.isValid()) {
        record.request.url = record.url;
        record.request.targetPath = record.targetPath;
        record.request.category = record.category;
    }
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
