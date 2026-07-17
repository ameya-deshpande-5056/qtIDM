#pragma once

#include "core/CurlEpollDownloader.h"
#include "core/DownloadScheduler.h"
#include "storage/DownloadRepository.h"
#include "theme/ThemeManager.h"

#include <QMainWindow>
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
    void resumeSelected();
    void pauseSelected();
    void cancelSelected();
    void deleteSelected();

private:
    void buildActions();
    void buildLayout();
    void buildTray();
    void loadPersistedDownloads();
    int rowForId(const QString& id) const;
    QString statusText(DownloadStatus status) const;

    CurlEpollDownloader& downloader_;
    DownloadScheduler& scheduler_;
    DownloadRepository& repository_;
    ThemeManager& themeManager_;
    QTreeWidget* categories_ = nullptr;
    QTableWidget* downloads_ = nullptr;
    QSystemTrayIcon* tray_ = nullptr;
};

}
