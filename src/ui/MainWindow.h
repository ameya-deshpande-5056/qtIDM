#pragma once

#include "core/CurlEpollDownloader.h"
#include "core/DownloadScheduler.h"
#include "core/MediaDownloader.h"
#include "storage/DownloadRepository.h"
#include "theme/ThemeManager.h"

#include <QMainWindow>
#include <QMetaObject>
#include <QSystemTrayIcon>
#include <QTableWidget>
#include <QTreeWidget>

namespace qtidm {

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(CurlEpollDownloader& downloader, DownloadScheduler& scheduler, DownloadRepository& repository, ThemeManager& themeManager, QWidget* parent = nullptr);

public slots:
    void addUrl(QString url = {}, QVariantMap headers = {});
    void addUrls(QStringList urls, QVariantMap headers = {});
    void raiseAndActivate();

private slots:
    void onDownloadAdded(const DownloadRecord& record);
    void onProgressChanged(const QString& id, qint64 received, qint64 total, double bytesPerSecond);
    void onSegmentsChanged(const QString& id, QVector<SegmentInfo> segments);
    void onStatusChanged(const QString& id, DownloadStatus status, const QString& message);
    void applyTheme();
    void importHistory();
    void exportHistory();
    void previewZip();
    void grabSite();
    void showQueue();
    void showOptions();
    void showAbout();
    void resumeSelected();
    void pauseSelected();
    void cancelSelected();
    void deleteSelected();
    void propertiesSelected();

private:
    void buildActions();
    void buildLayout();
    void buildTray();
    void loadPersistedDownloads();
    void configureClipboardMonitor();
    void applyCategoryFilter();
    int rowForId(const QString& id) const;
    QString statusText(DownloadStatus status) const;

    CurlEpollDownloader& downloader_;
    MediaDownloader mediaDownloader_;
    DownloadScheduler& scheduler_;
    DownloadRepository& repository_;
    ThemeManager& themeManager_;
    QTreeWidget* categories_ = nullptr;
    QTableWidget* downloads_ = nullptr;
    QSystemTrayIcon* tray_ = nullptr;
    QMetaObject::Connection clipboardConnection_;
    QString lastClipboardUrl_;
};

}
