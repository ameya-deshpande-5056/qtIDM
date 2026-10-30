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
#include <functional>
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
    void acknowledgeCheckpoint(const QString& id, const QVector<SegmentInfo>& confirmedSegments);
    void setPersistenceHandler(std::function<bool(const QString& id, const QVector<SegmentInfo>& segments)> handler);

signals:
    void downloadAdded(qtidm::DownloadRecord record);
    void progressChanged(QString id, qint64 received, qint64 total, double bytesPerSecond);
    void segmentsChanged(QString id, QVector<qtidm::SegmentInfo> segments);
    void metadataChanged(QString id, qint64 total, QString entityTag, QString lastModified);
    void statusChanged(QString id, qtidm::DownloadStatus status, QString message);
    void hostConnectionCountChanged(QString host, int activeConnections);

public:
    static bool isPrivateOrLoopbackHost(const QString& host, QString* resolvedPublicIp = nullptr);
    static bool validateRedirectUrl(const QUrl& originalUrl, const QUrl& targetUrl,
                                    bool allowPrivateNetworks, QString* error = nullptr,
                                    QString* pinnedPublicIp = nullptr);
    static QString toNetscapeCookieLine(const CookieRecord& cookie);
    static bool isSafeCrossOriginHeader(const QString& name);
    static void applyCrossOriginHeaderPolicy(QVariantMap* headers, QString* username = nullptr, QString* password = nullptr);

    uint32_t socketGeneration(curl_socket_t socket) const;
    uint32_t currentGeneration() const;
    void setNextGenerationForTesting(uint32_t nextGen);
    bool isStaleEvent(curl_socket_t socket, uint32_t eventGen) const;
    void updateSocket(curl_socket_t socket, int action);
    void removeSocket(curl_socket_t socket);

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
    void applyRateLimits();
    void addTransfer(QueuedRequest queued);
    void addSegmentTransfer(const std::shared_ptr<DownloadBatch>& batch, int segmentIndex, int attempt = 0, bool forceHttp1 = false);
    void startQueuedSegments(const std::shared_ptr<DownloadBatch>& batch);
    bool splitActiveSegment(const std::shared_ptr<DownloadBatch>& batch);
    void refillQueuedSegments();
    int activeConnectionsForHost(const QString& host) const;
    qint64 probeSize(DownloadRequest& request, bool* rangeSupported,
                     QString* entityTag, QString* lastModified, QString* error = nullptr);
    void applyControlRequests();
    void applyAcknowledgedCheckpoints();
    void checkCompleted();
    void retryTransfer(std::unique_ptr<SegmentTransfer> transfer, long responseCode, CURLcode result);
    bool verifyCompletedDownload(const std::shared_ptr<DownloadBatch>& batch, QString* error) const;
    bool publishCompletedDownload(const std::shared_ptr<DownloadBatch>& batch, QString* error) const;

    std::function<bool(const QString& id, const QVector<SegmentInfo>& segments)> persistenceHandler_;
    QMutex checkpointMutex_;
    QHash<QString, QVector<SegmentInfo>> pendingAcks_;

    std::atomic_bool running_ = false;
    std::atomic<qint64> sessionBytesReceived_ = 0;
    std::atomic<qint64> alternateSpeedLimit_ = 0;
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
    std::unordered_map<curl_socket_t, uint32_t> socketGenerations_;
    uint32_t currentGeneration_ = 0;
    QHash<QString, std::shared_ptr<DownloadBatch>> batches_;
    struct DelayedRetry {
        qint64 dueAtMs = 0;
        std::shared_ptr<DownloadBatch> batch;
        int segmentIndex = 0;
        int attempt = 0;
        bool forceHttp1 = false;
    };
    QVector<DelayedRetry> retries_;
};

}
