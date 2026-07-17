#pragma once

#include "core/CurlEpollDownloader.h"
#include "core/DownloadTypes.h"

#include <QObject>
#include <QTimer>

namespace qtidm {

class DownloadScheduler final : public QObject {
    Q_OBJECT
public:
    explicit DownloadScheduler(CurlEpollDownloader& downloader, QObject* parent = nullptr);

    bool load();
    bool save() const;
    void schedule(DownloadRequest request);
    QList<DownloadRequest> queued() const;
    QString lastError() const;

signals:
    void queueChanged();

private slots:
    void dispatchDue();

private:
    QString queuePath() const;

    CurlEpollDownloader& downloader_;
    QList<DownloadRequest> queue_;
    QTimer timer_;
    mutable QString lastError_;
};

}
