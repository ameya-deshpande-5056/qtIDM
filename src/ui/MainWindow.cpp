#include "ui/MainWindow.h"

#include "core/SiteGrabber.h"
#include "core/ZipPreview.h"
#include "integration/ImportExportService.h"
#include "ui/SegmentGrid.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDateTimeEdit>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStatusBar>
#include <QToolBar>
#include <QUrl>
#include <QVBoxLayout>

namespace qtidm {

MainWindow::MainWindow(CurlEpollDownloader& downloader, DownloadScheduler& scheduler, DownloadRepository& repository, ThemeManager& themeManager, QWidget* parent)
    : QMainWindow(parent)
    , downloader_(downloader)
    , scheduler_(scheduler)
    , repository_(repository)
    , themeManager_(themeManager)
{
    setWindowTitle(QStringLiteral("qtIDM"));
    resize(960, 560);
    buildActions();
    buildLayout();
    buildTray();
    loadPersistedDownloads();
    applyTheme();

    connect(&downloader_, &CurlEpollDownloader::downloadAdded, this, &MainWindow::onDownloadAdded);
    connect(&downloader_, &CurlEpollDownloader::progressChanged, this, &MainWindow::onProgressChanged);
    connect(&downloader_, &CurlEpollDownloader::segmentsChanged, this, &MainWindow::onSegmentsChanged);
    connect(&downloader_, &CurlEpollDownloader::statusChanged, this, &MainWindow::onStatusChanged);
    connect(&themeManager_, &ThemeManager::themeChanged, this, &MainWindow::applyTheme);
}

void MainWindow::addUrl(QString url, QVariantMap headers)
{
    if (url.isEmpty()) {
        bool ok = false;
        url = QInputDialog::getText(this, QStringLiteral("Add URL"), QStringLiteral("Address:"), QLineEdit::Normal, {}, &ok);
        if (!ok || url.isEmpty()) {
            return;
        }
    }
    const QUrl parsed(url);
    if (!parsed.isValid() || parsed.scheme().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Invalid URL"), QStringLiteral("The URL is not valid."));
        return;
    }

    const auto name = parsed.fileName().isEmpty() ? QStringLiteral("download.bin") : parsed.fileName();
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Download Options"));
    auto* form = new QFormLayout(&dialog);
    auto* targetEdit = new QLineEdit(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation) + QLatin1Char('/') + name);
    auto* categoryEdit = new QLineEdit(QStringLiteral("General"));
    auto* segments = new QSpinBox;
    segments->setRange(1, 32);
    segments->setValue(8);
    auto* speedLimit = new QSpinBox;
    speedLimit->setRange(0, 1024 * 1024);
    speedLimit->setSuffix(QStringLiteral(" KB/s"));
    auto* proxyEdit = new QLineEdit;
    auto* userEdit = new QLineEdit;
    auto* passwordEdit = new QLineEdit;
    passwordEdit->setEchoMode(QLineEdit::Password);
    auto* scheduledAt = new QDateTimeEdit;
    scheduledAt->setCalendarPopup(true);
    scheduledAt->setSpecialValueText(QStringLiteral("Start now"));
    scheduledAt->setMinimumDateTime(QDateTime::currentDateTime());
    scheduledAt->setDateTime(QDateTime::currentDateTime());
    auto* browse = new QPushButton(QStringLiteral("Browse"));
    connect(browse, &QPushButton::clicked, this, [this, targetEdit] {
        const auto selected = QFileDialog::getSaveFileName(this, QStringLiteral("Save As"), targetEdit->text());
        if (!selected.isEmpty()) {
            targetEdit->setText(selected);
        }
    });
    auto* targetRow = new QWidget;
    auto* targetLayout = new QHBoxLayout(targetRow);
    targetLayout->setContentsMargins(0, 0, 0, 0);
    targetLayout->addWidget(targetEdit);
    targetLayout->addWidget(browse);
    form->addRow(QStringLiteral("Save to:"), targetRow);
    form->addRow(QStringLiteral("Category:"), categoryEdit);
    form->addRow(QStringLiteral("Segments:"), segments);
    form->addRow(QStringLiteral("Speed limit:"), speedLimit);
    form->addRow(QStringLiteral("Proxy:"), proxyEdit);
    form->addRow(QStringLiteral("User:"), userEdit);
    form->addRow(QStringLiteral("Password:"), passwordEdit);
    form->addRow(QStringLiteral("Start at:"), scheduledAt);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted || targetEdit->text().isEmpty()) {
        return;
    }

    DownloadRequest request;
    request.url = parsed;
    request.targetPath = targetEdit->text();
    request.category = categoryEdit->text();
    request.headers = std::move(headers);
    request.segments = segments->value();
    request.proxyUrl = proxyEdit->text();
    request.username = userEdit->text();
    request.password = passwordEdit->text();
    request.speedLimitBytesPerSecond = static_cast<qint64>(speedLimit->value()) * 1024;
    if (scheduledAt->dateTime() > QDateTime::currentDateTime().addSecs(2)) {
        request.scheduledAt = scheduledAt->dateTime();
    }
    scheduler_.schedule(std::move(request));
}

void MainWindow::raiseAndActivate()
{
    showNormal();
    raise();
    activateWindow();
}

void MainWindow::onDownloadAdded(const DownloadRecord& record)
{
    repository_.upsertDownload(record);
    int row = rowForId(record.id);
    if (row < 0) {
        row = downloads_->rowCount();
        downloads_->insertRow(row);
    }
    downloads_->setItem(row, 0, new QTableWidgetItem(record.id));
    downloads_->setItem(row, 1, new QTableWidgetItem(record.url.toString()));
    downloads_->setItem(row, 2, new QTableWidgetItem(record.targetPath));
    downloads_->setItem(row, 3, new QTableWidgetItem(statusText(record.status)));
    const int percent = record.totalBytes > 0 ? static_cast<int>((record.completedBytes * 100) / record.totalBytes) : 0;
    downloads_->setItem(row, 4, new QTableWidgetItem(QString::number(percent) + QLatin1Char('%')));
    downloads_->setItem(row, 5, new QTableWidgetItem(QStringLiteral("0 B/s")));
    auto* grid = new SegmentGrid(downloads_);
    grid->setSegments(record.segments);
    downloads_->setCellWidget(row, 6, grid);
    statusBar()->showMessage(QStringLiteral("Download started"), 2000);
}

void MainWindow::onProgressChanged(const QString& id, qint64 received, qint64 total, double bytesPerSecond)
{
    repository_.updateProgress(id, received, total, total > 0 && received >= total ? DownloadStatus::Completed : DownloadStatus::Downloading);
    const int row = rowForId(id);
    if (row < 0) {
        return;
    }
    const int percent = total > 0 ? static_cast<int>((received * 100) / total) : 0;
    downloads_->item(row, 4)->setText(QString::number(percent) + QLatin1Char('%'));
    downloads_->item(row, 5)->setText(QString::number(bytesPerSecond / 1024.0, 'f', 1) + QStringLiteral(" KB/s"));
}

void MainWindow::onSegmentsChanged(const QString& id, QVector<SegmentInfo> segments)
{
    const int row = rowForId(id);
    if (row < 0) {
        return;
    }
    auto* grid = qobject_cast<SegmentGrid*>(downloads_->cellWidget(row, 6));
    if (grid) {
        grid->setSegments(segments);
    }
    repository_.updateSegments(id, segments);
}

void MainWindow::onStatusChanged(const QString& id, DownloadStatus status, const QString& message)
{
    repository_.updateStatus(id, status);
    const int row = rowForId(id);
    if (row >= 0) {
        downloads_->item(row, 3)->setText(statusText(status));
    }
    if (!message.isEmpty()) {
        statusBar()->showMessage(message, 5000);
    }
}

void MainWindow::applyTheme()
{
    qApp->setStyleSheet(themeManager_.styleSheet());
}

void MainWindow::importHistory()
{
    const auto path = QFileDialog::getOpenFileName(this, QStringLiteral("Import"), {}, QStringLiteral("qtIDM JSON (*.json)"));
    if (path.isEmpty()) {
        return;
    }
    ImportExportService service(repository_);
    if (!service.importHistory(path)) {
        QMessageBox::warning(this, QStringLiteral("Import Failed"), service.lastError());
        return;
    }
    downloads_->setRowCount(0);
    loadPersistedDownloads();
}

void MainWindow::exportHistory()
{
    const auto path = QFileDialog::getSaveFileName(this, QStringLiteral("Export"), QStringLiteral("qtidm-history.json"), QStringLiteral("qtIDM JSON (*.json)"));
    if (path.isEmpty()) {
        return;
    }
    ImportExportService service(repository_);
    if (!service.exportHistory(path)) {
        QMessageBox::warning(this, QStringLiteral("Export Failed"), service.lastError());
    }
}

void MainWindow::previewZip()
{
    const auto path = QFileDialog::getOpenFileName(this, QStringLiteral("ZIP Preview"), {}, QStringLiteral("ZIP Archives (*.zip)"));
    if (path.isEmpty()) {
        return;
    }
    QString error;
    const auto entries = ZipPreview().read(path, &error);
    if (!error.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("ZIP Preview"), error);
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("ZIP Preview"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* view = new QPlainTextEdit;
    view->setReadOnly(true);
    for (const auto& entry : entries) {
        view->appendPlainText(QStringLiteral("%1  %2 / %3 bytes").arg(entry.name).arg(entry.compressedSize).arg(entry.uncompressedSize));
    }
    layout->addWidget(view);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.resize(640, 420);
    dialog.exec();
}

void MainWindow::grabSite()
{
    bool ok = false;
    const auto root = QInputDialog::getText(this, QStringLiteral("Site Grabber"), QStringLiteral("Site URL:"), QLineEdit::Normal, {}, &ok);
    if (!ok || root.isEmpty()) {
        return;
    }
    const auto target = QFileDialog::getExistingDirectory(this, QStringLiteral("Save Site To"), QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
    if (target.isEmpty()) {
        return;
    }
    QString error;
    const auto requests = SiteGrabber().grab(QUrl(root), target, 1, &error);
    if (!error.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Site Grabber"), error);
        return;
    }
    for (auto request : requests) {
        scheduler_.schedule(std::move(request));
    }
}

void MainWindow::showQueue()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Scheduler"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* view = new QPlainTextEdit;
    view->setReadOnly(true);
    for (const auto& request : scheduler_.queued()) {
        view->appendPlainText(request.scheduledAt.toString(Qt::ISODateWithMs) + QStringLiteral("  ") + request.url.toString());
    }
    layout->addWidget(view);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.resize(640, 360);
    dialog.exec();
}

void MainWindow::showOptions()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Options"));
    auto* form = new QFormLayout(&dialog);
    auto* defaultSegments = new QSpinBox;
    defaultSegments->setRange(1, 32);
    defaultSegments->setValue(8);
    auto* defaultSpeedLimit = new QSpinBox;
    defaultSpeedLimit->setRange(0, 1024 * 1024);
    defaultSpeedLimit->setSuffix(QStringLiteral(" KB/s"));
    auto* clipboardMonitor = new QCheckBox(QStringLiteral("Clipboard monitor"));
    clipboardMonitor->setChecked(false);
    form->addRow(QStringLiteral("Default segments:"), defaultSegments);
    form->addRow(QStringLiteral("Default speed limit:"), defaultSpeedLimit);
    form->addRow(QStringLiteral("Integration:"), clipboardMonitor);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    form->addWidget(buttons);
    dialog.exec();
}

void MainWindow::showAbout()
{
    QMessageBox::about(this,
                       QStringLiteral("qtIDM"),
                       QStringLiteral("qtIDM\nLinux-native Qt download manager\nOriginal implementation."));
}

void MainWindow::resumeSelected()
{
    const auto selected = downloads_->selectedItems();
    if (selected.isEmpty()) {
        return;
    }
    const int row = selected.first()->row();
    const auto idItem = downloads_->item(row, 0);
    if (!idItem) {
        return;
    }
    for (const auto& record : repository_.listDownloads()) {
        if (record.id != idItem->text()) {
            continue;
        }
        DownloadRequest request;
        request.existingId = record.id;
        request.url = record.url;
        request.targetPath = record.targetPath;
        request.category = record.category;
        request.expectedTotalBytes = record.totalBytes;
        request.resumeSegments = record.segments;
        request.segments = record.segments.isEmpty() ? 8 : record.segments.size();
        scheduler_.schedule(std::move(request));
        return;
    }
}

void MainWindow::pauseSelected()
{
    const auto selected = downloads_->selectedItems();
    if (!selected.isEmpty()) {
        downloader_.pause(downloads_->item(selected.first()->row(), 0)->text());
    }
}

void MainWindow::cancelSelected()
{
    const auto selected = downloads_->selectedItems();
    if (!selected.isEmpty()) {
        downloader_.cancel(downloads_->item(selected.first()->row(), 0)->text());
    }
}

void MainWindow::deleteSelected()
{
    const auto selected = downloads_->selectedItems();
    if (selected.isEmpty()) {
        return;
    }
    const int row = selected.first()->row();
    const auto id = downloads_->item(row, 0)->text();
    downloader_.cancel(id);
    repository_.removeDownload(id);
    downloads_->removeRow(row);
}

void MainWindow::buildActions()
{
    auto* toolbar = addToolBar(QStringLiteral("Main"));
    toolbar->setMovable(false);

    auto addAction = toolbar->addAction(QIcon::fromTheme(QStringLiteral("list-add")), QStringLiteral("Add"));
    addAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_N));
    connect(addAction, &QAction::triggered, this, [this] { addUrl(); });

    toolbar->addAction(QIcon::fromTheme(QStringLiteral("media-playback-start")), QStringLiteral("Start"), this, &MainWindow::resumeSelected);
    toolbar->addAction(QIcon::fromTheme(QStringLiteral("media-playback-pause")), QStringLiteral("Pause"), this, &MainWindow::pauseSelected);
    toolbar->addAction(QIcon::fromTheme(QStringLiteral("media-playback-stop")), QStringLiteral("Stop"), this, &MainWindow::cancelSelected);
    toolbar->addAction(QIcon::fromTheme(QStringLiteral("edit-delete")), QStringLiteral("Delete"), this, &MainWindow::deleteSelected);
    toolbar->addSeparator();
    toolbar->addAction(QIcon::fromTheme(QStringLiteral("view-calendar")), QStringLiteral("Queue"), this, &MainWindow::showQueue);
    toolbar->addAction(QIcon::fromTheme(QStringLiteral("document-open")), QStringLiteral("Import"), this, &MainWindow::importHistory);
    toolbar->addAction(QIcon::fromTheme(QStringLiteral("document-save")), QStringLiteral("Export"), this, &MainWindow::exportHistory);
    toolbar->addAction(QIcon::fromTheme(QStringLiteral("folder-remote")), QStringLiteral("Grabber"), this, &MainWindow::grabSite);
    toolbar->addAction(QIcon::fromTheme(QStringLiteral("package-x-generic")), QStringLiteral("ZIP"), this, &MainWindow::previewZip);
    toolbar->addSeparator();
    toolbar->addAction(QIcon::fromTheme(QStringLiteral("preferences-system")), QStringLiteral("Options"), this, &MainWindow::showOptions);
    toolbar->addAction(QIcon::fromTheme(QStringLiteral("help-about")), QStringLiteral("About"), this, &MainWindow::showAbout);
}

void MainWindow::buildLayout()
{
    auto* splitter = new QSplitter(this);
    categories_ = new QTreeWidget(splitter);
    categories_->setHeaderHidden(true);
    categories_->addTopLevelItem(new QTreeWidgetItem(QStringList { QStringLiteral("All Downloads") }));
    categories_->addTopLevelItem(new QTreeWidgetItem(QStringList { QStringLiteral("Compressed") }));
    categories_->addTopLevelItem(new QTreeWidgetItem(QStringList { QStringLiteral("Documents") }));
    categories_->addTopLevelItem(new QTreeWidgetItem(QStringList { QStringLiteral("Music") }));
    categories_->addTopLevelItem(new QTreeWidgetItem(QStringList { QStringLiteral("Programs") }));
    categories_->addTopLevelItem(new QTreeWidgetItem(QStringList { QStringLiteral("Video") }));
    categories_->setMaximumWidth(220);

    downloads_ = new QTableWidget(splitter);
    downloads_->setColumnCount(7);
    downloads_->setHorizontalHeaderLabels({ QStringLiteral("ID"), QStringLiteral("File Name"), QStringLiteral("Save To"), QStringLiteral("Status"), QStringLiteral("Progress"), QStringLiteral("Transfer Rate"), QStringLiteral("Segments") });
    downloads_->setColumnHidden(0, true);
    downloads_->setSelectionBehavior(QAbstractItemView::SelectRows);
    downloads_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    downloads_->horizontalHeader()->setStretchLastSection(true);
    downloads_->horizontalHeader()->resizeSection(1, 260);
    downloads_->horizontalHeader()->resizeSection(2, 320);
    downloads_->horizontalHeader()->resizeSection(6, 180);

    splitter->addWidget(categories_);
    splitter->addWidget(downloads_);
    splitter->setStretchFactor(1, 1);
    setCentralWidget(splitter);
    statusBar()->showMessage(QStringLiteral("Ready"));
}

void MainWindow::buildTray()
{
    tray_ = new QSystemTrayIcon(QIcon::fromTheme(QStringLiteral("folder-download")), this);
    auto* menu = new QMenu(this);
    menu->addAction(QStringLiteral("Open qtIDM"), this, &MainWindow::raiseAndActivate);
    menu->addAction(QStringLiteral("Add URL"), this, [this] { addUrl(); });
    menu->addSeparator();
    menu->addAction(QStringLiteral("Quit"), qApp, &QApplication::quit);
    tray_->setContextMenu(menu);
    tray_->show();
}

void MainWindow::loadPersistedDownloads()
{
    for (const auto& record : repository_.listDownloads()) {
        onDownloadAdded(record);
    }
}

int MainWindow::rowForId(const QString& id) const
{
    for (int row = 0; row < downloads_->rowCount(); ++row) {
        const auto* item = downloads_->item(row, 0);
        if (item && item->text() == id) {
            return row;
        }
    }
    return -1;
}

QString MainWindow::statusText(DownloadStatus status) const
{
    switch (status) {
    case DownloadStatus::Queued: return QStringLiteral("Queued");
    case DownloadStatus::Connecting: return QStringLiteral("Connecting");
    case DownloadStatus::Downloading: return QStringLiteral("Downloading");
    case DownloadStatus::Paused: return QStringLiteral("Paused");
    case DownloadStatus::Completed: return QStringLiteral("Complete");
    case DownloadStatus::Failed: return QStringLiteral("Error");
    case DownloadStatus::Canceled: return QStringLiteral("Canceled");
    }
    return QStringLiteral("Error");
}

}
