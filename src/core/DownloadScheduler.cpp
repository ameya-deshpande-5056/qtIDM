#include "core/DownloadScheduler.h"

#include "app/Paths.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>

namespace qtidm {

DownloadScheduler::DownloadScheduler(CurlEpollDownloader& downloader, QObject* parent)
    : QObject(parent)
    , downloader_(downloader)
{
    timer_.setInterval(1000);
    connect(&timer_, &QTimer::timeout, this, &DownloadScheduler::dispatchDue);
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
    const auto doc = QJsonDocument::fromJson(file.readAll());
    queue_.clear();
    for (const auto& value : doc.array()) {
        queue_.append(requestFromJson(value.toObject()));
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
    file.write(QJsonDocument(items).toJson(QJsonDocument::Indented));
    return true;
}

void DownloadScheduler::schedule(DownloadRequest request)
{
    if (!request.scheduledAt.isValid() || request.scheduledAt <= QDateTime::currentDateTime()) {
        downloader_.enqueue(std::move(request));
        return;
    }
    queue_.append(std::move(request));
    save();
    emit queueChanged();
}

QList<DownloadRequest> DownloadScheduler::queued() const
{
    return queue_;
}

QString DownloadScheduler::lastError() const
{
    return lastError_;
}

void DownloadScheduler::dispatchDue()
{
    const auto now = QDateTime::currentDateTime();
    bool changed = false;
    for (int i = queue_.size() - 1; i >= 0; --i) {
        if (queue_[i].scheduledAt.isValid() && queue_[i].scheduledAt <= now) {
            downloader_.enqueue(queue_.takeAt(i));
            changed = true;
        }
    }
    if (changed) {
        save();
        emit queueChanged();
    }
}

QString DownloadScheduler::queuePath() const
{
    return Paths::dataDir() + QStringLiteral("/scheduled-downloads.json");
}

}
