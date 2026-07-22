#pragma once

#include "core/DownloadTypes.h"
#include "core/SparseFileWriter.h"

#include <QObject>
#include <QHash>
#include <QMutex>
#include <QQueue>
#include <QSet>
#include <atomic>
#include <curl/curl.h>
#include <memory>
#include <thread>
#include <unordered_map>

namespace qtidm {

class CurlEpollDownloader final : public QObject {
    Q_OBJECT
public:
    explicit CurlEpollDownloader(QObject* parent = nullptr);
    ~CurlEpollDownloader() override;

    void start();
    void stop();
    QString enqueue(DownloadRequest request);
    void pause(const QString& id);
    void cancel(const QString& id);
    void setAlternateSpeedLimit(qint64 bytesPerSecond);
    qint64 alternateSpeedLimit() const;
    qint64 sessionBytesReceived() const;
    void resetSessionBytesReceived();

signals:
    void downloadAdded(qtidm::DownloadRecord record);
    void progressChanged(QString id, qint64 received, qint64 total, double bytesPerSecond);
    void segmentsChanged(QString id, QVector<qtidm::SegmentInfo> segments);
    void metadataChanged(QString id, qint64 total, QString entityTag, QString lastModified);
    void statusChanged(QString id, qtidm::DownloadStatus status, QString message);
    void hostConnectionCountChanged(QString host, int activeConnections);

public:
    struct DownloadBatch;
    struct SegmentTransfer;
    struct QueuedRequest {
        QString id;
        DownloadRequest request;
    };

private:
    static size_t writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata);
    static int progressCallback(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t);

    void run();
    void wake();
    void addPendingTransfers();
    void addDueRetries();
    void applyRateLimits();
    void addTransfer(QueuedRequest queued);
    void addSegmentTransfer(const std::shared_ptr<DownloadBatch>& batch, int segmentIndex, int attempt = 0);
    void startQueuedSegments(const std::shared_ptr<DownloadBatch>& batch);
    void refillQueuedSegments();
    int activeConnectionsForHost(const QString& host) const;
    qint64 probeSize(const DownloadRequest& request, bool* rangeSupported,
                     QString* entityTag, QString* lastModified);
    void applyControlRequests();
    void checkCompleted();
    void retryTransfer(std::unique_ptr<SegmentTransfer> transfer, long responseCode, CURLcode result);
    bool verifyCompletedDownload(const std::shared_ptr<DownloadBatch>& batch, QString* error) const;
    bool publishCompletedDownload(const std::shared_ptr<DownloadBatch>& batch, QString* error) const;

    std::atomic_bool running_ = false;
    std::atomic<qint64> sessionBytesReceived_ = 0;
    std::atomic<qint64> alternateSpeedLimit_ = 0;
    std::jthread thread_;
    QMutex pendingMutex_;
    QQueue<QueuedRequest> pending_;
    QMutex controlMutex_;
    QHash<QString, DownloadStatus> controls_;
    CURLM* multi_ = nullptr;
    std::unordered_map<CURL*, std::unique_ptr<SegmentTransfer>> transfers_;
    QHash<QString, std::shared_ptr<DownloadBatch>> batches_;
    struct DelayedRetry {
        qint64 dueAtMs = 0;
        std::shared_ptr<DownloadBatch> batch;
        int segmentIndex = 0;
        int attempt = 0;
    };
    QVector<DelayedRetry> retries_;
};

}