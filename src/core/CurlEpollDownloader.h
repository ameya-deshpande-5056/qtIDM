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
    qint64 sessionBytesReceived() const;
    void resetSessionBytesReceived();

signals:
    void downloadAdded(qtidm::DownloadRecord record);
    void progressChanged(QString id, qint64 received, qint64 total, double bytesPerSecond);
    void segmentsChanged(QString id, QVector<qtidm::SegmentInfo> segments);
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
    static int socketCallback(CURL* easy, curl_socket_t socket, int what, void* userp, void* socketp);
    static int timerCallback(CURLM* multi, long timeoutMs, void* userp);
    static size_t writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata);
    static int progressCallback(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t);

    void run();
    void wake();
    void addPendingTransfers();
    void addDueRetries();
    void addTransfer(QueuedRequest queued);
    void addSegmentTransfer(const std::shared_ptr<DownloadBatch>& batch, int segmentIndex, int attempt = 0);
    void startQueuedSegments(const std::shared_ptr<DownloadBatch>& batch);
    void refillQueuedSegments();
    int activeConnectionsForHost(const QString& host) const;
    void updateSocket(curl_socket_t socket, int action);
    void removeSocket(curl_socket_t socket);
    qint64 probeSize(const DownloadRequest& request, bool* rangeSupported);
    void applyControlRequests();
    void checkCompleted();
    void retryTransfer(std::unique_ptr<SegmentTransfer> transfer, long responseCode, CURLcode result);
    bool verifyCompletedDownload(const std::shared_ptr<DownloadBatch>& batch, QString* error) const;
    bool publishCompletedDownload(const std::shared_ptr<DownloadBatch>& batch, QString* error) const;

    std::atomic_bool running_ = false;
    std::atomic<qint64> sessionBytesReceived_ = 0;
    std::jthread thread_;
    QMutex pendingMutex_;
    QQueue<QueuedRequest> pending_;
    QMutex controlMutex_;
    QHash<QString, DownloadStatus> controls_;
    CURLM* multi_ = nullptr;
    int epollFd_ = -1;
    int wakeFd_ = -1;
    std::unordered_map<CURL*, std::unique_ptr<SegmentTransfer>> transfers_;
    std::unordered_map<curl_socket_t, int> sockets_;
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
