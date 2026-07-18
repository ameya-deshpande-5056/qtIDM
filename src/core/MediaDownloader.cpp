#include "core/MediaDownloader.h"

#include "app/Logger.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <csignal>

namespace qtidm {

namespace {

QString makeId(const QUrl& url, const QString& targetPath)
{
    const auto seed = url.toString() + QLatin1Char('|') + targetPath
        + QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    return QStringLiteral("media-")
        + QString::fromLatin1(QCryptographicHash::hash(seed.toUtf8(), QCryptographicHash::Sha256).toHex().left(16));
}

QString headerValue(const QVariantMap& headers, QStringView name)
{
    for (auto it = headers.cbegin(); it != headers.cend(); ++it) {
        if (QStringView(it.key()).compare(name, Qt::CaseInsensitive) == 0) {
            return it.value().toString();
        }
    }
    return {};
}

QString ffmpegHeaders(const QVariantMap& headers, const QSet<QString>& excluded)
{
    QStringList lines;
    for (auto it = headers.cbegin(); it != headers.cend(); ++it) {
        const auto value = it.value().toString();
        if (!excluded.contains(it.key().toLower()) && !value.isEmpty()
            && !it.key().contains(QLatin1Char('\r')) && !it.key().contains(QLatin1Char('\n'))
            && !value.contains(QLatin1Char('\r')) && !value.contains(QLatin1Char('\n'))) {
            lines.append(it.key() + QStringLiteral(": ") + value);
        }
    }
    return lines.isEmpty() ? QString {} : lines.join(QStringLiteral("\r\n")) + QStringLiteral("\r\n");
}

QString mediaStagingPath(const QString& targetPath)
{
    const QFileInfo info(targetPath);
    const auto suffix = info.completeSuffix();
    const auto stagedName = suffix.isEmpty()
        ? info.fileName() + QStringLiteral(".part")
        : info.completeBaseName() + QStringLiteral(".part.") + suffix;
    return info.dir().filePath(stagedName);
}

}

MediaDownloader::MediaDownloader(QObject* parent)
    : QObject(parent)
{
}

MediaDownloader::~MediaDownloader()
{
    const auto jobs = jobs_;
    jobs_.clear();
    for (const auto& job : jobs) {
        job.process->disconnect(this);
        job.process->terminate();
        if (!job.process->waitForFinished(2000)) {
            job.process->kill();
            job.process->waitForFinished(1000);
        }
    }
}

bool MediaDownloader::supports(const QUrl& url, const QString& mediaTypeHint)
{
    const auto normalizedHint = mediaTypeHint.trimmed().toUpper();
    if (normalizedHint == QStringLiteral("HLS") || normalizedHint == QStringLiteral("DASH")) {
        return true;
    }
    const auto value = url.path(QUrl::FullyDecoded).toLower();
    return value.contains(QStringLiteral(".m3u8")) || value.contains(QStringLiteral(".mpd"));
}

bool MediaDownloader::declaresUnsupportedDrm(const QByteArray& manifest)
{
    const auto lower = manifest.toLower();
    return lower.contains("urn:uuid:edef8ba9-79d6-4ace-a3c8-27dcd51d21ed") // Widevine
        || lower.contains("urn:uuid:9a04f079-9840-4286-ab92-e65be0885f95") // PlayReady
        || lower.contains("com.apple.streamingkeydelivery") // FairPlay
        || lower.contains("method=sample-aes");
}

QStringList MediaDownloader::httpInputArguments(const QVariantMap& headers)
{
    QStringList arguments;
    const auto userAgent = headerValue(headers, u"User-Agent");
    if (!userAgent.isEmpty()) {
        arguments << QStringLiteral("-user_agent") << userAgent;
    }
    const auto referer = headerValue(headers, u"Referer");
    if (!referer.isEmpty()) {
        arguments << QStringLiteral("-referer") << referer;
    }
    const auto headersText = ffmpegHeaders(
        headers, { QStringLiteral("user-agent"), QStringLiteral("referer") });
    if (!headersText.isEmpty()) {
        arguments << QStringLiteral("-headers") << headersText;
    }
    return arguments;
}

QString MediaDownloader::enqueue(const QUrl& url, const QString& targetPath, const QVariantMap& headers, const QString& existingId)
{
    const auto id = existingId.isEmpty() ? makeId(url, targetPath) : existingId;
    DownloadRecord record;
    record.id = id;
    record.url = url;
    record.targetPath = targetPath;
    record.category = QStringLiteral("Video");
    record.status = DownloadStatus::Connecting;
    record.createdAt = QDateTime::currentDateTimeUtc();
    record.updatedAt = record.createdAt;
    record.request.url = url;
    record.request.targetPath = targetPath;
    record.request.category = record.category;
    record.request.headers = headers;
    emit downloadAdded(record);

    if (url.isLocalFile()) {
        QFile manifest(url.toLocalFile());
        if (manifest.open(QIODevice::ReadOnly)
            && declaresUnsupportedDrm(manifest.read(2 * 1024 * 1024))) {
            const auto message = QStringLiteral("This manifest declares DRM-protected media. qtIDM does not bypass DRM.");
            emit statusChanged(id, DownloadStatus::Failed, message);
            Logger::error(QStringLiteral("Media download %1").arg(id), message);
            return id;
        }
    }

    const auto ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty()) {
        const auto message = QStringLiteral("FFmpeg is required for HLS/DASH media downloads. Install the ffmpeg package.");
        emit statusChanged(id, DownloadStatus::Failed, message);
        Logger::error(QStringLiteral("Media download %1").arg(id), message);
        return id;
    }

    auto* process = new QProcess(this);
    process->setProgram(ffmpeg);
    process->setProcessChannelMode(QProcess::SeparateChannels);
    QDir().mkpath(QFileInfo(targetPath).absolutePath());
    const auto stagingPath = mediaStagingPath(targetPath);
    QFile::remove(stagingPath);

    QStringList arguments {
        QStringLiteral("-hide_banner"),
        QStringLiteral("-nostdin"),
        QStringLiteral("-y"),
        QStringLiteral("-loglevel"),
        QStringLiteral("warning"),
        QStringLiteral("-progress"),
        QStringLiteral("pipe:1"),
        QStringLiteral("-nostats")
    };
    arguments << httpInputArguments(headers);
    if (url.scheme() == QStringLiteral("http") || url.scheme() == QStringLiteral("https")) {
        arguments << QStringLiteral("-reconnect") << QStringLiteral("1")
                  << QStringLiteral("-reconnect_streamed") << QStringLiteral("1")
                  << QStringLiteral("-reconnect_on_network_error") << QStringLiteral("1")
                  << QStringLiteral("-reconnect_on_http_error") << QStringLiteral("408,429,5xx")
                  << QStringLiteral("-reconnect_delay_max") << QStringLiteral("30");
    }
    arguments << QStringLiteral("-i") << url.toString()
              << QStringLiteral("-map") << QStringLiteral("0:v?")
              << QStringLiteral("-map") << QStringLiteral("0:a?")
              << QStringLiteral("-map") << QStringLiteral("0:s?")
              << QStringLiteral("-map_metadata") << QStringLiteral("0")
              << QStringLiteral("-map_chapters") << QStringLiteral("0")
              << QStringLiteral("-c") << QStringLiteral("copy");
    const auto suffix = QFileInfo(targetPath).suffix().toLower();
    if (suffix == QStringLiteral("mp4") || suffix == QStringLiteral("mov")) {
        arguments << QStringLiteral("-movflags") << QStringLiteral("+faststart");
    }
    arguments << stagingPath;
    process->setArguments(arguments);

    jobs_.insert(id, Job { process, targetPath, stagingPath, {}, 0, QDateTime::currentMSecsSinceEpoch(), false, false });

    connect(process, &QProcess::started, this, [this, id] {
        emit statusChanged(id, DownloadStatus::Downloading, QStringLiteral("Downloading and muxing adaptive media"));
    });
    connect(process, &QProcess::readyReadStandardError, this, [this, id] { collectOutput(id); });
    connect(process, &QProcess::readyReadStandardOutput, this, [this, id] { collectOutput(id); });
    connect(process, &QProcess::errorOccurred, this, [this, id](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            finish(id, false, QStringLiteral("FFmpeg could not be started."));
        }
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, [this, id](int exitCode, QProcess::ExitStatus status) {
                finish(id, status == QProcess::NormalExit && exitCode == 0);
            });
    process->start();
    return id;
}

bool MediaDownloader::contains(const QString& id) const
{
    return jobs_.contains(id);
}

void MediaDownloader::pause(const QString& id)
{
    auto it = jobs_.find(id);
    if (it == jobs_.end() || it->process->state() == QProcess::NotRunning || it->paused) {
        return;
    }
    if (::kill(static_cast<pid_t>(it->process->processId()), SIGSTOP) == 0) {
        it->paused = true;
        emit statusChanged(id, DownloadStatus::Paused, QStringLiteral("Adaptive media download paused"));
    }
}

void MediaDownloader::resume(const QString& id)
{
    auto it = jobs_.find(id);
    if (it == jobs_.end() || it->process->state() == QProcess::NotRunning || !it->paused) {
        return;
    }
    if (::kill(static_cast<pid_t>(it->process->processId()), SIGCONT) == 0) {
        it->paused = false;
        it->lastTick = QDateTime::currentMSecsSinceEpoch();
        emit statusChanged(id, DownloadStatus::Downloading, QStringLiteral("Adaptive media download resumed"));
    }
}

void MediaDownloader::cancel(const QString& id)
{
    const auto it = jobs_.find(id);
    if (it == jobs_.end()) {
        return;
    }
    it->canceled = true;
    if (it->paused) {
        ::kill(static_cast<pid_t>(it->process->processId()), SIGCONT);
        it->paused = false;
    }
    auto* process = it->process;
    process->terminate();
    QTimer::singleShot(3000, process, [process] {
        if (process->state() != QProcess::NotRunning) {
            process->kill();
        }
    });
}

void MediaDownloader::collectOutput(const QString& id)
{
    auto it = jobs_.find(id);
    if (it == jobs_.end()) {
        return;
    }
    it->diagnostics.append(it->process->readAllStandardError());
    it->process->readAllStandardOutput();
    if (it->diagnostics.size() > 64 * 1024) {
        it->diagnostics = it->diagnostics.right(64 * 1024);
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 bytes = QFileInfo(it->stagingPath).size();
    const qint64 elapsed = now - it->lastTick;
    const double speed = elapsed > 0 ? (bytes - it->lastBytes) * 1000.0 / elapsed : 0.0;
    it->lastBytes = bytes;
    it->lastTick = now;
    emit progressChanged(id, bytes, -1, speed);
}

void MediaDownloader::finish(const QString& id, bool succeeded, const QString& error)
{
    auto it = jobs_.find(id);
    if (it == jobs_.end()) {
        return;
    }
    collectOutput(id);
    const auto process = it->process;
    const auto diagnostics = QString::fromUtf8(it->diagnostics).trimmed();
    const auto bytes = QFileInfo(it->stagingPath).size();
    const bool canceled = it->canceled;
    const auto targetPath = it->targetPath;
    const auto stagingPath = it->stagingPath;
    jobs_.erase(it);

    if (canceled) {
        emit statusChanged(id, DownloadStatus::Canceled, QStringLiteral("Media download canceled"));
    } else if (succeeded) {
        if ((QFileInfo::exists(targetPath) && !QFile::remove(targetPath))
            || !QFile::rename(stagingPath, targetPath)) {
            const auto message = QStringLiteral("Media was downloaded but could not be published from %1 to %2")
                                     .arg(stagingPath, targetPath);
            emit statusChanged(id, DownloadStatus::Failed, message);
            Logger::error(QStringLiteral("Media download %1").arg(id), message);
        } else {
            emit progressChanged(id, bytes, bytes, 0.0);
            emit statusChanged(id, DownloadStatus::Completed, QStringLiteral("Adaptive media download complete"));
        }
    } else {
        const bool drmDiagnostic = diagnostics.contains(QStringLiteral("widevine"), Qt::CaseInsensitive)
            || diagnostics.contains(QStringLiteral("playready"), Qt::CaseInsensitive)
            || diagnostics.contains(QStringLiteral("fairplay"), Qt::CaseInsensitive)
            || diagnostics.contains(QStringLiteral("DRM"), Qt::CaseInsensitive);
        const auto message = drmDiagnostic
            ? QStringLiteral("The media appears to be DRM-protected. qtIDM does not bypass DRM.")
            : !error.isEmpty() ? error
            : diagnostics.isEmpty() ? QStringLiteral("FFmpeg failed without diagnostic output.")
                                    : QStringLiteral("FFmpeg failed: %1").arg(diagnostics.right(2000));
        emit statusChanged(id, DownloadStatus::Failed, message);
        Logger::error(QStringLiteral("Media download %1").arg(id), message);
    }
    process->deleteLater();
}

}
