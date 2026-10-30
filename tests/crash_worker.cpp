#include "core/CurlEpollDownloader.h"
#include "storage/DownloadRepository.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFileInfo>
#include <iostream>

using namespace qtidm;

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    qRegisterMetaType<DownloadRecord>("qtidm::DownloadRecord");
    qRegisterMetaType<DownloadStatus>("qtidm::DownloadStatus");
    qRegisterMetaType<QVector<SegmentInfo>>("QVector<qtidm::SegmentInfo>");

    QCommandLineParser parser;
    parser.addHelpOption();
    QCommandLineOption dbOpt(QStringLiteral("db"), QStringLiteral("Database path"), QStringLiteral("path"));
    QCommandLineOption urlOpt(QStringLiteral("url"), QStringLiteral("URL to download"), QStringLiteral("url"));
    QCommandLineOption targetOpt(QStringLiteral("target"), QStringLiteral("Target path"), QStringLiteral("path"));
    QCommandLineOption modeOpt(QStringLiteral("mode"), QStringLiteral("Mode: download or resume"), QStringLiteral("mode"), QStringLiteral("download"));
    QCommandLineOption segmentsOpt(QStringLiteral("segments"), QStringLiteral("Segments"), QStringLiteral("count"), QStringLiteral("4"));
    QCommandLineOption totalOpt(QStringLiteral("total"), QStringLiteral("Total bytes"), QStringLiteral("bytes"), QStringLiteral("-1"));

    parser.addOption(dbOpt);
    parser.addOption(urlOpt);
    parser.addOption(targetOpt);
    parser.addOption(modeOpt);
    parser.addOption(segmentsOpt);
    parser.addOption(totalOpt);
    parser.process(app);

    const auto dbPath = parser.value(dbOpt);
    const auto urlStr = parser.value(urlOpt);
    const auto targetPath = parser.value(targetOpt);
    const auto mode = parser.value(modeOpt);
    const int segments = parser.value(segmentsOpt).toInt();
    const qint64 total = parser.value(totalOpt).toLongLong();

    DownloadRepository repository;
    if (!repository.open(dbPath)) {
        std::cerr << "Failed to open repository: " << repository.lastError().toStdString() << "\n";
        return 1;
    }

    CurlEpollDownloader downloader;
    downloader.setPersistenceHandler([&repository](const QString& id, const QVector<SegmentInfo>& segments) {
        return repository.checkpoint(id, segments);
    });
    downloader.start();

    if (mode == QStringLiteral("download")) {
        DownloadRequest request;
        request.url = QUrl(urlStr);
        request.targetPath = targetPath;
        request.segments = segments;
        request.expectedTotalBytes = total;
        request.dynamicSegmentation = true;

        QObject::connect(&downloader, &CurlEpollDownloader::downloadAdded,
            [&repository](const DownloadRecord& record) {
                repository.upsertDownload(record);
            });

        QObject::connect(&downloader, &CurlEpollDownloader::segmentsChanged,
            [&downloader, &repository](const QString& id, const QVector<SegmentInfo>& segs) {
                if (repository.checkpoint(id, segs)) {
                    downloader.acknowledgeCheckpoint(id, segs);
                }
                qint64 durableTotal = 0;
                for (const auto& s : segs) {
                    durableTotal += s.durableWritten;
                }
                if (durableTotal > 0) {
                    std::cout << "CHECKPOINT COMMITTED " << durableTotal << "\n";
                    std::cout.flush();
                }
            });

        QObject::connect(&downloader, &CurlEpollDownloader::statusChanged,
            [&app](const QString&, DownloadStatus status, const QString& error) {
                if (status == DownloadStatus::Completed) {
                    std::cout << "DOWNLOAD COMPLETED\n";
                    std::cout.flush();
                    app.quit();
                } else if (status == DownloadStatus::Failed) {
                    std::cerr << "DOWNLOAD FAILED: " << error.toStdString() << "\n";
                    app.exit(1);
                }
            });

        downloader.enqueue(request);
        return app.exec();
    } else if (mode == QStringLiteral("resume")) {
        const auto records = repository.listDownloads();
        if (records.isEmpty()) {
            std::cerr << "No records found in database to resume\n";
            return 1;
        }
        const auto record = records.first();
        DownloadRequest request = record.request;
        request.existingId = record.id;
        request.url = record.url;
        request.targetPath = record.targetPath;
        request.expectedTotalBytes = record.totalBytes;
        request.resumeSegments = record.segments;
        request.segments = record.segments.isEmpty() ? segments : record.segments.size();
        request.dynamicSegmentation = true;

        QObject::connect(&downloader, &CurlEpollDownloader::segmentsChanged,
            [&downloader, &repository](const QString& id, const QVector<SegmentInfo>& segs) {
                if (repository.checkpoint(id, segs)) {
                    downloader.acknowledgeCheckpoint(id, segs);
                }
            });

        QObject::connect(&downloader, &CurlEpollDownloader::statusChanged,
            [&app](const QString&, DownloadStatus status, const QString& error) {
                if (status == DownloadStatus::Completed) {
                    std::cout << "RESUME COMPLETED\n";
                    std::cout.flush();
                    app.quit();
                } else if (status == DownloadStatus::Failed) {
                    std::cerr << "RESUME FAILED: " << error.toStdString() << "\n";
                    app.exit(1);
                }
            });

        downloader.enqueue(request);
        return app.exec();
    }

    std::cerr << "Unknown mode: " << mode.toStdString() << "\n";
    return 1;
}

