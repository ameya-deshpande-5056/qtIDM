#include "integration/UpdateChecker.h"
#include "integration/SocialMediaExtractor.h"

#include <curl/curl.h>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QThread>
#include <algorithm>

namespace qtidm {

namespace {

size_t curlWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    auto* buffer = static_cast<QByteArray*>(userdata);
    const size_t total = size * nmemb;
    buffer->append(ptr, static_cast<int>(total));
    return total;
}

QString queryProcessOutput(const QString& program, const QStringList& args, int timeoutMs = 2500)
{
    QProcess process;
    process.start(program, args, QIODevice::ReadOnly);
    if (!process.waitForStarted(timeoutMs) || !process.waitForFinished(timeoutMs)) {
        process.kill();
        return {};
    }
    return QString::fromUtf8(process.readAllStandardOutput()).trimmed();
}

} // namespace

UpdateChecker::UpdateChecker(QObject* parent)
    : QObject(parent)
{
    qRegisterMetaType<qtidm::UpdateCheckResult>("qtidm::UpdateCheckResult");
}

UpdateChecker::~UpdateChecker() = default;

QString UpdateChecker::cleanVersionTag(const QString& rawTag)
{
    auto cleaned = rawTag.trimmed();
    if (cleaned.startsWith(QLatin1Char('v'), Qt::CaseInsensitive)) {
        cleaned = cleaned.mid(1).trimmed();
    }
    return cleaned;
}

QList<int> UpdateChecker::parseVersionNumbers(const QString& versionStr)
{
    QList<int> numbers;
    static const QRegularExpression digitPattern(QStringLiteral("[0-9]+"));
    auto it = digitPattern.globalMatch(versionStr);
    while (it.hasNext()) {
        const auto match = it.next();
        bool ok = false;
        int val = match.captured().toInt(&ok);
        if (ok) {
            numbers.append(val);
        }
    }
    return numbers;
}

bool UpdateChecker::isVersionNewer(const QString& latest, const QString& current)
{
    const auto latestNums = parseVersionNumbers(latest);
    const auto currentNums = parseVersionNumbers(current);
    if (latestNums.isEmpty() || currentNums.isEmpty()) {
        return false;
    }
    const int count = std::max(latestNums.size(), currentNums.size());
    for (int i = 0; i < count; ++i) {
        const int l = (i < latestNums.size()) ? latestNums[i] : 0;
        const int c = (i < currentNums.size()) ? currentNums[i] : 0;
        if (l != c) {
            return l > c;
        }
    }
    return false;
}

QByteArray UpdateChecker::httpGet(const QString& url, QString* effectiveUrl, QString* error) const
{
    QByteArray buffer;
    auto* easy = curl_easy_init();
    if (!easy) {
        if (error) *error = QStringLiteral("Could not initialize network client (curl).");
        return {};
    }

    const auto urlBytes = url.toUtf8();
    curl_easy_setopt(easy, CURLOPT_URL, urlBytes.constData());
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, 8000L);
    curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, 12000L);
    curl_easy_setopt(easy, CURLOPT_USERAGENT, "qtIDM-UpdateChecker/" QTIDM_VERSION);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, curlWriteCallback);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, &buffer);

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Accept: application/vnd.github.v3+json, application/json, */*");
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);

    const auto res = curl_easy_perform(easy);
    if (effectiveUrl) {
        char* eff = nullptr;
        curl_easy_getinfo(easy, CURLINFO_EFFECTIVE_URL, &eff);
        if (eff) {
            *effectiveUrl = QString::fromUtf8(eff);
        }
    }

    long httpCode = 0;
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &httpCode);

    curl_slist_free_all(headers);
    curl_easy_cleanup(easy);

    if (res != CURLE_OK) {
        if (error) *error = QString::fromUtf8(curl_easy_strerror(res));
        return {};
    }

    if (httpCode >= 400) {
        if (error) *error = QStringLiteral("HTTP error %1").arg(httpCode);
        return buffer;
    }

    return buffer;
}

AppUpdateInfo UpdateChecker::checkAppUpdate() const
{
    AppUpdateInfo info;
    info.currentVersion = QCoreApplication::applicationVersion().trimmed();
    if (info.currentVersion.isEmpty()) {
        info.currentVersion = QStringLiteral(QTIDM_VERSION);
    }

    QString effectiveUrl;
    QString error;
    // Primary: GitHub API
    const QString apiUrl = QStringLiteral("https://api.github.com/repos/ameya-deshpande-5056/qtIDM/releases/latest");
    const auto jsonBytes = httpGet(apiUrl, &effectiveUrl, &error);

    QJsonParseError parseError;
    const auto doc = QJsonDocument::fromJson(jsonBytes, &parseError);
    if (!doc.isNull() && doc.isObject() && doc.object().contains(QStringLiteral("tag_name"))) {
        const auto obj = doc.object();
        info.releaseTag = obj.value(QStringLiteral("tag_name")).toString();
        info.latestVersion = cleanVersionTag(info.releaseTag);
        info.releaseTitle = obj.value(QStringLiteral("name")).toString();
        info.releaseUrl = obj.value(QStringLiteral("html_url")).toString();
        info.releaseNotes = obj.value(QStringLiteral("body")).toString().trimmed();
        info.publishedDate = obj.value(QStringLiteral("published_at")).toString();
        info.updateAvailable = isVersionNewer(info.latestVersion, info.currentVersion);
        info.checkSucceeded = true;
        return info;
    }

    // Fallback: GitHub Releases redirect lookup (resistant to API rate limits)
    const QString fallbackUrl = QStringLiteral("https://github.com/ameya-deshpande-5056/qtIDM/releases/latest");
    httpGet(fallbackUrl, &effectiveUrl, &error);

    static const QRegularExpression tagPattern(QStringLiteral("/releases/tag/([^/\\s]+)"));
    const auto match = tagPattern.match(effectiveUrl);
    if (match.hasMatch()) {
        info.releaseTag = match.captured(1);
        info.latestVersion = cleanVersionTag(info.releaseTag);
        info.releaseUrl = effectiveUrl;
        info.releaseTitle = QStringLiteral("qtIDM %1").arg(info.latestVersion);
        info.updateAvailable = isVersionNewer(info.latestVersion, info.currentVersion);
        info.checkSucceeded = true;
        return info;
    }

    info.checkSucceeded = false;
    info.errorMessage = error.isEmpty() ? QStringLiteral("Could not reach release server.") : error;
    return info;
}

DependencyUpdateInfo UpdateChecker::checkYtDlp() const
{
    DependencyUpdateInfo info;
    info.name = QStringLiteral("yt-dlp");

    // 1. Check local installed version
    SocialMediaExtractor extractor;
    const auto command = extractor.resolveCommand();
    if (command.isValid()) {
        info.executablePath = command.program;
        const auto output = queryProcessOutput(command.program, command.prefixArguments + QStringList{QStringLiteral("--version")});
        if (!output.isEmpty()) {
            info.installed = true;
            info.installedVersion = output.trimmed();
        }
    }

    if (!info.installed) {
        const auto standard = QStandardPaths::findExecutable(QStringLiteral("yt-dlp"));
        if (!standard.isEmpty()) {
            info.executablePath = standard;
            const auto output = queryProcessOutput(standard, {QStringLiteral("--version")});
            if (!output.isEmpty()) {
                info.installed = true;
                info.installedVersion = output.trimmed();
            }
        }
    }

    // 2. Check upstream latest version via PyPI JSON API (unlimited & fast)
    QString error;
    const auto pypiBytes = httpGet(QStringLiteral("https://pypi.org/pypi/yt-dlp/json"), nullptr, &error);
    const auto doc = QJsonDocument::fromJson(pypiBytes);
    if (!doc.isNull() && doc.isObject()) {
        info.latestVersion = doc.object().value(QStringLiteral("info")).toObject().value(QStringLiteral("version")).toString().trimmed();
    }

    // Fallback: GitHub Releases API
    if (info.latestVersion.isEmpty()) {
        const auto ghBytes = httpGet(QStringLiteral("https://api.github.com/repos/yt-dlp/yt-dlp/releases/latest"), nullptr, &error);
        const auto ghDoc = QJsonDocument::fromJson(ghBytes);
        if (!ghDoc.isNull() && ghDoc.isObject()) {
            info.latestVersion = cleanVersionTag(ghDoc.object().value(QStringLiteral("tag_name")).toString());
        }
    }

    if (info.installed && !info.latestVersion.isEmpty()) {
        info.updateAvailable = isVersionNewer(info.latestVersion, info.installedVersion);
    }

    info.canAutoUpdate = true;
    if (info.executablePath.contains(QStringLiteral(".local")) || info.executablePath.contains(QStringLiteral("python3"))) {
        info.updateCommand = QStringLiteral("python3 -m pip install --upgrade --user yt-dlp");
    } else {
        info.updateCommand = QStringLiteral("yt-dlp -U");
    }
    info.installCommand = QStringLiteral("python3 -m pip install --user yt-dlp");

    if (!info.installed) {
        info.statusText = QStringLiteral("Not installed (recommended for social media & video extraction)");
    } else if (info.updateAvailable) {
        info.statusText = QStringLiteral("Update available: %1").arg(info.latestVersion);
    } else {
        info.statusText = QStringLiteral("Up to date (%1)").arg(info.installedVersion);
    }

    return info;
}

DependencyUpdateInfo UpdateChecker::checkDeno() const
{
    DependencyUpdateInfo info;
    info.name = QStringLiteral("Deno");

    // 1. Check local installed version
    QString denoPath = QStandardPaths::findExecutable(QStringLiteral("deno"));
    if (denoPath.isEmpty()) {
        const auto userLocal = QDir::home().filePath(QStringLiteral(".deno/bin/deno"));
        if (QFileInfo::exists(userLocal)) {
            denoPath = userLocal;
        } else if (QFileInfo::exists(QStringLiteral("/usr/local/bin/deno"))) {
            denoPath = QStringLiteral("/usr/local/bin/deno");
        }
    }

    if (!denoPath.isEmpty()) {
        info.executablePath = denoPath;
        const auto output = queryProcessOutput(denoPath, {QStringLiteral("--version")});
        if (!output.isEmpty()) {
            info.installed = true;
            // First line e.g. "deno 2.1.2 (release, ...)"
            const auto firstLine = output.split(QLatin1Char('\n')).value(0).trimmed();
            static const QRegularExpression verPattern(QStringLiteral("deno\\s+([0-9]+\\.[0-9]+\\.[0-9]+)"));
            const auto match = verPattern.match(firstLine);
            if (match.hasMatch()) {
                info.installedVersion = match.captured(1);
            } else {
                info.installedVersion = firstLine;
            }
        }
    }

    // 2. Query latest version from GitHub API or redirect
    QString error;
    QString effectiveUrl;
    const auto jsonBytes = httpGet(QStringLiteral("https://api.github.com/repos/denoland/deno/releases/latest"), &effectiveUrl, &error);
    const auto doc = QJsonDocument::fromJson(jsonBytes);
    if (!doc.isNull() && doc.isObject() && doc.object().contains(QStringLiteral("tag_name"))) {
        info.latestVersion = cleanVersionTag(doc.object().value(QStringLiteral("tag_name")).toString());
    } else {
        httpGet(QStringLiteral("https://github.com/denoland/deno/releases/latest"), &effectiveUrl, &error);
        static const QRegularExpression tagPattern(QStringLiteral("/releases/tag/([^/\\s]+)"));
        const auto match = tagPattern.match(effectiveUrl);
        if (match.hasMatch()) {
            info.latestVersion = cleanVersionTag(match.captured(1));
        }
    }

    if (info.installed && !info.latestVersion.isEmpty()) {
        info.updateAvailable = isVersionNewer(info.latestVersion, info.installedVersion);
    }

    info.canAutoUpdate = info.installed;
    info.updateCommand = QStringLiteral("deno upgrade");
    info.installCommand = QStringLiteral("curl -fsSL https://deno.land/install.sh | sh");

    if (!info.installed) {
        info.statusText = QStringLiteral("Not installed (optional; solves YouTube player JavaScript challenges)");
    } else if (info.updateAvailable) {
        info.statusText = QStringLiteral("Update available: %1").arg(info.latestVersion);
    } else {
        info.statusText = QStringLiteral("Up to date (%1)").arg(info.installedVersion);
    }

    return info;
}

DependencyUpdateInfo UpdateChecker::checkFfmpeg() const
{
    DependencyUpdateInfo info;
    info.name = QStringLiteral("FFmpeg");

    const auto path = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (!path.isEmpty()) {
        info.executablePath = path;
        info.installed = true;
        const auto output = queryProcessOutput(path, {QStringLiteral("-version")});
        const auto firstLine = output.split(QLatin1Char('\n')).value(0).trimmed();
        static const QRegularExpression verPattern(QStringLiteral("ffmpeg\\s+version\\s+([^\\s]+)"));
        const auto match = verPattern.match(firstLine);
        if (match.hasMatch()) {
            info.installedVersion = match.captured(1);
        } else {
            info.installedVersion = firstLine;
        }
        info.statusText = QStringLiteral("Installed (%1)").arg(info.installedVersion);
    } else {
        info.installed = false;
        info.statusText = QStringLiteral("Not installed (required for HLS/DASH media downloads)");
        info.installCommand = QStringLiteral("sudo apt install ffmpeg");
    }
    return info;
}

DependencyUpdateInfo UpdateChecker::check7Zip() const
{
    DependencyUpdateInfo info;
    info.name = QStringLiteral("7-Zip");

    QString tool;
    for (const auto& candidate : { QStringLiteral("7zz"), QStringLiteral("7z") }) {
        const auto found = QStandardPaths::findExecutable(candidate);
        if (!found.isEmpty()) {
            tool = found;
            break;
        }
    }

    if (!tool.isEmpty()) {
        info.executablePath = tool;
        info.installed = true;
        const auto output = queryProcessOutput(tool, {});
        const auto firstLine = output.split(QLatin1Char('\n')).value(0).trimmed();
        static const QRegularExpression verPattern(QStringLiteral("7-Zip\\s+(?:\\[[^\\]]+\\]\\s+)?([0-9]+\\.[0-9]+)"));
        const auto match = verPattern.match(firstLine);
        if (match.hasMatch()) {
            info.installedVersion = match.captured(1);
        } else {
            info.installedVersion = firstLine;
        }
        info.statusText = QStringLiteral("Installed (%1)").arg(info.installedVersion);
    } else {
        info.installed = false;
        info.statusText = QStringLiteral("Not installed (required for archive extraction)");
        info.installCommand = QStringLiteral("sudo apt install 7zip");
    }
    return info;
}

UpdateCheckResult UpdateChecker::checkSync()
{
    UpdateCheckResult result;
    result.app = checkAppUpdate();
    result.dependencies.append(checkYtDlp());
    result.dependencies.append(checkDeno());
    result.dependencies.append(checkFfmpeg());
    result.dependencies.append(check7Zip());
    result.checkSucceeded = result.app.checkSucceeded;
    result.errorMessage = result.app.errorMessage;
    return result;
}

void UpdateChecker::checkForUpdates(bool silentIfUpToDate)
{
    if (isChecking_) {
        return;
    }
    isChecking_ = true;
    emit checkStarted();

    // Run network and process inspection in a background worker thread.
    auto* thread = QThread::create([this, silentIfUpToDate] {
        const auto result = const_cast<UpdateChecker*>(this)->checkSync();
        QMetaObject::invokeMethod(this, [this, result, silentIfUpToDate] {
            isChecking_ = false;
            if (result.checkSucceeded) {
                emit checkFinished(result, silentIfUpToDate);
            } else {
                emit checkFailed(result.errorMessage, silentIfUpToDate);
            }
        });
    });

    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void UpdateChecker::runDependencyUpdate(const QString& dependencyName)
{
    QString program;
    QStringList args;

    if (dependencyName == QStringLiteral("yt-dlp")) {
        // Prefer pip upgrade if available, otherwise direct yt-dlp -U
        const auto python3 = QStandardPaths::findExecutable(QStringLiteral("python3"));
        if (!python3.isEmpty()) {
            program = python3;
            args = { QStringLiteral("-m"), QStringLiteral("pip"), QStringLiteral("install"),
                     QStringLiteral("--upgrade"), QStringLiteral("--user"), QStringLiteral("yt-dlp") };
        } else {
            const auto ytdlp = QStandardPaths::findExecutable(QStringLiteral("yt-dlp"));
            program = ytdlp.isEmpty() ? QStringLiteral("yt-dlp") : ytdlp;
            args = { QStringLiteral("-U") };
        }
    } else if (dependencyName == QStringLiteral("Deno")) {
        const auto deno = QStandardPaths::findExecutable(QStringLiteral("deno"));
        program = deno.isEmpty() ? QStringLiteral("deno") : deno;
        args = { QStringLiteral("upgrade") };
    } else {
        emit dependencyUpdateFinished(dependencyName, false,
            QStringLiteral("Automatic in-app update is not supported for %1; please use your package manager.").arg(dependencyName));
        return;
    }

    emit dependencyUpdateStarted(dependencyName);

    auto* process = new QProcess(this);
    process->setProcessChannelMode(QProcess::MergedChannels);

    connect(process, &QProcess::readyReadStandardOutput, this, [this, process, dependencyName] {
        const auto output = QString::fromUtf8(process->readAllStandardOutput());
        emit dependencyUpdateOutput(dependencyName, output);
    });

    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, [this, process, dependencyName](int exitCode, QProcess::ExitStatus exitStatus) {
        const bool success = (exitStatus == QProcess::NormalExit && exitCode == 0);
        const auto msg = success
            ? QStringLiteral("%1 updated successfully.").arg(dependencyName)
            : QStringLiteral("%1 update exited with code %2.").arg(dependencyName).arg(exitCode);
        emit dependencyUpdateFinished(dependencyName, success, msg);
        process->deleteLater();
    });

    connect(process, &QProcess::errorOccurred, this, [this, process, dependencyName](QProcess::ProcessError err) {
        emit dependencyUpdateFinished(dependencyName, false,
            QStringLiteral("Failed to execute update command for %1: error %2").arg(dependencyName).arg(err));
        process->deleteLater();
    });

    process->start(program, args);
}

} // namespace qtidm
