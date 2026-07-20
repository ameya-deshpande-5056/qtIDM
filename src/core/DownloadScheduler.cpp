#include "core/DownloadScheduler.h"

#include "app/Paths.h"
#include "integration/CredentialVault.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QSet>
#include <QUuid>
#include <algorithm>

namespace qtidm {

namespace {
QString availableCompletionPath(const QString& requested)
{
    if (!QFileInfo::exists(requested)) {
        return requested;
    }
    const QFileInfo info(requested);
    const auto base = info.completeBaseName();
    const auto suffix = info.completeSuffix();
    for (int copy = 1; copy < 100000; ++copy) {
        const auto name = suffix.isEmpty()
            ? QStringLiteral("%1 (%2)").arg(base).arg(copy)
            : QStringLiteral("%1 (%2).%3").arg(base).arg(copy).arg(suffix);
        const auto candidate = info.dir().filePath(name);
        if (!QFileInfo::exists(candidate)) {
            return candidate;
        }
    }
    return {};
}

bool moveCompletedFile(const QString& source, const QString& target)
{
    if (QFile::rename(source, target)) {
        return true;
    }
    if (!QFile::copy(source, target)) {
        return false;
    }
    if (QFile::remove(source)) {
        return true;
    }
    QFile::remove(target);
    return false;
}

QString defaultArchiveDestination(const QString& archivePath)
{
    auto name = QFileInfo(archivePath).fileName();
    const auto lower = name.toLower();
    static const QStringList suffixes {
        QStringLiteral(".tar.bz2"), QStringLiteral(".tar.gz"), QStringLiteral(".tar.xz"),
        QStringLiteral(".tbz2"), QStringLiteral(".tgz"), QStringLiteral(".txz"),
        QStringLiteral(".7z"), QStringLiteral(".zip"), QStringLiteral(".rar"),
        QStringLiteral(".tar"), QStringLiteral(".gz"), QStringLiteral(".bz2"), QStringLiteral(".xz")
    };
    for (const auto& suffix : suffixes) {
        if (lower.endsWith(suffix)) {
            name.chop(suffix.size());
            break;
        }
    }
    if (name.isEmpty()) {
        name = QStringLiteral("extracted");
    }
    return QFileInfo(archivePath).dir().filePath(name);
}
}

DownloadScheduler::DownloadScheduler(CurlEpollDownloader& downloader, QObject* parent)
    : QObject(parent)
    , downloader_(downloader)
{
    timer_.setInterval(1000);
    connect(&timer_, &QTimer::timeout, this, &DownloadScheduler::dispatchDue);
    connect(&downloader_, &CurlEpollDownloader::statusChanged,
            this, &DownloadScheduler::downloadStatusChanged);
    connect(&downloader_, &CurlEpollDownloader::downloadAdded, this,
            [this](const DownloadRecord& record) {
                auto it = activeRequests_.find(record.id);
                if (it != activeRequests_.end()) {
                    it->targetPath = record.targetPath;
                    it->entityTag = record.request.entityTag;
                    it->lastModified = record.request.lastModified;
                }
            });
    connect(&downloader_, &CurlEpollDownloader::segmentsChanged, this,
        [this](const QString& id, const QVector<SegmentInfo>& segments) {
            auto it = activeRequests_.find(id);
            if (it != activeRequests_.end()) {
                it->resumeSegments = segments;
                it->segments = segments.isEmpty() ? it->segments : segments.size();
            }
        });
    connect(&downloader_, &CurlEpollDownloader::progressChanged, this,
        [this](const QString& id, qint64, qint64 total, double) {
            auto it = activeRequests_.find(id);
            if (it != activeRequests_.end() && total >= 0) {
                it->expectedTotalBytes = total;
            }
        });
    connect(&downloader_, &CurlEpollDownloader::metadataChanged, this,
        [this](const QString& id, qint64 total, const QString& entityTag, const QString& lastModified) {
            auto it = activeRequests_.find(id);
            if (it != activeRequests_.end()) {
                if (total >= 0) {
                    it->expectedTotalBytes = total;
                }
                it->entityTag = entityTag;
                it->lastModified = lastModified;
            }
        });
    connect(&archiveExtractor_, &ArchiveExtractor::extractionFailed,
        this, &DownloadScheduler::completionAutomationFailed);
    connect(&archiveExtractor_, &ArchiveExtractor::extractionFinished,
        this, [this](const QString& id, const QString&, const QString& destination) {
            emit archiveExtractionFinished(id, destination);
        });
    timer_.start();
}

bool DownloadScheduler::load()
{
    QFile file(queuePath());
    if (!file.exists()) {
        return true;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        lastError_ = file.errorString();
        return false;
    }
    QJsonParseError parseError;
    const auto doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        lastError_ = parseError.errorString();
        return false;
    }
    queue_.clear();
    concurrency_.clear();
    enabled_.clear();
    queueDisplayNames_.clear();
    const auto requests = doc.isArray()
        ? doc.array()
        : doc.object().value(QStringLiteral("requests")).toArray();
    for (const auto& value : requests) {
        auto request = requestFromJson(value.toObject());
        if (request.scheduleId.isEmpty()) {
            request.scheduleId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        }
        queue_.append(std::move(request));
    }
    if (doc.isObject()) {
        const auto queues = doc.object().value(QStringLiteral("queues")).toObject();
        for (auto it = queues.begin(); it != queues.end(); ++it) {
            const auto key = it.key().trimmed().toLower();
            if (key.isEmpty() || !it.value().isObject()) {
                continue;
            }
            const auto config = it.value().toObject();
            concurrency_.insert(key, qBound(0, config.value(QStringLiteral("concurrency")).toInt(0), 64));
            enabled_.insert(key, config.value(QStringLiteral("enabled")).toBool(true));
            queueDisplayNames_.insert(key, config.value(QStringLiteral("name")).toString(it.key()));
        }
    }
    for (const auto& request : queue_) {
        const auto key = request.queueName.toLower();
        queueDisplayNames_.insert(key, request.queueName);
    }
    emit queueChanged();
    return true;
}

bool DownloadScheduler::save() const
{
    QDir().mkpath(Paths::dataDir());
    QFile file(queuePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        lastError_ = file.errorString();
        return false;
    }
    QJsonArray items;
    for (const auto& request : queue_) {
        items.append(requestToJson(request));
    }
    QJsonObject queues;
    QSet<QString> keys;
    for (auto it = queueDisplayNames_.cbegin(); it != queueDisplayNames_.cend(); ++it) {
        keys.insert(it.key());
    }
    for (auto it = concurrency_.cbegin(); it != concurrency_.cend(); ++it) {
        keys.insert(it.key());
    }
    for (auto it = enabled_.cbegin(); it != enabled_.cend(); ++it) {
        keys.insert(it.key());
    }
    for (const auto& key : keys) {
        queues.insert(key, QJsonObject {
            { QStringLiteral("name"), queueDisplayNames_.value(key, key) },
            { QStringLiteral("concurrency"), concurrency_.value(key, 0) },
            { QStringLiteral("enabled"), enabled_.value(key, true) }
        });
    }
    file.write(QJsonDocument(QJsonObject {
        { QStringLiteral("requests"), items },
        { QStringLiteral("queues"), queues }
    }).toJson(QJsonDocument::Indented));
    return true;
}

void DownloadScheduler::schedule(DownloadRequest request)
{
    if (request.scheduleId.isEmpty()) {
        request.scheduleId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    if (request.queueName.trimmed().isEmpty()) {
        request.queueName = QStringLiteral("Main");
    }
    request.queueName = request.queueName.trimmed();
    queueDisplayNames_.insert(request.queueName.toLower(), request.queueName);
    if (!request.scheduledAt.isValid()) {
        request.scheduledAt = QDateTime::currentDateTime();
    }
    queue_.append(std::move(request));
    save();
    emit queueChanged();
    dispatchDue();
}

bool DownloadScheduler::removeScheduled(const QString& scheduleId)
{
    const auto it = std::find_if(queue_.begin(), queue_.end(), [&scheduleId](const DownloadRequest& request) {
        return request.scheduleId == scheduleId;
    });
    if (it == queue_.end()) {
        return false;
    }
    queue_.erase(it);
    save();
    emit queueChanged();
    return true;
}

bool DownloadScheduler::moveScheduled(const QString& scheduleId, int offset)
{
    if (offset == 0) {
        return true;
    }
    const auto it = std::find_if(queue_.begin(), queue_.end(), [&scheduleId](const DownloadRequest& request) {
        return request.scheduleId == scheduleId;
    });
    if (it == queue_.end()) {
        return false;
    }
    const int from = static_cast<int>(std::distance(queue_.begin(), it));
    const int to = qBound(0, from + offset, queue_.size() - 1);
    if (from == to) {
        return true;
    }
    queue_.move(from, to);
    save();
    emit queueChanged();
    return true;
}

bool DownloadScheduler::updateScheduled(const QString& scheduleId, const QString& queueName,
                                        int priority, const QDateTime& scheduledAt)
{
    const auto it = std::find_if(queue_.begin(), queue_.end(), [&scheduleId](const DownloadRequest& request) {
        return request.scheduleId == scheduleId;
    });
    if (it == queue_.end() || !scheduledAt.isValid()) {
        return false;
    }
    it->queueName = queueName.trimmed().isEmpty() ? QStringLiteral("Main") : queueName.trimmed();
    it->priority = qBound(-100, priority, 100);
    it->scheduledAt = scheduledAt;
    queueDisplayNames_.insert(it->queueName.toLower(), it->queueName);
    save();
    emit queueChanged();
    dispatchDue();
    return true;
}

QList<DownloadRequest> DownloadScheduler::queued() const
{
    return queue_;
}

QList<DownloadRequest> DownloadScheduler::queued(const QString& queueName) const
{
    QList<DownloadRequest> result;
    for (const auto& request : queue_) {
        if (request.queueName.compare(queueName, Qt::CaseInsensitive) == 0) {
            result.append(request);
        }
    }
    return result;
}

QStringList DownloadScheduler::queueNames() const
{
    QStringList names;
    for (const auto& name : queueDisplayNames_) {
        if (!names.contains(name, Qt::CaseInsensitive)) {
            names.append(name);
        }
    }
    for (const auto& request : queue_) {
        if (!names.contains(request.queueName, Qt::CaseInsensitive)) {
            names.append(request.queueName);
        }
    }
    names.sort(Qt::CaseInsensitive);
    return names;
}

void DownloadScheduler::setQueueConcurrency(const QString& queueName, int maximum)
{
    const auto normalized = queueName.trimmed().isEmpty() ? QStringLiteral("Main") : queueName.trimmed();
    const auto key = normalized.toLower();
    queueDisplayNames_.insert(key, normalized);
    concurrency_.insert(key, qBound(0, maximum, 64));
    save();
    emit queueChanged();
    dispatchDue();
}

int DownloadScheduler::queueConcurrency(const QString& queueName) const
{
    return concurrency_.value(queueName.trimmed().toLower(), 0);
}

void DownloadScheduler::setQueueEnabled(const QString& queueName, bool enabled)
{
    const auto normalized = queueName.trimmed().isEmpty() ? QStringLiteral("Main") : queueName.trimmed();
    const auto key = normalized.toLower();
    queueDisplayNames_.insert(key, normalized);
    enabled_.insert(key, enabled);
    save();
    emit queueChanged();
    if (enabled) {
        dispatchDue();
    }
}

bool DownloadScheduler::isQueueEnabled(const QString& queueName) const
{
    return enabled_.value(queueName.trimmed().toLower(), true);
}

int DownloadScheduler::activeCount(const QString& queueName) const
{
    return activeByQueue_.value(queueName.trimmed().toLower(), 0);
}

void DownloadScheduler::setCredentialVault(CredentialVault* vault)
{
    credentialVault_ = vault;
    credentialBlocked_.clear();
    dispatchDue();
}

void DownloadScheduler::setMeteredNetworkPolicy(MeteredNetworkPolicy policy)
{
    meteredPolicy_ = policy;
    setNetworkMetered(networkMetered_);
    if (!networkMetered_ || meteredPolicy_ == MeteredNetworkPolicy::Allow) {
        dispatchDue();
    }
}

MeteredNetworkPolicy DownloadScheduler::meteredNetworkPolicy() const
{
    return meteredPolicy_;
}

void DownloadScheduler::setNetworkMetered(bool metered)
{
    networkMetered_ = metered;
    if (networkMetered_ && meteredPolicy_ == MeteredNetworkPolicy::PauseActive) {
        const auto activeIds = downloadQueues_.keys();
        for (const auto& id : activeIds) {
            if (!meteredPausedIds_.contains(id)) {
                meteredPausedIds_.insert(id);
                downloader_.pause(id);
            }
        }
    } else if (!networkMetered_ || meteredPolicy_ != MeteredNetworkPolicy::PauseActive) {
        const auto pausedIds = meteredPausedIds_.values();
        meteredPausedIds_.clear();
        for (const auto& id : pausedIds) {
            auto request = activeRequests_.value(id);
            if (request.url.isValid()) {
                request.existingId = id;
                downloader_.enqueue(std::move(request));
            }
        }
        dispatchDue();
    }
}

bool DownloadScheduler::isNetworkMetered() const
{
    return networkMetered_;
}

bool DownloadScheduler::isAllowedAt(const DownloadRequest& request, const QDateTime& dateTime)
{
    const int weekdayBit = 1 << (dateTime.date().dayOfWeek() - 1);
    if ((request.allowedWeekdays & weekdayBit) == 0) {
        return false;
    }
    if (!request.windowStart.isValid() || !request.windowEnd.isValid()
        || request.windowStart == request.windowEnd) {
        return true;
    }
    const auto time = dateTime.time();
    if (request.windowStart < request.windowEnd) {
        return time >= request.windowStart && time < request.windowEnd;
    }
    return time >= request.windowStart || time < request.windowEnd;
}

QString DownloadScheduler::lastError() const
{
    return lastError_;
}

void DownloadScheduler::dispatchDue()
{
    if (networkMetered_ && meteredPolicy_ != MeteredNetworkPolicy::Allow) {
        return;
    }
    const auto now = QDateTime::currentDateTime();
    bool changed = false;
    QList<int> dueIndexes;
    for (int index = 0; index < queue_.size(); ++index) {
        if (queue_[index].scheduledAt.isValid() && queue_[index].scheduledAt <= now
            && isAllowedAt(queue_[index], now)) {
            dueIndexes.append(index);
        }
    }
    std::stable_sort(dueIndexes.begin(), dueIndexes.end(), [this](int leftIndex, int rightIndex) {
        const auto& left = queue_[leftIndex];
        const auto& right = queue_[rightIndex];
        if (left.priority != right.priority) {
            return left.priority > right.priority;
        }
        return left.scheduledAt < right.scheduledAt;
    });

    QList<int> dispatched;
    for (const int index : dueIndexes) {
        const auto queueKey = queue_[index].queueName.toLower();
        if (!isQueueEnabled(queueKey)) {
            continue;
        }
        const int concurrency = queueConcurrency(queueKey);
        if (concurrency > 0 && activeByQueue_.value(queueKey, 0) >= concurrency) {
            continue;
        }
        auto request = queue_[index];
        if (!request.credentialVaultKey.isEmpty() && request.password.isEmpty()) {
            QString credentialError;
            const auto password = credentialVault_
                ? credentialVault_->lookup(request.credentialVaultKey, &credentialError)
                : QString {};
            if (password.isEmpty()) {
                if (!credentialBlocked_.contains(request.scheduleId)) {
                    credentialBlocked_.insert(request.scheduleId);
                    emit credentialLookupFailed(request.scheduleId,
                        credentialError.isEmpty()
                            ? QStringLiteral("The saved credential could not be loaded from the system vault.")
                            : credentialError);
                }
                continue;
            }
            credentialBlocked_.remove(request.scheduleId);
            request.password = password;
        }
        const auto id = downloader_.enqueue(request);
        downloadQueues_.insert(id, queueKey);
        activeRequests_.insert(id, request);
        activeByQueue_[queueKey] = activeByQueue_.value(queueKey, 0) + 1;
        emit downloadDispatched(id, request);
        if (request.repeatIntervalSeconds > 0) {
            do {
                request.scheduledAt = request.scheduledAt.addSecs(request.repeatIntervalSeconds);
            } while (request.scheduledAt <= now);
            queue_.append(std::move(request));
        }
        dispatched.append(index);
        changed = true;
    }
    std::sort(dispatched.begin(), dispatched.end(), std::greater<int>());
    for (const int index : dispatched) {
        queue_.removeAt(index);
    }
    if (changed) {
        save();
        emit queueChanged();
    }
}

void DownloadScheduler::downloadStatusChanged(const QString& id, DownloadStatus status, const QString&)
{
    if (status == DownloadStatus::Paused && meteredPausedIds_.contains(id)) {
        return;
    }
    if (status != DownloadStatus::Completed && status != DownloadStatus::Failed
        && status != DownloadStatus::Canceled && status != DownloadStatus::Paused) {
        return;
    }
    const auto it = downloadQueues_.find(id);
    if (it == downloadQueues_.end()) {
        return;
    }
    const auto queueKey = it.value();
    downloadQueues_.erase(it);
    meteredPausedIds_.remove(id);
    activeByQueue_[queueKey] = qMax(0, activeByQueue_.value(queueKey) - 1);
    const auto request = activeRequests_.take(id);
    QString completedPath = request.targetPath;
    if (status == DownloadStatus::Completed && !request.completionMoveDirectory.isEmpty()) {
        QDir destination(request.completionMoveDirectory);
        if (!destination.exists() && !QDir().mkpath(destination.absolutePath())) {
            emit completionAutomationFailed(id,
                QStringLiteral("Could not create post-download directory %1").arg(destination.absolutePath()));
        } else {
            auto target = destination.filePath(QFileInfo(request.targetPath).fileName());
            if (QFileInfo(target).absoluteFilePath() != QFileInfo(request.targetPath).absoluteFilePath()) {
                if (QFileInfo::exists(target)) {
                    if (request.fileConflictPolicy == FileConflictPolicy::Overwrite) {
                        QFile::remove(target);
                    } else if (request.fileConflictPolicy == FileConflictPolicy::Skip) {
                        target.clear();
                    } else {
                        target = availableCompletionPath(target);
                    }
                }
                if (!target.isEmpty()) {
                    if (moveCompletedFile(request.targetPath, target)) {
                        completedPath = target;
                        emit completionFileMoved(id, target);
                    } else {
                        emit completionAutomationFailed(id,
                            QStringLiteral("Could not move completed file to %1").arg(target));
                    }
                }
            }
        }
    }
    if (status == DownloadStatus::Completed && !request.completionCommand.isEmpty()) {
        auto command = QProcess::splitCommand(request.completionCommand);
        if (!command.isEmpty()) {
            const auto program = command.takeFirst();
            for (auto& argument : command) {
                argument.replace(QStringLiteral("{file}"), completedPath);
                argument.replace(QStringLiteral("{dir}"), QFileInfo(completedPath).absolutePath());
                argument.replace(QStringLiteral("{url}"), request.url.toString(QUrl::FullyEncoded));
            }
            emit completionCommandRequested(program, command);
        }
    }
    if (status == DownloadStatus::Completed && request.extractArchive) {
        const auto destination = request.archiveDestination.isEmpty()
            ? defaultArchiveDestination(completedPath)
            : request.archiveDestination;
        archiveExtractor_.extract(id, completedPath, destination, request.deleteArchiveAfterExtraction);
    }
    if (status == DownloadStatus::Completed && request.removeRecordOnCompletion) {
        emit historyRemovalRequested(id);
    }
    dispatchDue();
}

QString DownloadScheduler::queuePath() const
{
    return Paths::dataDir() + QStringLiteral("/scheduled-downloads.json");
}

}
