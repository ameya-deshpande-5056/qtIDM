#pragma once

#include "core/ArchiveExtractor.h"
#include "core/CurlEpollDownloader.h"
#include "core/DownloadTypes.h"

#include <QObject>
#include <QHash>
#include <QSet>
#include <QTimer>

namespace qtidm {

class CredentialVault;

enum class MeteredNetworkPolicy {
    Allow,
    HoldNew,
    PauseActive
};

class DownloadScheduler final : public QObject {
    Q_OBJECT
public:
    explicit DownloadScheduler(CurlEpollDownloader& downloader, QObject* parent = nullptr);

    bool load();
    bool save() const;
    void schedule(DownloadRequest request);
    bool removeScheduled(const QString& scheduleId);
    bool moveScheduled(const QString& scheduleId, int offset);
    bool updateScheduled(const QString& scheduleId, const QString& queueName,
                         int priority, const QDateTime& scheduledAt);
    QList<DownloadRequest> queued() const;
    QList<DownloadRequest> queued(const QString& queueName) const;
    QStringList queueNames() const;
    void setQueueConcurrency(const QString& queueName, int maximum);
    int queueConcurrency(const QString& queueName) const;
    void setQueueEnabled(const QString& queueName, bool enabled);
    bool isQueueEnabled(const QString& queueName) const;
    int activeCount(const QString& queueName) const;
    void setCredentialVault(CredentialVault* vault);
    void setMeteredNetworkPolicy(MeteredNetworkPolicy policy);
    MeteredNetworkPolicy meteredNetworkPolicy() const;
    void setNetworkMetered(bool metered);
    bool isNetworkMetered() const;
    static bool isAllowedAt(const DownloadRequest& request, const QDateTime& dateTime);
    QString lastError() const;

signals:
    void queueChanged();
    void downloadDispatched(QString id, qtidm::DownloadRequest request);
    void completionCommandRequested(QString program, QStringList arguments);
    void completionFileMoved(QString id, QString targetPath);
    void completionAutomationFailed(QString id, QString message);
    void archiveExtractionFinished(QString id, QString destination);
    void credentialLookupFailed(QString scheduleId, QString message);
    void historyRemovalRequested(QString id);

private slots:
    void dispatchDue();
    void downloadStatusChanged(const QString& id, DownloadStatus status, const QString& message);

private:
    QString queuePath() const;

    CurlEpollDownloader& downloader_;
    QList<DownloadRequest> queue_;
    QHash<QString, int> concurrency_;
    QHash<QString, bool> enabled_;
    QHash<QString, QString> queueDisplayNames_;
    QHash<QString, int> activeByQueue_;
    QHash<QString, QString> downloadQueues_;
    QHash<QString, DownloadRequest> activeRequests_;
    QSet<QString> meteredPausedIds_;
    QSet<QString> credentialBlocked_;
    QTimer timer_;
    ArchiveExtractor archiveExtractor_;
    CredentialVault* credentialVault_ = nullptr;
    MeteredNetworkPolicy meteredPolicy_ = MeteredNetworkPolicy::Allow;
    bool networkMetered_ = false;
    mutable QString lastError_;
};

}
