#pragma once

#include "core/DownloadTypes.h"
#include "core/HlsPlaylistParser.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariantMap>
#include <QVector>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace qtidm {

class HlsDownloader final : public QObject {
    Q_OBJECT
public:
    explicit HlsDownloader(QObject* parent = nullptr);
    ~HlsDownloader() override;

    bool start(const QString& id, const QUrl& playlistUrl, const QString& targetPath,
               const QVariantMap& headers = {}, const QString& dohUrl = {}, int concurrency = 4);
    void pause();
    void resume();
    void cancel();

    QString id() const { return id_; }
    bool isRunning() const { return running_.load(); }
    bool isPaused() const { return paused_.load(); }

signals:
    void progressChanged(QString id, qint64 receivedBytes, qint64 totalBytes, double bytesPerSecond);
    void statusChanged(QString id, qtidm::DownloadStatus status, QString message);
    void segmentsChanged(QString id, QVector<qtidm::SegmentInfo> segments);

private:
    struct ChunkTask {
        int index = 0;
        QUrl uri;
        QString outputFilename;
        qint64 byteRangeOffset = -1;
        qint64 byteRangeLength = -1;
        HlsKey key;
        bool isInitSegment = false;
        bool isAudio = false;
        bool completed = false;
        bool inProgress = false;
        int retries = 0;
        qint64 size = 0;
    };

    void runEngine();
    void workerLoop(const QString& stagingDir);
    bool fetchPlaylist(const QUrl& url, QString* content, QUrl* effectiveUrl);
    bool fetchKey(const QUrl& keyUrl, QByteArray* keyData);
    bool downloadChunk(const ChunkTask& task, const QString& stagingDir, qint64* downloadedSize);
    bool assembleOutput(const QString& stagingDir, const QString& targetPath, bool hasSeparateAudio);
    void emitSegmentsSnapshot(bool force = false);

    QString id_;
    QUrl initialUrl_;
    QString targetPath_;
    QVariantMap headers_;
    QString dohUrl_;
    int concurrency_ = 4;

    std::atomic_bool running_ { false };
    std::atomic_bool paused_ { false };
    std::atomic_bool canceled_ { false };
    std::atomic_bool failed_ { false };
    QString failureReason_;

    std::jthread engineThread_;
    std::vector<std::jthread> workerPool_;

    std::mutex queueMutex_;
    std::condition_variable queueCv_;
    std::deque<int> pendingTaskIndices_;
    int activeWorkers_ = 0;

    mutable QMutex stateMutex_;
    QVector<ChunkTask> tasks_;

    mutable QMutex keyMutex_;
    QHash<QUrl, QByteArray> keyCache_;

    qint64 totalEstimatedBytes_ = 0;
    std::atomic<qint64> downloadedBytes_ { 0 };
    QElapsedTimer speedTimer_;
    qint64 lastSpeedBytes_ = 0;
    double currentSpeed_ = 0.0;

    QElapsedTimer segmentsTimer_;
    qint64 lastSegmentsEmitMs_ = 0;
};

} // namespace qtidm
