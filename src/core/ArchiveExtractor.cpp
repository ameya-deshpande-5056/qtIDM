#include "core/ArchiveExtractor.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <algorithm>

namespace qtidm {

ArchiveExtractor::ArchiveExtractor(QObject* parent)
    : QObject(parent)
{
}

bool ArchiveExtractor::supports(const QString& path)
{
    const auto lower = path.toLower();
    static const QStringList suffixes {
        QStringLiteral(".7z"), QStringLiteral(".zip"), QStringLiteral(".rar"),
        QStringLiteral(".tar"), QStringLiteral(".tar.gz"), QStringLiteral(".tgz"),
        QStringLiteral(".tar.bz2"), QStringLiteral(".tbz2"), QStringLiteral(".tar.xz"),
        QStringLiteral(".txz"), QStringLiteral(".gz"), QStringLiteral(".bz2"),
        QStringLiteral(".xz")
    };
    return std::any_of(suffixes.cbegin(), suffixes.cend(),
        [&lower](const QString& suffix) { return lower.endsWith(suffix); });
}

bool ArchiveExtractor::listingIsSafe(const QByteArray& listing, QString* error)
{
    bool entriesStarted = false;
    const auto lines = QString::fromUtf8(listing).split(QLatin1Char('\n'));
    const QRegularExpression drivePrefix(QStringLiteral("^[A-Za-z]:[/\\\\]"));
    const QRegularExpression symlinkAttributes(QStringLiteral("(?:^|\\s)l[rwx-]{9}(?:\\s|$)"));
    for (const auto& rawLine : lines) {
        const auto line = rawLine.trimmed();
        if (line.startsWith(QStringLiteral("----------"))) {
            entriesStarted = true;
            continue;
        }
        if (!entriesStarted) {
            continue;
        }
        if (line.startsWith(QStringLiteral("Path = "))) {
            auto path = line.mid(7).trimmed();
            path.replace(QLatin1Char('\\'), QLatin1Char('/'));
            const auto clean = QDir::cleanPath(path);
            if (path.isEmpty() || path.startsWith(QLatin1Char('/'))
                || drivePrefix.match(path).hasMatch() || clean == QStringLiteral("..")
                || clean.startsWith(QStringLiteral("../"))) {
                if (error) {
                    *error = QStringLiteral("Archive contains an unsafe path: %1").arg(path);
                }
                return false;
            }
        } else if ((line.startsWith(QStringLiteral("Attributes = "))
                       && symlinkAttributes.match(line.mid(13)).hasMatch())
            || (line.startsWith(QStringLiteral("Mode = "))
                && line.mid(7).trimmed().startsWith(QLatin1Char('l')))
            || (line.startsWith(QStringLiteral("Symbolic Link = "))
                && !line.mid(16).trimmed().isEmpty())
            || (line.startsWith(QStringLiteral("Hard Link = "))
                && !line.mid(12).trimmed().isEmpty())) {
            if (error) {
                *error = QStringLiteral("Archive contains a symbolic link or hard link, which automatic extraction rejects.");
            }
            return false;
        }
    }
    if (!entriesStarted) {
        if (error) {
            *error = QStringLiteral("Could not parse the archive file listing.");
        }
        return false;
    }
    return true;
}

QString ArchiveExtractor::executable() const
{
    for (const auto& name : { QStringLiteral("7zz"), QStringLiteral("7z") }) {
        const auto found = QStandardPaths::findExecutable(name);
        if (!found.isEmpty()) {
            return found;
        }
    }
    return {};
}

bool ArchiveExtractor::isAvailable() const
{
    return !executable().isEmpty();
}

void ArchiveExtractor::extract(const QString& id, const QString& archivePath,
    const QString& destination, bool deleteArchiveAfterExtraction)
{
    const auto tool = executable();
    if (tool.isEmpty()) {
        emit extractionFailed(id, QStringLiteral("7-Zip is required for automatic archive extraction."));
        return;
    }
    if (!QFileInfo::exists(archivePath) || !supports(archivePath)) {
        emit extractionFailed(id, QStringLiteral("The completed file is not a supported archive: %1").arg(archivePath));
        return;
    }
    const auto absoluteDestination = QFileInfo(destination).absoluteFilePath();
    const QFileInfo destinationInfo(absoluteDestination);
    if (destinationInfo.exists()
        && (destinationInfo.isSymLink() || !destinationInfo.isDir()
            || !QDir(absoluteDestination).isEmpty())) {
        emit extractionFailed(id,
            QStringLiteral("For safe automatic extraction, the destination must be absent or an empty directory: %1")
                .arg(absoluteDestination));
        return;
    }
    const auto parentPath = QFileInfo(absoluteDestination).absolutePath();
    if (!QDir().mkpath(parentPath)) {
        emit extractionFailed(id, QStringLiteral("Could not create archive destination parent %1.").arg(parentPath));
        return;
    }
    auto staging = QSharedPointer<QTemporaryDir>::create(
        QDir(parentPath).filePath(QStringLiteral(".qtidm-extract-XXXXXX")));
    if (!staging->isValid()) {
        emit extractionFailed(id, QStringLiteral("Could not create a private archive extraction directory."));
        return;
    }

    auto* process = new QProcess(this);
    process->setProgram(tool);
    process->setArguments({ QStringLiteral("l"), QStringLiteral("-slt"), QStringLiteral("--"), archivePath });
    process->setProcessChannelMode(QProcess::SeparateChannels);
    jobs_.insert(process, Job {
        id, archivePath, absoluteDestination, staging, deleteArchiveAfterExtraction, true
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
        this, [this, process](int exitCode, QProcess::ExitStatus status) {
            finishProcess(process, exitCode, static_cast<int>(status));
        });
    connect(process, &QProcess::errorOccurred, this, [this, process, id](QProcess::ProcessError processError) {
        if (processError == QProcess::FailedToStart && jobs_.contains(process)) {
            jobs_.remove(process);
            emit extractionFailed(id, QStringLiteral("Could not start 7-Zip."));
            process->deleteLater();
        }
    });
    process->start();
}

void ArchiveExtractor::finishProcess(QProcess* process, int exitCode, int exitStatus)
{
    auto it = jobs_.find(process);
    if (it == jobs_.end()) {
        return;
    }
    const auto job = it.value();
    const auto standardOutput = process->readAllStandardOutput();
    const auto diagnostics = QString::fromUtf8(process->readAllStandardError()).trimmed();
    const bool succeeded = exitStatus == static_cast<int>(QProcess::NormalExit) && exitCode == 0;

    if (job.listing) {
        if (!succeeded) {
            jobs_.erase(it);
            emit extractionFailed(job.id,
                diagnostics.isEmpty() ? QStringLiteral("7-Zip could not inspect the archive.") : diagnostics.right(2000));
            process->deleteLater();
            return;
        }
        QString safetyError;
        if (!listingIsSafe(standardOutput, &safetyError)) {
            jobs_.erase(it);
            emit extractionFailed(job.id, safetyError);
            process->deleteLater();
            return;
        }
        it->listing = false;
        process->disconnect(this);
        process->setArguments({
            QStringLiteral("x"), QStringLiteral("-y"),
            QStringLiteral("-o%1").arg(job.stagingDirectory->path()),
            QStringLiteral("--"), job.archivePath
        });
        connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, [this, process](int code, QProcess::ExitStatus status) {
                finishProcess(process, code, static_cast<int>(status));
            });
        emit extractionStarted(job.id, job.archivePath, job.destination);
        process->start();
        return;
    }

    jobs_.erase(it);
    if (!succeeded) {
        emit extractionFailed(job.id,
            diagnostics.isEmpty() ? QStringLiteral("7-Zip extraction failed.") : diagnostics.right(2000));
    } else {
        const QFileInfo destinationInfo(job.destination);
        if (destinationInfo.exists()
            && (destinationInfo.isSymLink() || !destinationInfo.isDir()
                || !QDir(job.destination).isEmpty())) {
            emit extractionFailed(job.id,
                QStringLiteral("The extraction destination became non-empty; the private extraction was not published."));
            process->deleteLater();
            return;
        }
        if (destinationInfo.exists() && !QDir().rmdir(job.destination)) {
            emit extractionFailed(job.id, QStringLiteral("Could not prepare the empty extraction destination."));
            process->deleteLater();
            return;
        }
        if (!QDir().rename(job.stagingDirectory->path(), job.destination)) {
            emit extractionFailed(job.id, QStringLiteral("Could not publish the extracted archive atomically."));
            process->deleteLater();
            return;
        }
        if (job.deleteArchive && !QFile::remove(job.archivePath)) {
            emit extractionFailed(job.id,
                QStringLiteral("Extraction succeeded, but the archive could not be deleted."));
        } else {
            emit extractionFinished(job.id, job.archivePath, job.destination);
        }
    }
    process->deleteLater();
}

}
