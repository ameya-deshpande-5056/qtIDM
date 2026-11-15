#include "integration/AutomationApi.h"

#include "core/CurlEpollDownloader.h"
#include "core/DownloadScheduler.h"
#include "storage/DownloadRepository.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QStorageInfo>

namespace qtidm {

namespace {

QString compact(const QJsonObject& object)
{
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

QJsonObject skippedEntry(const QString& id, const QString& why)
{
    return QJsonObject {
        { QStringLiteral("id"), id },
        { QStringLiteral("why"), why }
    };
}

QJsonObject answer(const QString& action, const QJsonArray& done, const QJsonArray& skipped)
{
    return QJsonObject {
        { QStringLiteral("action"), action },
        { QStringLiteral("done"), done },
        { QStringLiteral("skipped"), skipped }
    };
}

QJsonValue freeSpace()
{
    const QStorageInfo storage(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
    if (!storage.isValid() || !storage.isReady()) {
        return {};
    }
    return QJsonValue(static_cast<qint64>(storage.bytesAvailable()));
}

}

AutomationApi::AutomationApi(CurlEpollDownloader& downloader, DownloadScheduler& scheduler,
                             DownloadRepository& repository, QObject* parent)
    : QObject(parent)
    , downloader_(downloader)
    , scheduler_(scheduler)
    , repository_(repository)
{
    // Only the engine knows a live rate, so keep the last sample for the list
    // answer and drop it once a transfer leaves the active states.
    connect(&downloader_, &CurlEpollDownloader::progressChanged, this,
            [this](const QString& id, qint64, qint64, double bytesPerSecond) {
                speeds_.insert(id, bytesPerSecond);
            });
    connect(&downloader_, &CurlEpollDownloader::statusChanged, this,
            [this](const QString& id, DownloadStatus status, const QString&) {
                if (!isActive(status)) {
                    speeds_.remove(id);
                }
            });
}

void AutomationApi::setMediaHandler(MediaHandler handler)
{
    mediaHandler_ = std::move(handler);
}

bool AutomationApi::isActive(DownloadStatus status)
{
    return status == DownloadStatus::Queued || status == DownloadStatus::Connecting
        || status == DownloadStatus::Downloading;
}

bool AutomationApi::isResumable(DownloadStatus status)
{
    return status == DownloadStatus::Paused || status == DownloadStatus::Failed
        || status == DownloadStatus::Canceled;
}

bool AutomationApi::isStoppable(DownloadStatus status)
{
    return status == DownloadStatus::Queued || isActive(status);
}

const DownloadRecord* AutomationApi::findRecord(const QList<DownloadRecord>& records, const QString& id)
{
    for (const auto& record : records) {
        if (record.id == id) {
            return &record;
        }
    }
    return nullptr;
}

QString AutomationApi::errorJson(const QString& message, const QString& code)
{
    QJsonObject object { { QStringLiteral("error"), message } };
    if (!code.isEmpty()) {
        object.insert(QStringLiteral("code"), code);
    }
    return compact(object);
}

QString AutomationApi::listJson() const
{
    QJsonArray downloads;
    qint64 totalSpeed = 0;
    for (const auto& record : repository_.listDownloads()) {
        const auto speed = static_cast<qint64>(speeds_.value(record.id, 0.0));
        if (isActive(record.status)) {
            totalSpeed += speed;
        }
        downloads.append(QJsonObject {
            { QStringLiteral("id"), record.id },
            { QStringLiteral("file"), QFileInfo(record.targetPath).fileName() },
            { QStringLiteral("url"), record.url.toString(QUrl::FullyEncoded) },
            { QStringLiteral("path"), record.targetPath },
            { QStringLiteral("status"), toStorageValue(record.status) },
            { QStringLiteral("size"), record.totalBytes >= 0
                  ? QJsonValue(static_cast<qint64>(record.totalBytes))
                  : QJsonValue() },
            { QStringLiteral("downloaded"), static_cast<qint64>(record.completedBytes) },
            { QStringLiteral("speed"), speed },
            { QStringLiteral("added"), record.createdAt.toUTC().toString(Qt::ISODate) },
            { QStringLiteral("category"), record.category },
            { QStringLiteral("queue"), record.request.queueName }
        });
    }

    // The Main queue exists from the first start, even before anything has been
    // queued, so scripts can rely on it being listed.
    auto queueNames = scheduler_.queueNames();
    if (!queueNames.contains(QStringLiteral("Main"), Qt::CaseInsensitive)) {
        queueNames.append(QStringLiteral("Main"));
        queueNames.sort(Qt::CaseInsensitive);
    }
    QJsonArray queues;
    for (const auto& name : queueNames) {
        queues.append(QJsonObject {
            { QStringLiteral("name"), name },
            { QStringLiteral("running"), scheduler_.isQueueEnabled(name) },
            { QStringLiteral("active"), scheduler_.activeCount(name) },
            { QStringLiteral("pending"), scheduler_.queued(name).size() }
        });
    }

    return compact(QJsonObject {
        { QStringLiteral("downloads"), downloads },
        { QStringLiteral("speed"), totalSpeed },
        { QStringLiteral("queues"), queues },
        { QStringLiteral("free"), freeSpace() }
    });
}

QString AutomationApi::pauseJson(const QStringList& ids)
{
    QJsonArray done;
    QJsonArray skipped;
    const auto records = repository_.listDownloads();
    for (const auto& id : ids) {
        const auto* record = findRecord(records, id);
        if (!record) {
            skipped.append(skippedEntry(id, QStringLiteral("unknown id")));
            continue;
        }
        if (!isActive(record->status)) {
            skipped.append(skippedEntry(id, QStringLiteral("not active")));
            continue;
        }
        if (mediaHandler_) {
            const auto handled = mediaHandler_(id, MediaAction::Pause);
            if (handled.has_value()) {
                if (*handled) {
                    done.append(id);
                } else {
                    skipped.append(skippedEntry(id, QStringLiteral("media control failed")));
                }
                continue;
            }
        }
        downloader_.pause(id);
        done.append(id);
    }
    return compact(answer(QStringLiteral("pause"), done, skipped));
}

QString AutomationApi::startJson(const QStringList& ids)
{
    QJsonArray done;
    QJsonArray skipped;
    const auto records = repository_.listDownloads();
    for (const auto& id : ids) {
        const auto* record = findRecord(records, id);
        if (!record) {
            skipped.append(skippedEntry(id, QStringLiteral("unknown id")));
            continue;
        }
        if (!isResumable(record->status)) {
            skipped.append(skippedEntry(id, record->status == DownloadStatus::Completed
                    ? QStringLiteral("already complete")
                    : QStringLiteral("not resumable")));
            continue;
        }
        if (mediaHandler_) {
            const auto handled = mediaHandler_(id, MediaAction::Start);
            if (handled.has_value()) {
                if (*handled) {
                    done.append(id);
                } else {
                    skipped.append(skippedEntry(id, QStringLiteral("media control failed")));
                }
                continue;
            }
        }
        bool alreadyQueued = false;
        for (const auto& queued : scheduler_.queued()) {
            if (queued.existingId == id) {
                alreadyQueued = true;
                break;
            }
        }
        if (alreadyQueued) {
            skipped.append(skippedEntry(id, QStringLiteral("already queued")));
            continue;
        }
        // The same reconstruction the desktop app uses for a selected resume.
        DownloadRequest request = record->request;
        request.existingId = record->id;
        request.url = record->url;
        request.targetPath = record->targetPath;
        request.category = record->category;
        request.expectedTotalBytes = record->totalBytes;
        request.resumeSegments = record->segments;
        request.segments = record->request.segments > 0 ? record->request.segments : 8;
        request.scheduledAt = QDateTime::currentDateTime();
        scheduler_.schedule(std::move(request));
        done.append(id);
    }
    return compact(answer(QStringLiteral("start"), done, skipped));
}

QString AutomationApi::cancelAndForget(const QStringList& ids, bool deleteFiles)
{
    const auto action = deleteFiles ? QStringLiteral("delete") : QStringLiteral("remove");
    QJsonArray done;
    QJsonArray skipped;
    const auto records = repository_.listDownloads();
    for (const auto& id : ids) {
        const auto* record = findRecord(records, id);
        if (!record) {
            skipped.append(skippedEntry(id, QStringLiteral("unknown id")));
            continue;
        }
        if (isStoppable(record->status)) {
            bool mediaControlled = false;
            if (mediaHandler_) {
                mediaControlled = mediaHandler_(id, MediaAction::Cancel).has_value();
            }
            if (!mediaControlled) {
                downloader_.cancel(id);
            }
        }
        const auto targetPath = record->targetPath;
        if (deleteFiles) {
            QFile::remove(targetPath);
            QFile::remove(targetPath + QStringLiteral(".part"));
        }
        if (!repository_.removeDownload(id)) {
            skipped.append(skippedEntry(id, repository_.lastError().isEmpty()
                    ? QStringLiteral("could not remove the entry")
                    : repository_.lastError()));
            continue;
        }
        speeds_.remove(id);
        done.append(id);
    }
    return compact(answer(action, done, skipped));
}

QString AutomationApi::removeJson(const QStringList& ids)
{
    return cancelAndForget(ids, false);
}

QString AutomationApi::deleteJson(const QStringList& ids)
{
    return cancelAndForget(ids, true);
}

QString AutomationApi::startQueueJson(const QString& queueName)
{
    const auto name = queueName.trimmed();
    if (name.isEmpty()) {
        return errorJson(QStringLiteral("A queue name is required."), QStringLiteral("invalid-queue"));
    }
    if (name.compare(QStringLiteral("Main"), Qt::CaseInsensitive) != 0
        && !scheduler_.queueNames().contains(name, Qt::CaseInsensitive)) {
        return errorJson(QStringLiteral("Unknown queue: %1").arg(name), QStringLiteral("unknown-queue"));
    }
    scheduler_.setQueueEnabled(name, true);
    return compact(QJsonObject {
        { QStringLiteral("queue"), name },
        { QStringLiteral("running"), scheduler_.isQueueEnabled(name) }
    });
}

QString AutomationApi::stopQueueJson(const QString& queueName)
{
    const auto name = queueName.trimmed();
    if (name.isEmpty()) {
        return errorJson(QStringLiteral("A queue name is required."), QStringLiteral("invalid-queue"));
    }
    if (name.compare(QStringLiteral("Main"), Qt::CaseInsensitive) != 0
        && !scheduler_.queueNames().contains(name, Qt::CaseInsensitive)) {
        return errorJson(QStringLiteral("Unknown queue: %1").arg(name), QStringLiteral("unknown-queue"));
    }
    scheduler_.setQueueEnabled(name, false);
    return compact(QJsonObject {
        { QStringLiteral("queue"), name },
        { QStringLiteral("running"), scheduler_.isQueueEnabled(name) }
    });
}

QString AutomationApi::handleVerb(const QString& verb, const QStringList& arguments)
{
    if (verb == QLatin1String("list") || verb == QLatin1String("status")) {
        return listJson();
    }
    if (verb == QLatin1String("pause")) {
        return pauseJson(arguments);
    }
    if (verb == QLatin1String("start")) {
        return startJson(arguments);
    }
    if (verb == QLatin1String("remove")) {
        return removeJson(arguments);
    }
    if (verb == QLatin1String("delete")) {
        return deleteJson(arguments);
    }
    if (verb == QLatin1String("start-queue")) {
        return startQueueJson(arguments.value(0));
    }
    if (verb == QLatin1String("stop-queue")) {
        return stopQueueJson(arguments.value(0));
    }
    return errorJson(QStringLiteral("Unknown verb: %1").arg(verb), QStringLiteral("unknown-verb"));
}

}
