#include "core/CurlEpollDownloader.h"
#include "core/DownloadTypes.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QJsonDocument>
#include <QUrl>
#include <iostream>
#include <sys/resource.h>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc < 4) {
        std::cerr << "Usage: benchmark_runner <url> <target_path> <segments> [dynamic:0|1]\n";
        return 1;
    }

    const QUrl url(QString::fromUtf8(argv[1]));
    const QString targetPath = QString::fromUtf8(argv[2]);
    const int segments = std::max(1, std::atoi(argv[3]));
    const bool dynamicSeg = argc > 4 ? (std::atoi(argv[4]) != 0) : true;

    qRegisterMetaType<qtidm::DownloadRecord>("qtidm::DownloadRecord");
    qRegisterMetaType<qtidm::DownloadStatus>("qtidm::DownloadStatus");
    qRegisterMetaType<QVector<qtidm::SegmentInfo>>("QVector<qtidm::SegmentInfo>");

    qtidm::CurlEpollDownloader downloader;

    int maxActiveConnections = 0;
    qint64 totalBytesReceived = 0;
    qint64 reportedTotalBytes = -1;
    bool completed = false;
    bool failed = false;
    QString failureMessage;

    QObject::connect(&downloader, &qtidm::CurlEpollDownloader::hostConnectionCountChanged,
                     [&maxActiveConnections](const QString&, int active) {
                         if (active > maxActiveConnections) {
                             maxActiveConnections = active;
                         }
                     });

    QObject::connect(&downloader, &qtidm::CurlEpollDownloader::progressChanged,
                     [&totalBytesReceived, &reportedTotalBytes](const QString&, qint64 received, qint64 total, double) {
                         totalBytesReceived = received;
                         reportedTotalBytes = total;
                     });

    QObject::connect(&downloader, &qtidm::CurlEpollDownloader::statusChanged,
                     [&completed, &failed, &failureMessage, &app](const QString&, qtidm::DownloadStatus status, const QString& msg) {
                         if (status == qtidm::DownloadStatus::Completed) {
                             completed = true;
                             app.quit();
                         } else if (status == qtidm::DownloadStatus::Failed || status == qtidm::DownloadStatus::Canceled) {
                             failed = true;
                             failureMessage = msg;
                             app.quit();
                         }
                     });

    qtidm::DownloadRequest request;
    request.url = url;
    request.targetPath = targetPath;
    request.segments = segments;
    request.dynamicSegmentation = dynamicSeg;
    request.fileConflictPolicy = qtidm::FileConflictPolicy::Overwrite;

    downloader.start();

    struct rusage usageStart;
    getrusage(RUSAGE_SELF, &usageStart);

    QElapsedTimer timer;
    timer.start();

    downloader.enqueue(request);

    app.exec();
    const qint64 elapsedMs = timer.elapsed();

    downloader.stop();

    struct rusage usageEnd;
    getrusage(RUSAGE_SELF, &usageEnd);

    const long userCpuMs = (usageEnd.ru_utime.tv_sec - usageStart.ru_utime.tv_sec) * 1000 +
                           (usageEnd.ru_utime.tv_usec - usageStart.ru_utime.tv_usec) / 1000;
    const long sysCpuMs = (usageEnd.ru_stime.tv_sec - usageStart.ru_stime.tv_sec) * 1000 +
                          (usageEnd.ru_stime.tv_usec - usageStart.ru_stime.tv_usec) / 1000;

    const qint64 actualFileSize = QFileInfo::exists(targetPath) ? QFileInfo(targetPath).size() : 0;
    const double throughputMBs = elapsedMs > 0 ? (static_cast<double>(actualFileSize) / (1024.0 * 1024.0)) / (static_cast<double>(elapsedMs) / 1000.0) : 0.0;

    QJsonObject res;
    res["success"] = completed && !failed;
    res["error"] = failureMessage;
    res["elapsed_ms"] = elapsedMs;
    res["actual_file_size"] = actualFileSize;
    res["reported_total_bytes"] = reportedTotalBytes;
    res["throughput_mb_s"] = throughputMBs;
    res["max_active_connections"] = maxActiveConnections;
    res["user_cpu_ms"] = static_cast<qint64>(userCpuMs);
    res["sys_cpu_ms"] = static_cast<qint64>(sysCpuMs);
    res["peak_rss_kb"] = static_cast<qint64>(usageEnd.ru_maxrss);

    std::cout << QJsonDocument(res).toJson(QJsonDocument::Compact).toStdString() << std::endl;
    return (completed && !failed) ? 0 : 1;
}

