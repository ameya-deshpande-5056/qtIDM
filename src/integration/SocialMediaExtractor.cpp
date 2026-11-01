#include "integration/SocialMediaExtractor.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>
#include <algorithm>

namespace qtidm {

namespace {
QList<int> parseVersion(const QString& raw)
{
    QList<int> numbers;
    const auto tokens = raw.trimmed().split(QLatin1Char('.'));
    for (const auto& token : tokens) {
        bool ok = false;
        int n = token.toInt(&ok);
        if (ok) {
            numbers.append(n);
        }
    }
    return numbers;
}

bool isNewerVersion(const QList<int>& a, const QList<int>& b)
{
    const int maxLen = std::max(a.size(), b.size());
    for (int i = 0; i < maxLen; ++i) {
        const int valA = (i < a.size()) ? a[i] : 0;
        const int valB = (i < b.size()) ? b[i] : 0;
        if (valA != valB) {
            return valA > valB;
        }
    }
    return false;
}

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
    const bool video = format.value(QStringLiteral("vcodec")).toString(QStringLiteral("none")) != QStringLiteral("none")
        || height > 0;
    const bool audio = format.value(QStringLiteral("acodec")).toString(QStringLiteral("none")) != QStringLiteral("none");
    if (video && audio) {
        parts.append(QStringLiteral("video + audio"));
    } else if (video) {
        parts.append(QStringLiteral("video only"));
    } else if (audio) {
        parts.append(QStringLiteral("audio only"));
    }
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

SocialMediaExtractor::CommandInvocation SocialMediaExtractor::selectLatestCommand(
    const QList<CommandInvocation>& candidates)
{
    CommandInvocation bestCmd;
    QList<int> bestVersion;

    for (const auto& candidate : candidates) {
        if (!candidate.isValid()) continue;

        QProcess process;
        process.start(candidate.program, candidate.prefixArguments + QStringList{QStringLiteral("--version")}, QIODevice::ReadOnly);
        if (!process.waitForStarted(1500) || !process.waitForFinished(3000) || process.exitCode() != 0) {
            if (!bestCmd.isValid()) {
                bestCmd = candidate;
            }
            continue;
        }

        const auto output = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
        const auto version = parseVersion(output);
        if (version.isEmpty()) {
            if (!bestCmd.isValid()) {
                bestCmd = candidate;
            }
            continue;
        }

        if (!bestCmd.isValid() || isNewerVersion(version, bestVersion)) {
            bestCmd = candidate;
            bestVersion = version;
        }
    }

    return bestCmd;
}

SocialMediaExtractor::CommandInvocation SocialMediaExtractor::resolveCommand() const
{
    if (commandResolved_) {
        return cachedCommand_;
    }

    if (!executable_.isEmpty()) {
        cachedCommand_ = CommandInvocation{executable_, {}};
        commandResolved_ = true;
        return cachedCommand_;
    }

    QList<CommandInvocation> candidates;
    QSet<QString> seenPaths;

    auto addCandidate = [&](const QString& path, const QStringList& prefixArgs = {}) {
        if (path.isEmpty()) return;
        const auto canonical = QFileInfo(path).canonicalFilePath();
        const auto key = canonical.isEmpty() ? path : canonical;
        if (seenPaths.contains(key)) return;
        seenPaths.insert(key);
        candidates.append(CommandInvocation{path, prefixArgs});
    };

    // 1. User pip / pipx / virtualenvs (~/.local/bin, ~/.pyenv/shims)
    const auto userLocal = QDir::home().filePath(QStringLiteral(".local/bin/yt-dlp"));
    if (QFileInfo::exists(userLocal)) {
        addCandidate(userLocal);
    }
    const auto pyenvShim = QDir::home().filePath(QStringLiteral(".pyenv/shims/yt-dlp"));
    if (QFileInfo::exists(pyenvShim)) {
        addCandidate(pyenvShim);
    }

    // 2. Custom system-wide pip install (/usr/local/bin)
    const auto usrLocal = QStringLiteral("/usr/local/bin/yt-dlp");
    if (QFileInfo::exists(usrLocal)) {
        addCandidate(usrLocal);
    }

    // 3. User PATH lookup (finds user venv or active python environment)
    const auto standard = QStandardPaths::findExecutable(QStringLiteral("yt-dlp"));
    if (!standard.isEmpty()) {
        addCandidate(standard);
    }

    // 4. Distribution package (apt /usr/bin)
    const auto usrBin = QStringLiteral("/usr/bin/yt-dlp");
    if (QFileInfo::exists(usrBin)) {
        addCandidate(usrBin);
    }

    // 5. Python3 module (python3 -m yt_dlp)
    const auto python3 = QStandardPaths::findExecutable(QStringLiteral("python3"));
    if (!python3.isEmpty()) {
        candidates.append(CommandInvocation{python3, {QStringLiteral("-m"), QStringLiteral("yt_dlp")}});
    }

    cachedCommand_ = selectLatestCommand(candidates);
    commandResolved_ = true;
    return cachedCommand_;
}

QString SocialMediaExtractor::executable() const
{
    return resolveCommand().program;
}

bool SocialMediaExtractor::isAvailable() const
{
    return resolveCommand().isValid();
}

bool SocialMediaExtractor::supports(const QUrl& url)
{
    const auto host = url.host().toLower();
    static const QStringList supportedHosts {
        QStringLiteral("youtube.com"), QStringLiteral("youtu.be"),
        QStringLiteral("bilibili.com"), QStringLiteral("b23.tv"), QStringLiteral("bili2233.cn"),
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
    const auto cmd = resolveCommand();
    if (!cmd.isValid()) {
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
        QStringLiteral("--format"), QStringLiteral("all")
    };
    if (!browserCookies.trimmed().isEmpty()) {
        arguments << QStringLiteral("--cookies-from-browser") << browserCookies.trimmed();
    }
    arguments << QStringLiteral("--") << url.toString(QUrl::FullyEncoded);

    QProcess process;
    process.start(cmd.program, cmd.prefixArguments + arguments, QIODevice::ReadOnly);
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
    struct ParsedFormat {
        QString id;
        QString label;
        QString extension;
        QUrl url;
        QVariantMap headers;
        bool adaptive = false;
        bool video = false;
        bool audio = false;
        int height = 0;
        int width = 0;
        double abr = 0.0;
        double tbr = 0.0;
        QString vcodec;
        QString acodec;
        QString note;
    };

    QList<ParsedFormat> allParsed;
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
        const auto ext = format.value(QStringLiteral("ext")).toString().toLower();
        const auto note = format.value(QStringLiteral("format_note")).toString().trimmed();
        const auto noteLower = note.toLower();
        const auto vcodec = format.value(QStringLiteral("vcodec")).toString(QStringLiteral("none"));
        const auto acodec = format.value(QStringLiteral("acodec")).toString(QStringLiteral("none"));
        const int height = format.value(QStringLiteral("height")).toInt();
        const bool video = (vcodec != QStringLiteral("none")) || (height > 0);
        const bool audio = (acodec != QStringLiteral("none"));
        if ((!video && !audio) || ext == QStringLiteral("mhtml") || noteLower.contains(QStringLiteral("storyboard"))) {
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

        ParsedFormat item;
        item.id = format.value(QStringLiteral("format_id")).toString();
        item.label = formatLabel(format);
        item.extension = ext.isEmpty() ? QStringLiteral("mp4") : ext;
        item.url = mediaUrl;
        item.headers = headers;
        item.adaptive = adaptive;
        item.video = video;
        item.audio = audio;
        item.height = height;
        item.width = format.value(QStringLiteral("width")).toInt();
        item.abr = format.value(QStringLiteral("abr")).toDouble(0.0);
        item.tbr = format.value(QStringLiteral("tbr")).toDouble(format.value(QStringLiteral("vbr")).toDouble(0.0));
        item.vcodec = vcodec;
        item.acodec = acodec;
        item.note = note;
        allParsed.append(std::move(item));
    }

    QList<ParsedFormat> videoOnly;
    QList<ParsedFormat> audioOnly;
    QList<ParsedFormat> preMuxed;

    for (const auto& item : allParsed) {
        if (item.video && item.audio) {
            preMuxed.append(item);
        } else if (item.video) {
            videoOnly.append(item);
        } else if (item.audio) {
            audioOnly.append(item);
        }
    }

    std::stable_sort(videoOnly.begin(), videoOnly.end(), [](const ParsedFormat& a, const ParsedFormat& b) {
        if (a.height != b.height) {
            return a.height > b.height;
        }
        return a.tbr > b.tbr;
    });

    std::stable_sort(audioOnly.begin(), audioOnly.end(), [](const ParsedFormat& a, const ParsedFormat& b) {
        if (a.abr != b.abr) {
            return a.abr > b.abr;
        }
        return a.tbr > b.tbr;
    });

    std::stable_sort(preMuxed.begin(), preMuxed.end(), [](const ParsedFormat& a, const ParsedFormat& b) {
        if (a.height != b.height) {
            return a.height > b.height;
        }
        return a.tbr > b.tbr;
    });

    if (!videoOnly.isEmpty() && !audioOnly.isEmpty()) {
        auto findBestAudio = [&](const ParsedFormat& video) -> ParsedFormat {
            ParsedFormat best = audioOnly.first();
            double bestScore = -1.0;
            for (const auto& audio : audioOnly) {
                double score = audio.abr > 0 ? audio.abr : (audio.tbr > 0 ? audio.tbr : 1.0);
                const auto vext = video.extension.toLower();
                const auto aext = audio.extension.toLower();
                const auto acodec = audio.acodec.toLower();
                if (vext == QStringLiteral("mp4")) {
                    if (aext == QStringLiteral("m4a") || aext == QStringLiteral("mp4")
                        || acodec.contains(QStringLiteral("aac")) || acodec.contains(QStringLiteral("mp4a"))) {
                        score += 10000.0;
                    }
                } else if (vext == QStringLiteral("webm")) {
                    if (aext == QStringLiteral("webm") || acodec.contains(QStringLiteral("opus"))
                        || acodec.contains(QStringLiteral("vorbis"))) {
                        score += 10000.0;
                    }
                }
                if (score > bestScore) {
                    bestScore = score;
                    best = audio;
                }
            }
            return best;
        };

        for (const auto& video : videoOnly) {
            const auto bestAudio = findBestAudio(video);
            SocialMediaFormat combined;
            combined.id = QStringLiteral("%1+%2").arg(video.id, bestAudio.id);

            QStringList parts;
            if (!video.note.isEmpty() && !video.note.contains(QStringLiteral("video only"), Qt::CaseInsensitive)) {
                parts.append(video.note);
            }
            if (video.height > 0) {
                parts.append(QStringLiteral("%1p").arg(video.height));
            }
            if (!video.extension.isEmpty()) {
                parts.append(video.extension.toUpper());
            }
            parts.append(QStringLiteral("video + audio"));
            combined.label = parts.join(QStringLiteral(" · "));

            combined.extension = video.extension;
            combined.url = video.url;
            combined.headers = video.headers;
            combined.audioUrl = bestAudio.url;
            combined.audioHeaders = bestAudio.headers;
            combined.adaptive = video.adaptive || bestAudio.adaptive;
            combined.isCombined = true;
            result.formats.append(combined);
        }
    }

    for (const auto& item : preMuxed) {
        result.formats.append({
            item.id,
            item.label,
            item.extension,
            item.url,
            {},
            item.headers,
            {},
            item.adaptive,
            false
        });
    }

    for (const auto& item : audioOnly) {
        result.formats.append({
            item.id,
            item.label,
            item.extension,
            item.url,
            {},
            item.headers,
            {},
            item.adaptive,
            false
        });
    }

    for (const auto& item : videoOnly) {
        result.formats.append({
            item.id,
            item.label,
            item.extension,
            item.url,
            {},
            item.headers,
            {},
            item.adaptive,
            false
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
