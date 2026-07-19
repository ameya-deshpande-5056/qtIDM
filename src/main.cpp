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

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("qtIDM"));
    QApplication::setApplicationDisplayName(QStringLiteral("qtIDM"));
    QApplication::setDesktopFileName(QStringLiteral("io.qtidm.Qtidm"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/qtidm/icons/application.svg")));
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

    downloader.start();
    window.show();
    QTimer::singleShot(0, &window, [&window, startupArgs] {
        for (const auto& arg : startupArgs) {
            window.addUrl(arg);
        }
    });
    const int rc = app.exec();
    downloader.stop();
    return rc;
}
