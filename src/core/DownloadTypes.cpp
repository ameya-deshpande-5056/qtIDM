#include "core/DownloadTypes.h"

#include <QJsonArray>
#include <QJsonValue>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>

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
    QJsonArray cookiesJson;
    for (const auto& c : request.cookies) {
        cookiesJson.append(QJsonObject {
            { QStringLiteral("name"), c.name },
            { QStringLiteral("value"), c.value },
            { QStringLiteral("domain"), c.domain },
            { QStringLiteral("hostOnly"), c.hostOnly },
            { QStringLiteral("path"), c.path },
            { QStringLiteral("secure"), c.secure },
            { QStringLiteral("httpOnly"), c.httpOnly },
            { QStringLiteral("session"), c.session },
            { QStringLiteral("expirationDate"), QString::number(c.expirationDate) }
        });
    }
    return {
        { QStringLiteral("url"), request.url.toString(QUrl::FullyEncoded) },
        { QStringLiteral("scheduleId"), request.scheduleId },
        { QStringLiteral("existingId"), request.existingId },
        { QStringLiteral("refreshTarget"), request.refreshTarget.toString(QUrl::FullyEncoded) },
        { QStringLiteral("siteAdapterToken"), request.siteAdapterToken },
        { QStringLiteral("siteAdapterHint"), request.siteAdapterHint },
        { QStringLiteral("pageTitle"), request.pageTitle },
        { QStringLiteral("browserTlsProfile"), request.browserTlsProfile },
        { QStringLiteral("enableSemanticNaming"), request.enableSemanticNaming },
        { QStringLiteral("targetPath"), request.targetPath },
        { QStringLiteral("category"), request.category },
        { QStringLiteral("queueName"), request.queueName },
        { QStringLiteral("username"), request.username },
        { QStringLiteral("password"), request.credentialVaultKey.isEmpty() ? request.password : QString {} },
        { QStringLiteral("proxyUrl"), request.proxyUrl },
        { QStringLiteral("dohUrl"), request.dohUrl },
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
        { QStringLiteral("allowPrivateNetworkRedirects"), request.allowPrivateNetworkRedirects },
        { QStringLiteral("insecureSkipSslVerification"), request.insecureSkipSslVerification },
        { QStringLiteral("caCertificatePath"), request.caCertificatePath },
        { QStringLiteral("priority"), request.priority },
        { QStringLiteral("repeatIntervalSeconds"), request.repeatIntervalSeconds },
        { QStringLiteral("fileConflictPolicy"), toStorageValue(request.fileConflictPolicy) },
        { QStringLiteral("speedLimitBytesPerSecond"), QString::number(request.speedLimitBytesPerSecond) },
        { QStringLiteral("sessionDataLimitBytes"), QString::number(request.sessionDataLimitBytes) },
        { QStringLiteral("expectedTotalBytes"), QString::number(request.expectedTotalBytes) },
        { QStringLiteral("entityTag"), request.entityTag },
        { QStringLiteral("lastModified"), request.lastModified },
        { QStringLiteral("serverCookies"), QJsonArray::fromStringList(request.serverCookies) },
        { QStringLiteral("cookies"), cookiesJson },
        { QStringLiteral("headers"), headers }
    };
}

DownloadRequest requestFromJson(const QJsonObject& object)
{
    DownloadRequest request;
    request.url = QUrl(object.value(QStringLiteral("url")).toString());
    request.scheduleId = object.value(QStringLiteral("scheduleId")).toString();
    request.existingId = object.value(QStringLiteral("existingId")).toString();
    request.refreshTarget = QUrl(object.value(QStringLiteral("refreshTarget")).toString());
    request.siteAdapterToken = object.value(QStringLiteral("siteAdapterToken")).toString();
    request.siteAdapterHint = object.value(QStringLiteral("siteAdapterHint")).toString().trimmed();
    request.pageTitle = object.value(QStringLiteral("pageTitle")).toString();
    request.browserTlsProfile = object.value(QStringLiteral("browserTlsProfile")).toString(QStringLiteral("auto")).trimmed().toLower();
    if (request.browserTlsProfile.isEmpty()) {
        request.browserTlsProfile = QStringLiteral("auto");
    }
    request.enableSemanticNaming = object.value(QStringLiteral("enableSemanticNaming")).toBool(true);
    request.targetPath = object.value(QStringLiteral("targetPath")).toString();
    request.category = object.value(QStringLiteral("category")).toString();
    request.queueName = object.value(QStringLiteral("queueName")).toString(QStringLiteral("Main")).trimmed();
    if (request.queueName.isEmpty()) {
        request.queueName = QStringLiteral("Main");
    }
    request.username = object.value(QStringLiteral("username")).toString();
    request.password = object.value(QStringLiteral("password")).toString();
    request.proxyUrl = object.value(QStringLiteral("proxyUrl")).toString();
    request.dohUrl = object.value(QStringLiteral("dohUrl")).toString().trimmed();
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
    request.allowPrivateNetworkRedirects = object.value(QStringLiteral("allowPrivateNetworkRedirects")).toBool(false);
    request.insecureSkipSslVerification = object.value(QStringLiteral("insecureSkipSslVerification")).toBool(false);
    request.caCertificatePath = object.value(QStringLiteral("caCertificatePath")).toString();
    request.pinnedIp.clear(); // Pinned IP is strictly an internal runtime resolution artifact
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
    request.entityTag = object.value(QStringLiteral("entityTag")).toString().trimmed();
    request.lastModified = object.value(QStringLiteral("lastModified")).toString().trimmed();
    const auto serverCookiesArr = object.value(QStringLiteral("serverCookies")).toArray();
    for (const auto& val : serverCookiesArr) {
        request.serverCookies.append(val.toString());
    }
    const auto cookiesArr = object.value(QStringLiteral("cookies")).toArray();
    for (const auto& val : cookiesArr) {
        const auto cObj = val.toObject();
        CookieRecord record;
        record.name = cObj.value(QStringLiteral("name")).toString();
        record.value = cObj.value(QStringLiteral("value")).toString();
        record.domain = cObj.value(QStringLiteral("domain")).toString();
        record.hostOnly = cObj.value(QStringLiteral("hostOnly")).toBool();
        record.path = cObj.value(QStringLiteral("path")).toString(QStringLiteral("/"));
        record.secure = cObj.value(QStringLiteral("secure")).toBool();
        record.httpOnly = cObj.value(QStringLiteral("httpOnly")).toBool();
        record.session = cObj.value(QStringLiteral("session")).toBool();
        record.expirationDate = cObj.value(QStringLiteral("expirationDate")).toString().toLongLong();
        if (!record.name.isEmpty()) {
            request.cookies.append(record);
        }
    }
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
            { QStringLiteral("durableWritten"), QString::number(segment.durableWritten) },
            { QStringLiteral("status"), toStorageValue(segment.status) }
        });
    }
    return {
        { QStringLiteral("id"), record.id },
        { QStringLiteral("url"), record.url.toString(QUrl::FullyEncoded) },
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
        segment.durableWritten = item.contains(QStringLiteral("durableWritten"))
            ? item.value(QStringLiteral("durableWritten")).toString().toLongLong()
            : segment.written;
        segment.status = downloadStatusFromStorage(item.value(QStringLiteral("status")).toString());
        record.segments.append(segment);
    }
    return record;
}

QString DomainRule::normalizeDomain(const QString& raw)
{
    QString clean = raw.trimmed().toLower();
    if (clean.startsWith(QStringLiteral("*."))) {
        clean = clean.mid(2);
    }
    while (clean.startsWith(QLatin1Char('.'))) {
        clean.remove(0, 1);
    }
    while (clean.endsWith(QLatin1Char('.'))) {
        clean.chop(1);
    }
    return clean;
}

bool DomainRule::matchesHost(const QString& host) const
{
    const QString normRule = normalizeDomain(domain);
    const QString normHost = normalizeDomain(host);
    if (normRule.isEmpty() || normHost.isEmpty()) {
        return false;
    }
    if (normHost == normRule) {
        return true;
    }
    return includeSubdomains && normHost.endsWith(QLatin1Char('.') + normRule);
}

const DomainRule* DomainRule::findBestMatch(const QVector<DomainRule>& rules, const QString& host)
{
    const QString normHost = normalizeDomain(host);
    if (normHost.isEmpty()) {
        return nullptr;
    }
    const DomainRule* best = nullptr;
    int bestScore = -1;
    for (const auto& rule : rules) {
        if (!rule.matchesHost(normHost)) {
            continue;
        }
        const QString normRule = normalizeDomain(rule.domain);
        // Exact match always beats subdomain match; otherwise longer domain suffix wins
        const int score = (normRule == normHost ? 10000 : 0) + normRule.size();
        if (score > bestScore) {
            bestScore = score;
            best = &rule;
        }
    }
    return best;
}

QJsonObject domainRuleToJson(const DomainRule& rule)
{
    QJsonObject headers;
    for (auto it = rule.customHeaders.cbegin(); it != rule.customHeaders.cend(); ++it) {
        headers.insert(it.key(), QJsonValue::fromVariant(it.value()));
    }
    return {
        { QStringLiteral("domain"), DomainRule::normalizeDomain(rule.domain) },
        { QStringLiteral("includeSubdomains"), rule.includeSubdomains },
        { QStringLiteral("maxSegments"), qBound(0, rule.maxSegments, 32) },
        { QStringLiteral("maxConcurrentDownloads"), qMax(0, rule.maxConcurrentDownloads) },
        { QStringLiteral("speedLimitBytesPerSecond"), QString::number(qMax<qint64>(0, rule.speedLimitBytesPerSecond)) },
        { QStringLiteral("customUserAgent"), rule.customUserAgent },
        { QStringLiteral("customHeaders"), headers }
    };
}

DomainRule domainRuleFromJson(const QJsonObject& object)
{
    DomainRule rule;
    rule.domain = DomainRule::normalizeDomain(object.value(QStringLiteral("domain")).toString());
    rule.includeSubdomains = object.value(QStringLiteral("includeSubdomains")).toBool(true);
    rule.maxSegments = qBound(0, object.value(QStringLiteral("maxSegments")).toInt(0), 32);
    rule.maxConcurrentDownloads = qMax(0, object.value(QStringLiteral("maxConcurrentDownloads")).toInt(0));
    rule.speedLimitBytesPerSecond = qMax<qint64>(0, object.value(QStringLiteral("speedLimitBytesPerSecond")).toString().toLongLong());
    rule.customUserAgent = object.value(QStringLiteral("customUserAgent")).toString().trimmed();
    const auto headers = object.value(QStringLiteral("customHeaders")).toObject();
    for (auto it = headers.begin(); it != headers.end(); ++it) {
        rule.customHeaders.insert(it.key(), it.value().toVariant());
    }
    return rule;
}

QString extensionFromMime(const QString& mimeType)
{
    if (mimeType.isEmpty()) {
        return {};
    }
    const auto cleaned = mimeType.section(QLatin1Char(';'), 0, 0).trimmed().toLower();

    // Audio
    if (cleaned == QStringLiteral("audio/mpeg") || cleaned == QStringLiteral("audio/mp3")) return QStringLiteral("mp3");
    if (cleaned == QStringLiteral("audio/mp4") || cleaned == QStringLiteral("audio/x-m4a") || cleaned == QStringLiteral("audio/m4a")) return QStringLiteral("m4a");
    if (cleaned == QStringLiteral("audio/aac") || cleaned == QStringLiteral("audio/x-aac") || cleaned == QStringLiteral("audio/aacp")) return QStringLiteral("aac");
    if (cleaned == QStringLiteral("audio/ogg") || cleaned == QStringLiteral("audio/vorbis") || cleaned == QStringLiteral("application/ogg")) return QStringLiteral("ogg");
    if (cleaned == QStringLiteral("audio/opus") || cleaned == QStringLiteral("audio/x-opus")) return QStringLiteral("opus");
    if (cleaned == QStringLiteral("audio/flac") || cleaned == QStringLiteral("audio/x-flac")) return QStringLiteral("flac");
    if (cleaned == QStringLiteral("audio/wav") || cleaned == QStringLiteral("audio/x-wav") || cleaned == QStringLiteral("audio/wave")) return QStringLiteral("wav");
    if (cleaned == QStringLiteral("audio/webm")) return QStringLiteral("webm");

    // Video
    if (cleaned == QStringLiteral("video/mp4")) return QStringLiteral("mp4");
    if (cleaned == QStringLiteral("video/webm")) return QStringLiteral("webm");
    if (cleaned == QStringLiteral("video/x-matroska")) return QStringLiteral("mkv");
    if (cleaned == QStringLiteral("video/quicktime")) return QStringLiteral("mov");
    if (cleaned == QStringLiteral("video/x-flv")) return QStringLiteral("flv");
    if (cleaned == QStringLiteral("video/x-msvideo")) return QStringLiteral("avi");
    if (cleaned == QStringLiteral("video/3gpp")) return QStringLiteral("3gp");
    if (cleaned == QStringLiteral("video/mp2t")) return QStringLiteral("ts");

    // Streaming Manifests
    if (cleaned == QStringLiteral("application/vnd.apple.mpegurl") || cleaned == QStringLiteral("application/x-mpegurl") || cleaned == QStringLiteral("audio/mpegurl")) return QStringLiteral("m3u8");
    if (cleaned == QStringLiteral("application/dash+xml")) return QStringLiteral("mpd");

    // Subtitles
    if (cleaned == QStringLiteral("text/vtt")) return QStringLiteral("vtt");
    if (cleaned == QStringLiteral("application/x-subrip")) return QStringLiteral("srt");
    if (cleaned == QStringLiteral("application/ttml+xml")) return QStringLiteral("ttml");
    if (cleaned == QStringLiteral("text/x-ssa") || cleaned == QStringLiteral("text/x-ass")) return QStringLiteral("ass");

    // Common web formats
    if (cleaned == QStringLiteral("application/pdf")) return QStringLiteral("pdf");
    if (cleaned == QStringLiteral("application/zip")) return QStringLiteral("zip");
    if (cleaned == QStringLiteral("application/x-tar")) return QStringLiteral("tar");
    if (cleaned == QStringLiteral("application/gzip") || cleaned == QStringLiteral("application/x-gzip")) return QStringLiteral("gz");
    if (cleaned == QStringLiteral("application/x-7z-compressed")) return QStringLiteral("7z");

    // Broad audio/video fallbacks
    if (cleaned.startsWith(QStringLiteral("audio/"))) return QStringLiteral("mp3");
    if (cleaned.startsWith(QStringLiteral("video/"))) return QStringLiteral("mp4");

    return {};
}

QString safeSuggestedFilename(QString value)
{
    value = value.trimmed();
    if (value.isEmpty() || value == QStringLiteral(".") || value == QStringLiteral("..")) {
        return {};
    }
    value.remove(QRegularExpression(QStringLiteral("[\\x{0}-\\x{1f}\\x{7f}]")));
    value.replace(QLatin1Char('/'), QStringLiteral(" - "));
    value.replace(QLatin1Char('\\'), QStringLiteral(" - "));
    value.replace(QLatin1Char(':'), QStringLiteral(" - "));
    value.replace(QLatin1Char('"'), QLatin1Char('\''));
    value.replace(QLatin1Char('?'), QString());
    value.replace(QLatin1Char('*'), QLatin1Char('_'));
    value.replace(QLatin1Char('<'), QLatin1Char('_'));
    value.replace(QLatin1Char('>'), QLatin1Char('_'));
    value.replace(QLatin1Char('|'), QLatin1Char('-'));
    value.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    value.replace(QRegularExpression(QStringLiteral("-\\s+-+")), QStringLiteral("-"));
    while (value.endsWith(QLatin1Char('.')) || value.endsWith(QLatin1Char(' ')) || value.endsWith(QLatin1Char('-'))) {
        value.chop(1);
        value = value.trimmed();
    }
    return value.trimmed();
}

QString adaptiveMediaSuggestedFilename(const QString& inputName)
{
    QString name = inputName.trimmed();
    if (name.isEmpty()) {
        return QStringLiteral("stream.mkv");
    }
    static const QStringList mediaExtensions {
        QStringLiteral(".m3u8"), QStringLiteral(".mpd"),
        QStringLiteral(".mkv"), QStringLiteral(".mp4"),
        QStringLiteral(".webm"), QStringLiteral(".ts"),
        QStringLiteral(".m4v"), QStringLiteral(".mov"),
        QStringLiteral(".avi"), QStringLiteral(".flv")
    };
    for (const auto& ext : mediaExtensions) {
        if (name.endsWith(ext, Qt::CaseInsensitive)) {
            name.chop(ext.length());
            break;
        }
    }
    name = safeSuggestedFilename(name);
    if (name.isEmpty()) {
        name = QStringLiteral("stream");
    }
    return name + QStringLiteral(".mkv");
}

QString resolveDownloadFilename(const QString& rawSuggested, const QUrl& url, const QString& mediaTypeHint, bool adaptiveMedia, const QString& mimeTypeHint)
{
    QString name = safeSuggestedFilename(rawSuggested);
    if (name.isEmpty()) {
        name = safeSuggestedFilename(url.fileName());
    }
    if (name.isEmpty()) {
        name = QStringLiteral("download");
    }

    if (adaptiveMedia) {
        return adaptiveMediaSuggestedFilename(name);
    }

    const auto dotIndex = name.lastIndexOf(QLatin1Char('.'));
    QString currentExt;
    if (dotIndex >= 0 && dotIndex < name.length() - 1) {
        const auto candidate = name.mid(dotIndex + 1).trimmed();
        static const QRegularExpression validExtRe(QStringLiteral("^[a-zA-Z0-9]{1,10}$"));
        if (validExtRe.match(candidate).hasMatch() && !candidate.contains(QLatin1Char(' '))) {
            currentExt = candidate.toLower();
        }
    }

    static const QSet<QString> scriptExtensions {
        QStringLiteral("php"), QStringLiteral("asp"), QStringLiteral("aspx"),
        QStringLiteral("jsp"), QStringLiteral("cgi"), QStringLiteral("htm"),
        QStringLiteral("html")
    };

    static const QSet<QString> mediaExtensions {
        QStringLiteral("mp4"), QStringLiteral("mp3"), QStringLiteral("m4a"),
        QStringLiteral("webm"), QStringLiteral("opus"), QStringLiteral("ogg"),
        QStringLiteral("flac"), QStringLiteral("wav"), QStringLiteral("aac"),
        QStringLiteral("mkv"), QStringLiteral("mov"), QStringLiteral("avi"),
        QStringLiteral("flv"), QStringLiteral("vtt"), QStringLiteral("srt")
    };

    const auto urlPath = url.path(QUrl::FullyDecoded);
    const auto urlDot = urlPath.lastIndexOf(QLatin1Char('.'));
    QString urlExt;
    if (urlDot >= 0 && urlDot < urlPath.length() - 1) {
        const auto candidate = urlPath.mid(urlDot + 1).trimmed().toLower();
        static const QRegularExpression validExtRe(QStringLiteral("^[a-zA-Z0-9]{1,10}$"));
        if (validExtRe.match(candidate).hasMatch() && !scriptExtensions.contains(candidate)) {
            urlExt = candidate;
        }
    }

    const QString mimeExt = extensionFromMime(mimeTypeHint);

    QString hintExt;
    const auto hint = mediaTypeHint.trimmed().toUpper();
    if (hint == QStringLiteral("VIDEO")) hintExt = QStringLiteral("mp4");
    else if (hint == QStringLiteral("AUDIO")) hintExt = QStringLiteral("mp3");
    else if (hint == QStringLiteral("SUBTITLE")) hintExt = QStringLiteral("vtt");
    else if (hint == QStringLiteral("HLS") || hint == QStringLiteral("DASH")) hintExt = QStringLiteral("mkv");

    QString targetExt;
    if (!urlExt.isEmpty() && mediaExtensions.contains(urlExt)) {
        targetExt = urlExt;
    } else if (!mimeExt.isEmpty()) {
        targetExt = mimeExt;
    } else if (!urlExt.isEmpty()) {
        targetExt = urlExt;
    } else if (!hintExt.isEmpty()) {
        targetExt = hintExt;
    }

    if (currentExt.isEmpty() || scriptExtensions.contains(currentExt)) {
        if (!currentExt.isEmpty()) {
            name = name.left(dotIndex);
        }
        if (!targetExt.isEmpty()) {
            name += QLatin1Char('.') + targetExt;
        } else {
            name += QStringLiteral(".bin");
        }
    } else if (!targetExt.isEmpty() && mediaExtensions.contains(currentExt) && currentExt != targetExt && !mimeExt.isEmpty()) {
        name = name.left(dotIndex) + QLatin1Char('.') + targetExt;
    }

    return name;
}

const QVector<DohPreset>& defaultDohPresets()
{
    static const QVector<DohPreset> presets = {
        { QStringLiteral("system"), QStringLiteral("System Default"), QString(),
          QStringLiteral("Use local OS name servers (standard DNS).") },
        { QStringLiteral("cloudflare"), QStringLiteral("Cloudflare (1.1.1.1)"), QStringLiteral("https://cloudflare-dns.com/dns-query"),
          QStringLiteral("Fast, privacy-first DNS-over-HTTPS by Cloudflare.") },
        { QStringLiteral("google"), QStringLiteral("Google (8.8.8.8)"), QStringLiteral("https://dns.google/dns-query"),
          QStringLiteral("High-reliability global DNS-over-HTTPS by Google.") },
        { QStringLiteral("quad9"), QStringLiteral("Quad9 (Recommended for Security)"), QStringLiteral("https://dns.quad9.net/dns-query"),
          QStringLiteral("Privacy-friendly DNS with automatic threat and malicious domain blocking.") },
        { QStringLiteral("adguard"), QStringLiteral("AdGuard DNS"), QStringLiteral("https://dns.adguard-dns.com/dns-query"),
          QStringLiteral("Blocks ads, tracking domains, and phishing endpoints.") },
        { QStringLiteral("opendns"), QStringLiteral("OpenDNS Home"), QStringLiteral("https://dns.opendns.com/dns-query"),
          QStringLiteral("Cisco OpenDNS with anti-phishing protection.") },
        { QStringLiteral("cloudflare_security"), QStringLiteral("Cloudflare Security"), QStringLiteral("https://security.cloudflare-dns.com/dns-query"),
          QStringLiteral("Cloudflare 1.1.1.2 with automated malware blocking.") },
        { QStringLiteral("custom"), QStringLiteral("Custom DoH URL…"), QString(),
          QStringLiteral("Enter a custom RFC 8484 DNS-over-HTTPS endpoint.") }
    };
    return presets;
}

QString resolveDohUrl(const QString& presetOrUrl)
{
    const QString trimmed = presetOrUrl.trimmed();
    if (trimmed.isEmpty() || trimmed.compare(QStringLiteral("system"), Qt::CaseInsensitive) == 0) {
        return QString();
    }
    for (const auto& preset : defaultDohPresets()) {
        if (preset.id.compare(trimmed, Qt::CaseInsensitive) == 0 ||
            preset.name.compare(trimmed, Qt::CaseInsensitive) == 0 ||
            preset.url.compare(trimmed, Qt::CaseInsensitive) == 0) {
            return preset.url;
        }
    }
    return trimmed;
}

}
