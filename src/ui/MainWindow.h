#pragma once

#include "core/CurlEpollDownloader.h"
#include "core/DownloadScheduler.h"
#include "core/MediaDownloader.h"
#include "integration/CredentialVault.h"
#include "integration/MeteredNetworkMonitor.h"
#include "integration/SocialMediaExtractor.h"
#include "storage/DownloadRepository.h"
#include "theme/ThemeManager.h"

#include <QAction>
#include <QMainWindow>
#include <QMetaObject>
#include <QComboBox>
#include <QHash>
#include <QLineEdit>
#include <QElapsedTimer>
#include <QSplitter>
#include <QSet>
#include <QSystemTrayIcon>
#include <QTableWidget>
#include <QTabWidget>
#include <QTreeWidget>

class QLabel;
class QToolButton;

namespace qtidm {

class SegmentGrid;

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
    void updateDownloadActionStates();
    void refreshDownloadDetails();
    void refreshDownloadDetailsFor(const QString& id);
    void refreshDownloadProgressDetails(const QString& id, qint64 received,
                                        qint64 total, double bytesPerSecond);
    void updateSessionStatus();
    void setAlternateSpeedLimit(bool enabled, qint64 bytesPerSecond);
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
    QSplitter* navigationSplitter_ = nullptr;
    QSplitter* downloadSplitter_ = nullptr;
    QTabWidget* detailsTabs_ = nullptr;
    QTreeWidget* generalDetails_ = nullptr;
    QTreeWidget* requestDetails_ = nullptr;
    QTableWidget* segmentDetails_ = nullptr;
    SegmentGrid* detailSegmentGrid_ = nullptr;
    QLineEdit* search_ = nullptr;
    QComboBox* statusFilter_ = nullptr;
    QSystemTrayIcon* tray_ = nullptr;
    QHash<QString, QString> statusMessages_;
    QAction* startAction_ = nullptr;
    QAction* pauseAction_ = nullptr;
    QAction* stopAction_ = nullptr;
    QAction* deleteAction_ = nullptr;
    QAction* editAction_ = nullptr;
    QLabel* sessionRateLabel_ = nullptr;
    QLabel* sessionSizeLabel_ = nullptr;
    QToolButton* alternateLimitButton_ = nullptr;
    struct TransferMetric {
        qint64 lastReceived = 0;
        qint64 lastSampleMs = 0;
        qint64 lastDataMs = 0;
        qint64 lastUiUpdateMs = 0;
        double smoothedBytesPerSecond = 0.0;
        bool hasSpeed = false;
    };
    QHash<QString, TransferMetric> transferMetrics_;
    QSet<QString> activeTransfers_;
    QElapsedTimer transferClock_;
    qint64 sessionDownloadedBytes_ = 0;
    QMetaObject::Connection clipboardConnection_;
    QString lastClipboardUrl_;
    bool browserRequestActive_ = false;
    bool quitting_ = false;
};

}
