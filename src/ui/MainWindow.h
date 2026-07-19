#pragma once

#include "core/CurlEpollDownloader.h"
#include "core/DownloadScheduler.h"
#include "core/MediaDownloader.h"
#include "integration/CredentialVault.h"
#include "integration/MeteredNetworkMonitor.h"
#include "integration/SocialMediaExtractor.h"
#include "storage/DownloadRepository.h"
#include "theme/ThemeManager.h"

#include <QMainWindow>
#include <QMetaObject>
#include <QComboBox>
#include <QLineEdit>
#include <QSystemTrayIcon>
#include <QTableWidget>
#include <QTreeWidget>

namespace qtidm {

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(CurlEpollDownloader& downloader, DownloadScheduler& scheduler, DownloadRepository& repository, ThemeManager& themeManager, QWidget* parent = nullptr);

public slots:
    bool addUrl(QString url = {}, QVariantMap headers = {});
    bool addUrls(QStringList urls, QVariantMap headers = {});
    bool addBrowserDownloads(QVariantList downloads);
    void raiseAndActivate();

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void onDownloadAdded(const DownloadRecord& record);
    void onProgressChanged(const QString& id, qint64 received, qint64 total, double bytesPerSecond);
    void onSegmentsChanged(const QString& id, QVector<SegmentInfo> segments);
    void onStatusChanged(const QString& id, DownloadStatus status, const QString& message);
    void applyTheme();
    void importHistory();
    void exportHistory();
    void importLinks();
    void exportLinks();
    void previewZip();
    void grabSite();
    void extractSocialMedia();
    void showQueue();
    void showOptions();
    void showAbout();
    void resumeSelected();
    void pauseSelected();
    void cancelSelected();
    void deleteSelected();
    void propertiesSelected();
    void openSelectedFile();
    void openSelectedFolder();
    void copySelectedUrl();
    void copySelectedPath();
    void calculateSelectedChecksum();
    void clearFinished();

private:
    void buildActions();
    void buildLayout();
    void buildTray();
    void loadPersistedDownloads();
    void configureClipboardMonitor();
    void applyCategoryFilter();
    void applyRequestDefaults(DownloadRequest& request) const;
    QList<int> selectedRows() const;
    bool hasDuplicateUrl(const QUrl& url) const;
    int rowForId(const QString& id) const;
    QString statusText(DownloadStatus status) const;

    CurlEpollDownloader& downloader_;
    MediaDownloader mediaDownloader_;
    CredentialVault credentialVault_;
    MeteredNetworkMonitor networkMonitor_;
    SocialMediaExtractor socialMediaExtractor_;
    DownloadScheduler& scheduler_;
    DownloadRepository& repository_;
    ThemeManager& themeManager_;
    QTreeWidget* categories_ = nullptr;
    QTableWidget* downloads_ = nullptr;
    QLineEdit* search_ = nullptr;
    QComboBox* statusFilter_ = nullptr;
    QSystemTrayIcon* tray_ = nullptr;
    QMetaObject::Connection clipboardConnection_;
    QString lastClipboardUrl_;
    bool browserRequestActive_ = false;
    bool quitting_ = false;
};

}
