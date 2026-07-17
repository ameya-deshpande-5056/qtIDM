#include "ui/MainWindow.h"

#include "app/Logger.h"
#include "core/SiteGrabber.h"
#include "core/ZipPreview.h"
#include "integration/ImportExportService.h"
#include "ui/SegmentGrid.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
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
#include <QSet>
#include <QSettings>
#include <QToolBar>
#include <QTimeEdit>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>

namespace qtidm {

MainWindow::MainWindow(CurlEpollDownloader& downloader, DownloadScheduler& scheduler, DownloadRepository& repository, ThemeManager& themeManager, QWidget* parent)
    : QMainWindow(parent)
    , downloader_(downloader)
    , mediaDownloader_(this)
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
    configureClipboardMonitor();

    connect(&downloader_, &CurlEpollDownloader::downloadAdded, this, &MainWindow::onDownloadAdded);
    connect(&downloader_, &CurlEpollDownloader::progressChanged, this, &MainWindow::onProgressChanged);
    connect(&downloader_, &CurlEpollDownloader::segmentsChanged, this, &MainWindow::onSegmentsChanged);
    connect(&downloader_, &CurlEpollDownloader::statusChanged, this, &MainWindow::onStatusChanged);
    connect(&mediaDownloader_, &MediaDownloader::downloadAdded, this, &MainWindow::onDownloadAdded);
    connect(&mediaDownloader_, &MediaDownloader::progressChanged, this, &MainWindow::onProgressChanged);
    connect(&mediaDownloader_, &MediaDownloader::statusChanged, this, &MainWindow::onStatusChanged);
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
    const QUrl parsed = QUrl::fromUserInput(url);
    if (!parsed.isValid() || parsed.scheme().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Invalid URL"), QStringLiteral("The URL is not valid or missing a scheme."));
        return;
    }

    const bool adaptiveMedia = MediaDownloader::supports(parsed);
    QString name = parsed.fileName().isEmpty() ? QStringLiteral("download.bin") : parsed.fileName();
    if (adaptiveMedia) {
        name = QFileInfo(name).completeBaseName() + QStringLiteral(".mkv");
    }
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Download Options"));
    auto* form = new QFormLayout(&dialog);
    auto* targetEdit = new QLineEdit(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation) + QLatin1Char('/') + name);
    auto* categoryEdit = new QLineEdit(adaptiveMedia ? QStringLiteral("Video") : QStringLiteral("General"));
    auto* queueEdit = new QLineEdit(QStringLiteral("Main"));
    auto* segments = new QSpinBox;
    segments->setRange(1, 32);
    QSettings settings;
    segments->setValue(settings.value(QStringLiteral("downloads/defaultSegments"), 8).toInt());
    auto* dynamicSegments = new QCheckBox(QStringLiteral("Queue additional ranges and refill completed connections"));
    dynamicSegments->setChecked(true);
    auto* hostConnections = new QSpinBox;
    hostConnections->setRange(1, 128);
    hostConnections->setValue(settings.value(QStringLiteral("downloads/defaultHostConnections"), 16).toInt());
    auto* speedLimit = new QSpinBox;
    speedLimit->setRange(0, 1024 * 1024);
    speedLimit->setSuffix(QStringLiteral(" KB/s"));
    speedLimit->setValue(settings.value(QStringLiteral("downloads/defaultSpeedLimitKib"), 0).toInt());
    auto* proxyEdit = new QLineEdit;
    auto* priority = new QSpinBox;
    priority->setRange(-100, 100);
    priority->setValue(0);
    auto* repeatMinutes = new QSpinBox;
    repeatMinutes->setRange(0, 525600);
    repeatMinutes->setSuffix(QStringLiteral(" min"));
    auto* checksumEdit = new QLineEdit;
    checksumEdit->setPlaceholderText(QStringLiteral("Optional 64-character SHA-256"));
    auto* completionCommand = new QLineEdit;
    completionCommand->setPlaceholderText(QStringLiteral("Optional executable and arguments"));
    auto* conflictPolicy = new QComboBox;
    conflictPolicy->addItem(QStringLiteral("Rename automatically"), static_cast<int>(FileConflictPolicy::AutoRename));
    conflictPolicy->addItem(QStringLiteral("Overwrite existing file"), static_cast<int>(FileConflictPolicy::Overwrite));
    conflictPolicy->addItem(QStringLiteral("Skip download"), static_cast<int>(FileConflictPolicy::Skip));
    conflictPolicy->setCurrentIndex(qBound(0, settings.value(QStringLiteral("downloads/defaultConflictPolicy"), 0).toInt(), 2));
    auto* userEdit = new QLineEdit;
    auto* passwordEdit = new QLineEdit;
    passwordEdit->setEchoMode(QLineEdit::Password);
    auto* scheduledAt = new QDateTimeEdit;
    scheduledAt->setCalendarPopup(true);
    scheduledAt->setSpecialValueText(QStringLiteral("Start now"));
    scheduledAt->setMinimumDateTime(QDateTime::currentDateTime());
    scheduledAt->setDateTime(QDateTime::currentDateTime());
    auto* restrictWindow = new QCheckBox(QStringLiteral("Restrict execution time"));
    auto* windowStart = new QTimeEdit(QTime(0, 0));
    auto* windowEnd = new QTimeEdit(QTime(23, 59, 59));
    auto* weekdays = new QWidget;
    auto* weekdaysLayout = new QHBoxLayout(weekdays);
    weekdaysLayout->setContentsMargins(0, 0, 0, 0);
    QList<QCheckBox*> weekdayChecks;
    for (const auto* label : { "Mo", "Tu", "We", "Th", "Fr", "Sa", "Su" }) {
        auto* check = new QCheckBox(QString::fromLatin1(label));
        check->setChecked(true);
        weekdayChecks.append(check);
        weekdaysLayout->addWidget(check);
    }
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
    form->addRow(QStringLiteral("Queue:"), queueEdit);
    form->addRow(QStringLiteral("Priority:"), priority);
    form->addRow(QStringLiteral("Repeat every:"), repeatMinutes);
    form->addRow(QStringLiteral("Segments:"), segments);
    form->addRow(QStringLiteral("Dynamic segmentation:"), dynamicSegments);
    form->addRow(QStringLiteral("Per-host connection limit:"), hostConnections);
    form->addRow(QStringLiteral("Speed limit:"), speedLimit);
    form->addRow(QStringLiteral("Proxy:"), proxyEdit);
    form->addRow(QStringLiteral("SHA-256:"), checksumEdit);
    form->addRow(QStringLiteral("After completion:"), completionCommand);
    form->addRow(QStringLiteral("If file exists:"), conflictPolicy);
    form->addRow(QStringLiteral("User:"), userEdit);
    form->addRow(QStringLiteral("Password:"), passwordEdit);
    form->addRow(QStringLiteral("Start at:"), scheduledAt);
    form->addRow(QStringLiteral("Schedule window:"), restrictWindow);
    form->addRow(QStringLiteral("Allowed from:"), windowStart);
    form->addRow(QStringLiteral("Allowed until:"), windowEnd);
    form->addRow(QStringLiteral("Allowed days:"), weekdays);
    if (adaptiveMedia) {
        segments->setEnabled(false);
        speedLimit->setEnabled(false);
        scheduledAt->setEnabled(false);
        form->addRow(QStringLiteral("Media:"), new QLabel(QStringLiteral("HLS/DASH audio and video will be downloaded and muxed with FFmpeg.")));
    }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted || targetEdit->text().isEmpty()) {
        return;
    }

    if (adaptiveMedia) {
        mediaDownloader_.enqueue(parsed, targetEdit->text(), headers);
        return;
    }

    DownloadRequest request;
    request.url = parsed;
    request.targetPath = targetEdit->text();
    request.category = categoryEdit->text();
    request.queueName = queueEdit->text().trimmed();
    request.priority = priority->value();
    request.repeatIntervalSeconds = repeatMinutes->value() * 60;
    request.headers = std::move(headers);
    request.segments = segments->value();
    request.dynamicSegmentation = dynamicSegments->isChecked();
    request.perHostConnectionLimit = hostConnections->value();
    request.proxyUrl = proxyEdit->text();
    request.expectedSha256 = checksumEdit->text().trimmed().toLower();
    request.completionCommand = completionCommand->text().trimmed();
    request.fileConflictPolicy = static_cast<FileConflictPolicy>(conflictPolicy->currentData().toInt());
    request.username = userEdit->text();
    request.password = passwordEdit->text();
    request.speedLimitBytesPerSecond = static_cast<qint64>(speedLimit->value()) * 1024;
    if (scheduledAt->dateTime() > QDateTime::currentDateTime().addSecs(2)) {
        request.scheduledAt = scheduledAt->dateTime();
    }
    if (restrictWindow->isChecked()) {
        request.windowStart = windowStart->time();
        request.windowEnd = windowEnd->time();
        request.allowedWeekdays = 0;
        for (int day = 0; day < weekdayChecks.size(); ++day) {
            if (weekdayChecks[day]->isChecked()) {
                request.allowedWeekdays |= 1 << day;
            }
        }
        if (request.allowedWeekdays == 0) {
            QMessageBox::warning(this, QStringLiteral("Download Options"), QStringLiteral("Select at least one allowed weekday."));
            return;
        }
    }
    scheduler_.schedule(std::move(request));
}

void MainWindow::addUrls(QStringList urls, QVariantMap headers)
{
    urls.removeDuplicates();
    QList<QUrl> validUrls;
    for (const auto& value : std::as_const(urls)) {
        const auto url = QUrl::fromUserInput(value.trimmed());
        if (url.isValid() && (url.scheme() == QStringLiteral("http") || url.scheme() == QStringLiteral("https"))) {
            validUrls.append(url);
        }
    }
    if (validUrls.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Batch Download"), QStringLiteral("The batch does not contain any valid HTTP or HTTPS URLs."));
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Batch Download (%1 URLs)").arg(validUrls.size()));
    auto* form = new QFormLayout(&dialog);
    auto* directoryEdit = new QLineEdit(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
    auto* browse = new QPushButton(QStringLiteral("Browse"));
    connect(browse, &QPushButton::clicked, this, [this, directoryEdit] {
        const auto selected = QFileDialog::getExistingDirectory(this, QStringLiteral("Batch Download Directory"), directoryEdit->text());
        if (!selected.isEmpty()) {
            directoryEdit->setText(selected);
        }
    });
    auto* directoryRow = new QWidget;
    auto* directoryLayout = new QHBoxLayout(directoryRow);
    directoryLayout->setContentsMargins(0, 0, 0, 0);
    directoryLayout->addWidget(directoryEdit);
    directoryLayout->addWidget(browse);

    auto* categoryEdit = new QLineEdit(QStringLiteral("General"));
    auto* queueEdit = new QLineEdit(QStringLiteral("Main"));
    auto* segments = new QSpinBox;
    segments->setRange(1, 32);
    QSettings settings;
    segments->setValue(settings.value(QStringLiteral("downloads/defaultSegments"), 8).toInt());
    auto* dynamicSegments = new QCheckBox(QStringLiteral("Queue additional ranges as connections finish"));
    dynamicSegments->setChecked(true);
    auto* hostConnections = new QSpinBox;
    hostConnections->setRange(1, 128);
    hostConnections->setValue(settings.value(QStringLiteral("downloads/defaultHostConnections"), 16).toInt());
    auto* conflictPolicy = new QComboBox;
    conflictPolicy->addItem(QStringLiteral("Rename automatically"), static_cast<int>(FileConflictPolicy::AutoRename));
    conflictPolicy->addItem(QStringLiteral("Overwrite existing files"), static_cast<int>(FileConflictPolicy::Overwrite));
    conflictPolicy->addItem(QStringLiteral("Skip existing files"), static_cast<int>(FileConflictPolicy::Skip));
    conflictPolicy->setCurrentIndex(qBound(0, settings.value(QStringLiteral("downloads/defaultConflictPolicy"), 0).toInt(), 2));
    auto* completionCommand = new QLineEdit;
    completionCommand->setPlaceholderText(QStringLiteral("Optional executable and arguments"));
    auto* restrictWindow = new QCheckBox(QStringLiteral("Restrict execution time"));
    auto* windowStart = new QTimeEdit(QTime(0, 0));
    auto* windowEnd = new QTimeEdit(QTime(23, 59, 59));
    auto* weekdays = new QWidget;
    auto* weekdaysLayout = new QHBoxLayout(weekdays);
    weekdaysLayout->setContentsMargins(0, 0, 0, 0);
    QList<QCheckBox*> weekdayChecks;
    for (const auto* label : { "Mo", "Tu", "We", "Th", "Fr", "Sa", "Su" }) {
        auto* check = new QCheckBox(QString::fromLatin1(label));
        check->setChecked(true);
        weekdayChecks.append(check);
        weekdaysLayout->addWidget(check);
    }
    auto* summary = new QLabel(QStringLiteral("%1 unique valid URLs will be added using shared settings.").arg(validUrls.size()));
    summary->setWordWrap(true);

    form->addRow(summary);
    form->addRow(QStringLiteral("Save directory:"), directoryRow);
    form->addRow(QStringLiteral("Category:"), categoryEdit);
    form->addRow(QStringLiteral("Queue:"), queueEdit);
    form->addRow(QStringLiteral("Connections:"), segments);
    form->addRow(QStringLiteral("Dynamic segmentation:"), dynamicSegments);
    form->addRow(QStringLiteral("Per-host connection limit:"), hostConnections);
    form->addRow(QStringLiteral("If file exists:"), conflictPolicy);
    form->addRow(QStringLiteral("After completion:"), completionCommand);
    form->addRow(QStringLiteral("Schedule window:"), restrictWindow);
    form->addRow(QStringLiteral("Allowed from:"), windowStart);
    form->addRow(QStringLiteral("Allowed until:"), windowEnd);
    form->addRow(QStringLiteral("Allowed days:"), weekdays);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted || directoryEdit->text().trimmed().isEmpty()) {
        return;
    }
    if (restrictWindow->isChecked()
        && std::any_of(validUrls.cbegin(), validUrls.cend(), [](const QUrl& url) {
            return MediaDownloader::supports(url);
        })) {
        QMessageBox::warning(this, QStringLiteral("Batch Download"),
                             QStringLiteral("Schedule windows are not yet available for adaptive HLS/DASH jobs. Remove those URLs or disable the window."));
        return;
    }
    int allowedWeekdays = 0;
    if (restrictWindow->isChecked()) {
        for (int day = 0; day < weekdayChecks.size(); ++day) {
            if (weekdayChecks[day]->isChecked()) {
                allowedWeekdays |= 1 << day;
            }
        }
        if (allowedWeekdays == 0) {
            QMessageBox::warning(this, QStringLiteral("Batch Download"), QStringLiteral("Select at least one allowed weekday."));
            return;
        }
    }

    QSet<QString> names;
    int unnamed = 1;
    for (const auto& url : std::as_const(validUrls)) {
        const bool adaptiveMedia = MediaDownloader::supports(url);
        QString name = url.fileName();
        if (name.isEmpty()) {
            name = QStringLiteral("download-%1.bin").arg(unnamed++);
        }
        if (adaptiveMedia) {
            name = QFileInfo(name).completeBaseName() + QStringLiteral(".mkv");
        }
        const QFileInfo nameInfo(name);
        const auto base = nameInfo.completeBaseName();
        const auto suffix = nameInfo.completeSuffix();
        int copy = 1;
        while (names.contains(name.toLower())) {
            name = suffix.isEmpty()
                ? QStringLiteral("%1 (%2)").arg(base).arg(copy++)
                : QStringLiteral("%1 (%2).%3").arg(base).arg(copy++).arg(suffix);
        }
        names.insert(name.toLower());
        const auto targetPath = QDir(directoryEdit->text().trimmed()).filePath(name);
        if (adaptiveMedia) {
            mediaDownloader_.enqueue(url, targetPath, headers);
            continue;
        }
        DownloadRequest request;
        request.url = url;
        request.targetPath = targetPath;
        request.category = categoryEdit->text().trimmed();
        request.queueName = queueEdit->text().trimmed();
        request.headers = headers;
        request.segments = segments->value();
        request.dynamicSegmentation = dynamicSegments->isChecked();
        request.perHostConnectionLimit = hostConnections->value();
        request.fileConflictPolicy = static_cast<FileConflictPolicy>(conflictPolicy->currentData().toInt());
        request.completionCommand = completionCommand->text().trimmed();
        if (restrictWindow->isChecked()) {
            request.windowStart = windowStart->time();
            request.windowEnd = windowEnd->time();
            request.allowedWeekdays = allowedWeekdays;
        }
        scheduler_.schedule(std::move(request));
    }
    statusBar()->showMessage(QStringLiteral("%1 batch downloads added").arg(validUrls.size()), 5000);
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
    auto* idItem = new QTableWidgetItem(record.id);
    idItem->setData(Qt::UserRole, record.category);
    downloads_->setItem(row, 0, idItem);
    downloads_->setItem(row, 1, new QTableWidgetItem(record.url.toString()));
    downloads_->setItem(row, 2, new QTableWidgetItem(record.targetPath));
    downloads_->setItem(row, 3, new QTableWidgetItem(statusText(record.status)));
    const int percent = record.totalBytes > 0 ? static_cast<int>((record.completedBytes * 100) / record.totalBytes) : 0;
    downloads_->setItem(row, 4, new QTableWidgetItem(QString::number(percent) + QLatin1Char('%')));
    downloads_->setItem(row, 5, new QTableWidgetItem(QStringLiteral("0 B/s")));
    auto* grid = new SegmentGrid(downloads_);
    grid->setSegments(record.segments);
    downloads_->setCellWidget(row, 6, grid);
    applyCategoryFilter();
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
    if (status == DownloadStatus::Failed) {
        Logger::error(QStringLiteral("Download %1").arg(id),
                      message.isEmpty() ? QStringLiteral("Download failed without a reported cause") : message);
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
    auto* form = new QFormLayout;
    auto* queue = new QComboBox;
    auto names = scheduler_.queueNames();
    if (!names.contains(QStringLiteral("Main"), Qt::CaseInsensitive)) {
        names.prepend(QStringLiteral("Main"));
    }
    queue->addItems(names);
    auto* concurrency = new QSpinBox;
    concurrency->setRange(1, 64);
    auto* enabled = new QCheckBox(QStringLiteral("Dispatch downloads from this queue"));
    form->addRow(QStringLiteral("Queue:"), queue);
    form->addRow(QStringLiteral("Concurrent downloads:"), concurrency);
    form->addRow(QStringLiteral("State:"), enabled);
    layout->addLayout(form);

    auto* view = new QTableWidget;
    view->setColumnCount(5);
    view->setHorizontalHeaderLabels({
        QStringLiteral("ID"),
        QStringLiteral("Queue"),
        QStringLiteral("Priority"),
        QStringLiteral("Scheduled"),
        QStringLiteral("URL")
    });
    view->setColumnHidden(0, true);
    view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view->setSelectionBehavior(QAbstractItemView::SelectRows);
    view->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(view);

    auto* entryForm = new QFormLayout;
    auto* entryQueue = new QComboBox;
    entryQueue->setEditable(true);
    entryQueue->addItems(names);
    auto* entryPriority = new QSpinBox;
    entryPriority->setRange(-100, 100);
    auto* entryTime = new QDateTimeEdit;
    entryTime->setCalendarPopup(true);
    entryTime->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    entryForm->addRow(QStringLiteral("Selected queue:"), entryQueue);
    entryForm->addRow(QStringLiteral("Selected priority:"), entryPriority);
    entryForm->addRow(QStringLiteral("Selected start:"), entryTime);
    layout->addLayout(entryForm);

    const auto refresh = [this, view] {
        view->setRowCount(0);
        for (const auto& request : scheduler_.queued()) {
            const int row = view->rowCount();
            view->insertRow(row);
            view->setItem(row, 0, new QTableWidgetItem(request.scheduleId));
            view->setItem(row, 1, new QTableWidgetItem(request.queueName));
            view->setItem(row, 2, new QTableWidgetItem(QString::number(request.priority)));
            view->setItem(row, 3, new QTableWidgetItem(request.scheduledAt.toString(Qt::ISODateWithMs)));
            view->setItem(row, 4, new QTableWidgetItem(request.url.toString()));
        }
        view->resizeColumnsToContents();
    };
    refresh();

    connect(view, &QTableWidget::itemSelectionChanged, &dialog,
            [view, entryQueue, entryPriority, entryTime] {
                const int row = view->currentRow();
                if (row < 0) {
                    return;
                }
                entryQueue->setCurrentText(view->item(row, 1)->text());
                entryPriority->setValue(view->item(row, 2)->text().toInt());
                entryTime->setDateTime(
                    QDateTime::fromString(view->item(row, 3)->text(), Qt::ISODateWithMs).toLocalTime());
            });

    const auto loadControls = [this, queue, concurrency, enabled] {
        const auto name = queue->currentText();
        concurrency->setValue(scheduler_.queueConcurrency(name));
        enabled->setChecked(scheduler_.isQueueEnabled(name));
    };
    connect(queue, &QComboBox::currentTextChanged, &dialog, loadControls);
    loadControls();

    auto* entryButtons = new QDialogButtonBox;
    auto* moveUp = entryButtons->addButton(QStringLiteral("Move Up"), QDialogButtonBox::ActionRole);
    auto* moveDown = entryButtons->addButton(QStringLiteral("Move Down"), QDialogButtonBox::ActionRole);
    auto* edit = entryButtons->addButton(QStringLiteral("Apply Entry Changes"), QDialogButtonBox::ActionRole);
    auto* remove = entryButtons->addButton(QStringLiteral("Remove"), QDialogButtonBox::DestructiveRole);
    const auto selectedId = [view] {
        const int row = view->currentRow();
        const auto* item = row >= 0 ? view->item(row, 0) : nullptr;
        return item ? item->text() : QString {};
    };
    connect(moveUp, &QPushButton::clicked, &dialog, [this, selectedId, refresh] {
        if (scheduler_.moveScheduled(selectedId(), -1)) {
            refresh();
        }
    });
    connect(moveDown, &QPushButton::clicked, &dialog, [this, selectedId, refresh] {
        if (scheduler_.moveScheduled(selectedId(), 1)) {
            refresh();
        }
    });
    connect(edit, &QPushButton::clicked, &dialog,
            [this, selectedId, entryQueue, entryPriority, entryTime, refresh] {
                if (scheduler_.updateScheduled(selectedId(), entryQueue->currentText(),
                                               entryPriority->value(), entryTime->dateTime())) {
                    refresh();
                }
            });
    connect(remove, &QPushButton::clicked, &dialog, [this, selectedId, refresh] {
        if (scheduler_.removeScheduled(selectedId())) {
            refresh();
        }
    });
    layout->addWidget(entryButtons);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Close);
    connect(buttons->button(QDialogButtonBox::Save), &QPushButton::clicked, &dialog,
            [this, queue, concurrency, enabled] {
                scheduler_.setQueueConcurrency(queue->currentText(), concurrency->value());
                scheduler_.setQueueEnabled(queue->currentText(), enabled->isChecked());
                statusBar()->showMessage(
                    enabled->isChecked()
                        ? QStringLiteral("Queue enabled")
                        : QStringLiteral("Queue paused; active downloads continue"),
                    4000);
            });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.resize(900, 560);
    dialog.exec();
}

void MainWindow::showOptions()
{
    QSettings settings;
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Options"));
    auto* form = new QFormLayout(&dialog);
    auto* defaultSegments = new QSpinBox;
    defaultSegments->setRange(1, 32);
    defaultSegments->setValue(settings.value(QStringLiteral("downloads/defaultSegments"), 8).toInt());
    auto* defaultHostConnections = new QSpinBox;
    defaultHostConnections->setRange(1, 128);
    defaultHostConnections->setValue(settings.value(QStringLiteral("downloads/defaultHostConnections"), 16).toInt());
    auto* defaultSpeedLimit = new QSpinBox;
    defaultSpeedLimit->setRange(0, 1024 * 1024);
    defaultSpeedLimit->setSuffix(QStringLiteral(" KB/s"));
    defaultSpeedLimit->setValue(settings.value(QStringLiteral("downloads/defaultSpeedLimitKib"), 0).toInt());
    auto* defaultConflictPolicy = new QComboBox;
    defaultConflictPolicy->addItems({
        QStringLiteral("Rename automatically"),
        QStringLiteral("Overwrite existing file"),
        QStringLiteral("Skip download")
    });
    defaultConflictPolicy->setCurrentIndex(qBound(0, settings.value(QStringLiteral("downloads/defaultConflictPolicy"), 0).toInt(), 2));
    auto* clipboardMonitor = new QCheckBox(QStringLiteral("Clipboard monitor"));
    clipboardMonitor->setChecked(settings.value(QStringLiteral("integration/clipboardMonitor"), false).toBool());
    form->addRow(QStringLiteral("Default segments:"), defaultSegments);
    form->addRow(QStringLiteral("Default per-host connections:"), defaultHostConnections);
    form->addRow(QStringLiteral("Default speed limit:"), defaultSpeedLimit);
    form->addRow(QStringLiteral("If file exists:"), defaultConflictPolicy);
    form->addRow(QStringLiteral("Integration:"), clipboardMonitor);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    form->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    settings.setValue(QStringLiteral("downloads/defaultSegments"), defaultSegments->value());
    settings.setValue(QStringLiteral("downloads/defaultHostConnections"), defaultHostConnections->value());
    settings.setValue(QStringLiteral("downloads/defaultSpeedLimitKib"), defaultSpeedLimit->value());
    settings.setValue(QStringLiteral("downloads/defaultConflictPolicy"), defaultConflictPolicy->currentIndex());
    settings.setValue(QStringLiteral("integration/clipboardMonitor"), clipboardMonitor->isChecked());
    configureClipboardMonitor();
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
        if (mediaDownloader_.contains(record.id)) {
            mediaDownloader_.resume(record.id);
            return;
        }
        if (MediaDownloader::supports(record.url)) {
            mediaDownloader_.enqueue(record.url, record.targetPath, {}, record.id);
            return;
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
        const auto id = downloads_->item(selected.first()->row(), 0)->text();
        if (mediaDownloader_.contains(id)) {
            mediaDownloader_.pause(id);
        } else {
            downloader_.pause(id);
        }
    }
}

void MainWindow::cancelSelected()
{
    const auto selected = downloads_->selectedItems();
    if (!selected.isEmpty()) {
        const auto id = downloads_->item(selected.first()->row(), 0)->text();
        if (mediaDownloader_.contains(id)) {
            mediaDownloader_.cancel(id);
        } else {
            downloader_.cancel(id);
        }
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
    if (mediaDownloader_.contains(id)) {
        mediaDownloader_.cancel(id);
    } else {
        downloader_.cancel(id);
    }
    repository_.removeDownload(id);
    downloads_->removeRow(row);
}

void MainWindow::propertiesSelected()
{
    const auto selected = downloads_->selectedItems();
    if (selected.isEmpty()) {
        return;
    }
    const int row = selected.first()->row();
    const auto id = downloads_->item(row, 0)->text();
    const auto records = repository_.listDownloads();
    const auto it = std::find_if(records.cbegin(), records.cend(), [&id](const DownloadRecord& record) {
        return record.id == id;
    });
    if (it == records.cend()) {
        QMessageBox::warning(this, QStringLiteral("Download Properties"), QStringLiteral("The selected download no longer exists in the database."));
        return;
    }
    if (it->status == DownloadStatus::Downloading || it->status == DownloadStatus::Connecting) {
        QMessageBox::information(this, QStringLiteral("Download Properties"), QStringLiteral("Pause the download before changing its source URL."));
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Download Properties"));
    auto* form = new QFormLayout(&dialog);
    auto* urlEdit = new QLineEdit(it->url.toString());
    auto* categoryEdit = new QLineEdit(it->category);
    auto* destination = new QLineEdit(it->targetPath);
    destination->setReadOnly(true);
    auto* progress = new QLabel(QStringLiteral("%1 / %2 bytes preserved")
                                    .arg(it->completedBytes)
                                    .arg(it->totalBytes));
    progress->setWordWrap(true);
    form->addRow(QStringLiteral("Source URL:"), urlEdit);
    form->addRow(QStringLiteral("Destination:"), destination);
    form->addRow(QStringLiteral("Category:"), categoryEdit);
    form->addRow(QStringLiteral("Resume state:"), progress);
    auto* note = new QLabel(QStringLiteral("Replacing an expired URL preserves downloaded ranges. Resume will reject the new source if its size differs."));
    note->setWordWrap(true);
    form->addRow(note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const QUrl replacement(urlEdit->text().trimmed());
    if (!replacement.isValid()
        || (replacement.scheme() != QStringLiteral("http") && replacement.scheme() != QStringLiteral("https"))) {
        QMessageBox::warning(this, QStringLiteral("Download Properties"), QStringLiteral("The replacement must be a valid HTTP or HTTPS URL."));
        return;
    }
    auto updated = *it;
    updated.url = replacement;
    updated.category = categoryEdit->text().trimmed();
    updated.updatedAt = QDateTime::currentDateTimeUtc();
    if (!repository_.upsertDownload(updated)) {
        QMessageBox::warning(this, QStringLiteral("Download Properties"), repository_.lastError());
        return;
    }
    downloads_->item(row, 1)->setText(replacement.toString());
    downloads_->item(row, 0)->setData(Qt::UserRole, updated.category);
    applyCategoryFilter();
    statusBar()->showMessage(QStringLiteral("Download properties updated; use Start to resume"), 5000);
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
    toolbar->addAction(QIcon::fromTheme(QStringLiteral("document-properties")), QStringLiteral("Properties"), this, &MainWindow::propertiesSelected);
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
    categories_->setCurrentItem(categories_->topLevelItem(0));
    connect(categories_, &QTreeWidget::currentItemChanged, this, [this] { applyCategoryFilter(); });

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

void MainWindow::configureClipboardMonitor()
{
    if (clipboardConnection_) {
        disconnect(clipboardConnection_);
        clipboardConnection_ = {};
    }
    if (!QSettings().value(QStringLiteral("integration/clipboardMonitor"), false).toBool()) {
        return;
    }
    auto* clipboard = QApplication::clipboard();
    clipboardConnection_ = connect(clipboard, &QClipboard::dataChanged, this, [this, clipboard] {
        const auto text = clipboard->text().trimmed();
        const QUrl url(text);
        if (text == lastClipboardUrl_ || !url.isValid()
            || (url.scheme() != QStringLiteral("http") && url.scheme() != QStringLiteral("https"))) {
            return;
        }
        lastClipboardUrl_ = text;
        QTimer::singleShot(0, this, [this, text] { addUrl(text); });
    });
}

void MainWindow::applyCategoryFilter()
{
    if (!downloads_ || !categories_ || !categories_->currentItem()) {
        return;
    }
    const auto selected = categories_->currentItem()->text(0);
    for (int row = 0; row < downloads_->rowCount(); ++row) {
        const auto* idItem = downloads_->item(row, 0);
        const auto category = idItem ? idItem->data(Qt::UserRole).toString() : QString {};
        downloads_->setRowHidden(row,
            selected != QStringLiteral("All Downloads")
            && category.compare(selected, Qt::CaseInsensitive) != 0);
    }
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
