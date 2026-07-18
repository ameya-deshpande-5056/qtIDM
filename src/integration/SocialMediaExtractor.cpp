#include "integration/SocialMediaExtractor.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>
#include <algorithm>

namespace qtidm {

namespace {
QVariantMap headersFromJson(const QJsonObject& object)
{
    QVariantMap result;
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (it.value().isString()) {
            result.insert(it.key(), it.value().toString());
        }
    }
    return result;
}

QString formatLabel(const QJsonObject& format)
{
    QStringList parts;
    const auto note = format.value(QStringLiteral("format_note")).toString();
    const int height = format.value(QStringLiteral("height")).toInt();
    if (!note.isEmpty()) parts.append(note);
    if (height > 0) parts.append(QStringLiteral("%1p").arg(height));
    const auto extension = format.value(QStringLiteral("ext")).toString();
    if (!extension.isEmpty()) parts.append(extension.toUpper());
    const bool video = format.value(QStringLiteral("vcodec")).toString(QStringLiteral("none")) != QStringLiteral("none");
    const bool audio = format.value(QStringLiteral("acodec")).toString(QStringLiteral("none")) != QStringLiteral("none");
    parts.append(video && audio ? QStringLiteral("video + audio")
                               : video ? QStringLiteral("video only") : QStringLiteral("audio only"));
    return parts.join(QStringLiteral(" · "));
}

bool jsonValueMeansDrm(const QJsonValue& value)
{
    if (value.isBool()) {
        return value.toBool();
    }
    if (value.isDouble()) {
        return value.toInt() != 0;
    }
    if (value.isString()) {
        const auto marker = value.toString().trimmed().toLower();
        return !marker.isEmpty() && marker != QStringLiteral("false")
            && marker != QStringLiteral("no") && marker != QStringLiteral("0");
    }
    return false;
}
}

SocialMediaExtractor::SocialMediaExtractor(QString executable)
    : executable_(std::move(executable))
{
}

QString SocialMediaExtractor::executable() const
{
    return executable_.isEmpty() ? QStandardPaths::findExecutable(QStringLiteral("yt-dlp")) : executable_;
}

bool SocialMediaExtractor::isAvailable() const
{
    return !executable().isEmpty();
}

bool SocialMediaExtractor::supports(const QUrl& url)
{
    const auto host = url.host().toLower();
    static const QStringList supportedHosts {
        QStringLiteral("instagram.com"), QStringLiteral("facebook.com"), QStringLiteral("fb.watch"),
        QStringLiteral("twitter.com"), QStringLiteral("x.com"), QStringLiteral("tiktok.com"),
        QStringLiteral("reddit.com"), QStringLiteral("redd.it"), QStringLiteral("vimeo.com"),
        QStringLiteral("dailymotion.com"), QStringLiteral("twitch.tv"), QStringLiteral("soundcloud.com")
    };
    return std::any_of(supportedHosts.cbegin(), supportedHosts.cend(), [&host](const QString& domain) {
        return host == domain || host.endsWith(QLatin1Char('.') + domain);
    });
}

SocialMediaExtraction SocialMediaExtractor::extract(
    const QUrl& url, const QString& browserCookies, QString* error) const
{
    SocialMediaExtraction result;
    if (!supports(url)) {
        if (error) {
            *error = QStringLiteral("This address is not from a supported social-media site.");
        }
        return result;
    }
    const auto tool = executable();
    if (tool.isEmpty()) {
        if (error) {
            *error = QStringLiteral("Site-specific extraction requires yt-dlp.");
        }
        return result;
    }

    QStringList arguments {
        QStringLiteral("--dump-single-json"),
        QStringLiteral("--no-warnings"),
        QStringLiteral("--no-playlist"),
        QStringLiteral("--skip-download"),
        QStringLiteral("--format"), QStringLiteral("best")
    };
    if (!browserCookies.trimmed().isEmpty()) {
        arguments << QStringLiteral("--cookies-from-browser") << browserCookies.trimmed();
    }
    arguments << QStringLiteral("--") << url.toString(QUrl::FullyEncoded);

    QProcess process;
    process.start(tool, arguments, QIODevice::ReadOnly);
    if (!process.waitForStarted(5000)) {
        if (error) {
            *error = process.errorString();
        }
        return result;
    }
    if (!process.waitForFinished(90000)) {
        process.kill();
        process.waitForFinished(1000);
        if (error) {
            *error = QStringLiteral("Social-media extraction timed out.");
        }
        return result;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (error) {
            const auto diagnostics = QString::fromUtf8(process.readAllStandardError()).trimmed();
            *error = diagnostics.contains(QStringLiteral("DRM"), Qt::CaseInsensitive)
                ? QStringLiteral("This media is DRM-protected. qtIDM does not bypass DRM.")
                : diagnostics.isEmpty() ? QStringLiteral("yt-dlp could not extract this address.")
                                        : diagnostics.right(2000);
        }
        return result;
    }

    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(process.readAllStandardOutput(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) {
            *error = QStringLiteral("yt-dlp returned invalid metadata.");
        }
        return result;
    }
    const auto root = document.object();
    if (jsonValueMeansDrm(root.value(QStringLiteral("is_drm")))
        || jsonValueMeansDrm(root.value(QStringLiteral("has_drm")))
        || jsonValueMeansDrm(root.value(QStringLiteral("_has_drm")))) {
        if (error) {
            *error = QStringLiteral("This media is DRM-protected. qtIDM does not bypass DRM.");
        }
        return result;
    }
    result.title = root.value(QStringLiteral("title")).toString(QStringLiteral("social-media"));
    result.extractor = root.value(QStringLiteral("extractor_key")).toString(
        root.value(QStringLiteral("extractor")).toString());
    const auto baseHeaders = headersFromJson(root.value(QStringLiteral("http_headers")).toObject());
    QSet<QString> seenUrls;
    const auto formats = root.value(QStringLiteral("formats")).toArray();
    bool rejectedDrmFormat = false;
    for (const auto& value : formats) {
        const auto format = value.toObject();
        if (jsonValueMeansDrm(format.value(QStringLiteral("has_drm")))
            || jsonValueMeansDrm(format.value(QStringLiteral("is_drm")))) {
            rejectedDrmFormat = true;
            continue;
        }
        const QUrl mediaUrl(format.value(QStringLiteral("url")).toString());
        if (!mediaUrl.isValid() || (mediaUrl.scheme() != QStringLiteral("http")
            && mediaUrl.scheme() != QStringLiteral("https")) || seenUrls.contains(mediaUrl.toString())) {
            continue;
        }
        seenUrls.insert(mediaUrl.toString());
        auto headers = baseHeaders;
        const auto formatHeaders = headersFromJson(format.value(QStringLiteral("http_headers")).toObject());
        for (auto it = formatHeaders.cbegin(); it != formatHeaders.cend(); ++it) {
            headers.insert(it.key(), it.value());
        }
        const auto protocol = format.value(QStringLiteral("protocol")).toString().toLower();
        const auto mediaAddress = mediaUrl.path(QUrl::FullyDecoded).toLower();
        const bool adaptive = protocol.contains(QStringLiteral("m3u8"))
            || protocol.contains(QStringLiteral("dash"))
            || mediaAddress.contains(QStringLiteral(".m3u8"))
            || mediaAddress.contains(QStringLiteral(".mpd"));
        result.formats.append({
            format.value(QStringLiteral("format_id")).toString(),
            formatLabel(format),
            format.value(QStringLiteral("ext")).toString(QStringLiteral("mp4")),
            mediaUrl,
            headers,
            adaptive
        });
    }
    if (result.formats.isEmpty() && error) {
        *error = rejectedDrmFormat
            ? QStringLiteral("All extracted formats are DRM-protected. qtIDM does not bypass DRM.")
            : QStringLiteral("No downloadable non-DRM formats were found.");
    }
    return result;
}

}
