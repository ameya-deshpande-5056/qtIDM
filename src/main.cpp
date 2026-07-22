#include "app/Paths.h"
#include "app/Logger.h"
#include "app/SingleInstance.h"
#include "core/CurlEpollDownloader.h"
#include "core/DownloadScheduler.h"
#include "storage/DownloadRepository.h"
#include "theme/ThemeManager.h"
#include "ui/MainWindow.h"

#include <QApplication>
#include <QIcon>
#include <QMessageBox>
#include <QProcess>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <cstdio>
#ifdef Q_OS_WIN
#include <QLocalServer>
#include <QLocalSocket>
#include <QDataStream>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVariantList>
#endif

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("qtIDM"));
    QApplication::setApplicationDisplayName(QStringLiteral("qtIDM"));
    QApplication::setDesktopFileName(QStringLiteral("io.qtidm.Qtidm"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/qtidm/icons/application.png")));
    QApplication::setOrganizationName(QStringLiteral("qtIDM"));
    QApplication::setApplicationVersion(QStringLiteral(QTIDM_VERSION));
    qtidm::Paths::ensureRuntimeDirs();
    qtidm::Logger::install();
    if (app.arguments().contains(QStringLiteral("--version"))) {
        std::printf("qtIDM %s\n", QTIDM_VERSION);
        return 0;
    }
    if (app.arguments().contains(QStringLiteral("--help"))) {
        std::printf("Usage: qtIDM [--help] [--version] [url...]\n");
        return 0;
    }
    qRegisterMetaType<qtidm::DownloadRecord>("qtidm::DownloadRecord");
    qRegisterMetaType<qtidm::DownloadStatus>("qtidm::DownloadStatus");
    qRegisterMetaType<QVector<qtidm::SegmentInfo>>("QVector<qtidm::SegmentInfo>");

    QStringList startupArgs;
    for (const auto& arg : app.arguments().mid(1)) {
        const QUrl url(arg);
        if (!arg.startsWith(QLatin1String("--")) && url.isValid() && !url.scheme().isEmpty()) {
            startupArgs.append(arg);
        }
    }
    qtidm::SingleInstance singleInstance;
    if (!singleInstance.acquire()) {
        singleInstance.notifyExistingInstance(startupArgs);
        return 0;
    }

    qtidm::DownloadRepository repository;
    if (!repository.open(qtidm::Paths::databasePath())) {
        QMessageBox::critical(nullptr, QStringLiteral("qtIDM"), repository.lastError());
        return 1;
    }

    qtidm::CurlEpollDownloader downloader;
    qtidm::DownloadScheduler scheduler(downloader);
    scheduler.load();
    qtidm::ThemeManager themeManager;
    qtidm::MainWindow window(downloader, scheduler, repository, themeManager);
    QObject::connect(&scheduler, &qtidm::DownloadScheduler::completionCommandRequested,
                     &app, [](const QString& program, const QStringList& arguments) {
                         QProcess::startDetached(program, arguments);
                     });

    QObject::connect(&singleInstance, &qtidm::SingleInstance::activateRequested, &window, &qtidm::MainWindow::raiseAndActivate);
    singleInstance.setUrlHandler([&window](QString url, QVariantMap headers) { return window.addUrl(std::move(url), std::move(headers)); });
    singleInstance.setUrlsHandler([&window](QStringList urls, QVariantMap headers) { return window.addUrls(std::move(urls), std::move(headers)); });
    singleInstance.setDownloadsHandler([&window](QVariantList downloads) { return window.addBrowserDownloads(std::move(downloads)); });

#ifdef Q_OS_WIN
    // On Windows, listen for incoming native messaging host connections
    // on a named pipe via QLocalServer. The native messaging host connects
    // here instead of using D-Bus.
    QLocalServer localServer;
    // Remove any stale pipe from a previous crash
    QLocalServer::removeServer(QStringLiteral("io.qtidm.Qtidm"));
    localServer.listen(QStringLiteral("io.qtidm.Qtidm"));
    QObject::connect(&localServer, &QLocalServer::newConnection, &app, [&]() {
        while (auto* socket = localServer.nextPendingConnection()) {
            // Process requests from the native messaging host
            QObject::connect(socket, &QLocalSocket::readyRead, &app, [socket, &window]() {
                QDataStream stream(socket);
                stream.setByteOrder(QDataStream::LittleEndian);
                while (!stream.atEnd()) {
                    quint32 msgSize = 0;
                    stream >> msgSize;
                    if (msgSize == 0 || msgSize > 8 * 1024 * 1024) break;
                    QByteArray payload(static_cast<qsizetype>(msgSize), Qt::Uninitialized);
                    if (stream.readRawData(payload.data(), payload.size()) != payload.size()) break;
                    QJsonParseError parseError;
                    const auto doc = QJsonDocument::fromJson(payload, &parseError);
                    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) break;
                    const auto obj = doc.object();
                    if (obj.value(QStringLiteral("type")).toString() == QStringLiteral("status")) {
                        QJsonObject statusReply;
                        statusReply[QStringLiteral("ok")] = true;
                        statusReply[QStringLiteral("status")] = QStringLiteral("ready");
                        statusReply[QStringLiteral("persistent")] = true;
                        const auto replyBytes = QJsonDocument(statusReply).toJson(QJsonDocument::Compact);
                        QDataStream replyStream(socket);
                        replyStream.setByteOrder(QDataStream::LittleEndian);
                        replyStream << static_cast<quint32>(replyBytes.size());
                        replyStream.writeRawData(replyBytes.constData(), replyBytes.size());
                        continue;
                    }
                    QJsonObject reply;
                    reply[QStringLiteral("ok")] = true;
                    reply[QStringLiteral("accepted")] = true;
                    if (obj.contains(QStringLiteral("url")) && obj.value(QStringLiteral("url")).isString()) {
                        window.addUrl(obj.value(QStringLiteral("url")).toString(), QVariantMap());
                    } else if (obj.contains(QStringLiteral("urls")) && obj.value(QStringLiteral("urls")).isArray()) {
                        QStringList urls;
                        for (const auto& u : obj.value(QStringLiteral("urls")).toArray()) {
                            urls.append(u.toString());
                        }
                        window.addUrls(urls, QVariantMap());
                    } else if (obj.contains(QStringLiteral("downloads")) && obj.value(QStringLiteral("downloads")).isArray()) {
                        QVariantList downloads;
                        for (const auto& d : obj.value(QStringLiteral("downloads")).toArray()) {
                            downloads.append(d.toObject().toVariantMap());
                        }
                        window.addBrowserDownloads(downloads);
                    }
                    const auto replyBytes = QJsonDocument(reply).toJson(QJsonDocument::Compact);
                    QDataStream replyStream(socket);
                    replyStream.setByteOrder(QDataStream::LittleEndian);
                    replyStream << static_cast<quint32>(replyBytes.size());
                    replyStream.writeRawData(replyBytes.constData(), replyBytes.size());
                }
            });
            QObject::connect(socket, &QLocalSocket::disconnected, socket, &QLocalSocket::deleteLater);
        }
    });
#endif

    downloader.start();
    window.show();
    QTimer::singleShot(0, &window, [&window, startupArgs] {
        for (const auto& arg : startupArgs) {
            window.addUrl(arg);
        }
    });
    const int rc = app.exec();
    downloader.stop();
#ifdef Q_OS_WIN
    localServer.close();
#endif
    return rc;
}
