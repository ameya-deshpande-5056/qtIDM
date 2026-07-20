#include "ui/MainWindow.h"

#include "app/Logger.h"
#include "core/ArchiveExtractor.h"
#include "core/SiteGrabber.h"
#include "core/ZipPreview.h"
#include "integration/ImportExportService.h"
#include "ui/SegmentGrid.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QClipboard>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTimeEdit>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopedValueRollback>
#include <QScrollArea>
#include <QSplitter>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStatusBar>
#include <QSet>
#include <QSettings>
#include <QTextStream>
#include <QToolBar>
#include <QToolButton>
#include <QTimeEdit>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

#ifndef QTIDM_BUILD_TIMESTAMP
#define QTIDM_BUILD_TIMESTAMP "unknown"
#endif

#ifndef QTIDM_PROJECT_URL
#define QTIDM_PROJECT_URL "https://github.com/ameya-deshpande-5056/qtIDM"
#endif

namespace qtidm {

namespace {
QString categoryForName(const QString& value)
{
    const auto suffix = QFileInfo(value).suffix().toLower();
    static const QSet<QString> compressed {
        QStringLiteral("7z"), QStringLiteral("bz2"), QStringLiteral("gz"), QStringLiteral("iso"),
        QStringLiteral("rar"), QStringLiteral("tar"), QStringLiteral("xz"), QStringLiteral("zip"),
        QStringLiteral("zst")
    };
    static const QSet<QString> documents {
        QStringLiteral("csv"), QStringLiteral("doc"), QStringLiteral("docx"), QStringLiteral("epub"),
        QStringLiteral("html"), QStringLiteral("md"), QStringLiteral("ods"), QStringLiteral("odt"),
        QStringLiteral("pdf"), QStringLiteral("ppt"), QStringLiteral("pptx"), QStringLiteral("rtf"),
        QStringLiteral("srt"), QStringLiteral("txt"), QStringLiteral("vtt"), QStringLiteral("xls"),
        QStringLiteral("xlsx")
    };
    static const QSet<QString> music {
        QStringLiteral("aac"), QStringLiteral("flac"), QStringLiteral("m4a"), QStringLiteral("mp3"),
        QStringLiteral("oga"), QStringLiteral("ogg"), QStringLiteral("opus"), QStringLiteral("wav"),
        QStringLiteral("wma")
    };
    static const QSet<QString> programs {
        QStringLiteral("apk"), QStringLiteral("appimage"), QStringLiteral("bin"), QStringLiteral("deb"),
        QStringLiteral("exe"), QStringLiteral("flatpak"), QStringLiteral("flatpakref"), QStringLiteral("msi"),
        QStringLiteral("rpm"), QStringLiteral("run"), QStringLiteral("sh")
    };
    static const QSet<QString> videos {
        QStringLiteral("avi"), QStringLiteral("flv"), QStringLiteral("m2ts"), QStringLiteral("m3u8"),
        QStringLiteral("m4v"), QStringLiteral("mkv"), QStringLiteral("mov"), QStringLiteral("mp4"),
        QStringLiteral("mpd"), QStringLiteral("mpeg"), QStringLiteral("mpg"), QStringLiteral("ts"),
        QStringLiteral("webm"), QStringLiteral("wmv")
    };
    static const QSet<QString> images {
        QStringLiteral("avif"), QStringLiteral("bmp"), QStringLiteral("gif"), QStringLiteral("heic"),
        QStringLiteral("jpeg"), QStringLiteral("jpg"), QStringLiteral("png"), QStringLiteral("svg"),
        QStringLiteral("tif"), QStringLiteral("tiff"), QStringLiteral("webp")
    };
    if (compressed.contains(suffix)) return QStringLiteral("Compressed");
    if (documents.contains(suffix)) return QStringLiteral("Documents");
    if (music.contains(suffix)) return QStringLiteral("Music");
    if (programs.contains(suffix)) return QStringLiteral("Programs");
    if (videos.contains(suffix)) return QStringLiteral("Video");
    if (images.contains(suffix)) return QStringLiteral("Images");
    return QStringLiteral("General");
}

QString downloadFolderForCategory(const QString& category)
{
    const auto normalized = category.trimmed();
    if (normalized.compare(QStringLiteral("Video"), Qt::CaseInsensitive) == 0) return QStringLiteral("Videos");
    if (normalized.compare(QStringLiteral("Images"), Qt::CaseInsensitive) == 0) return QStringLiteral("Images");
    if (normalized.compare(QStringLiteral("General"), Qt::CaseInsensitive) == 0) return QStringLiteral("Others");
    const auto safe = normalized.simplified().remove(QRegularExpression(QStringLiteral("[^A-Za-z0-9 _-]")));
    return safe.isEmpty() || safe == QStringLiteral(".") || safe == QStringLiteral("..")
        ? QStringLiteral("Others") : safe;
}

QString defaultDownloadPath(const QString& name, const QString& category)
{
    const QDir downloads(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
    return downloads.filePath(downloadFolderForCategory(category) + QLatin1Char('/') + name);
}

bool containsHeader(const QVariantMap& headers, QStringView name)
{
    for (auto it = headers.cbegin(); it != headers.cend(); ++it) {
        if (QStringView(it.key()).compare(name, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

QString takeHeader(QVariantMap* headers, QStringView name)
{
    for (auto it = headers->begin(); it != headers->end(); ++it) {
        if (QStringView(it.key()).compare(name, Qt::CaseInsensitive) == 0) {
            const auto value = it.value().toString();
            headers->erase(it);
            return value;
        }
    }
    return {};
}

QString safeSuggestedFilename(QString value)
{
    value = value.trimmed();
    value = value.section(QLatin1Char('/'), -1);
    value.remove(QRegularExpression(QStringLiteral("[\\x{0}-\\x{1f}\\x{7f}]")));
    if (value == QStringLiteral(".") || value == QStringLiteral("..")) {
        return {};
    }
    return value;
}

bool mergeHeaderLines(const QString& text, QVariantMap* headers, QString* error)
{
    const auto lines = text.split(QLatin1Char('\n'));
    for (int index = 0; index < lines.size(); ++index) {
        const auto line = lines[index].trimmed();
        if (line.isEmpty()) {
            continue;
        }
        const int colon = line.indexOf(QLatin1Char(':'));
        if (colon <= 0 || line.left(colon).contains(QLatin1Char('\r'))
            || line.mid(colon + 1).contains(QLatin1Char('\r'))) {
            *error = QStringLiteral("Header line %1 must use Name: value format.").arg(index + 1);
            return false;
        }
        headers->insert(line.left(colon).trimmed(), line.mid(colon + 1).trimmed());
    }
    return true;
}

QCryptographicHash::Algorithm checksumAlgorithm(const QString& name)
{
    if (name == QStringLiteral("MD5")) return QCryptographicHash::Md5;
    if (name == QStringLiteral("SHA-1")) return QCryptographicHash::Sha1;
    if (name == QStringLiteral("SHA-512")) return QCryptographicHash::Sha512;
    return QCryptographicHash::Sha256;
}

QString formatBytes(qint64 bytes)
{
    if (bytes < 0) return QStringLiteral("Unknown");
    static const QStringList units { QStringLiteral("B"), QStringLiteral("KiB"), QStringLiteral("MiB"),
        QStringLiteral("GiB"), QStringLiteral("TiB") };
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit + 1 < units.size()) {
        value /= 1024.0;
        ++unit;
    }
    return unit == 0
        ? QStringLiteral("%1 %2").arg(bytes).arg(units[unit])
        : QStringLiteral("%1 %2").arg(value, 0, 'f', value >= 100.0 ? 0 : 1).arg(units[unit]);
}

QString formatDuration(qint64 seconds)
{
    if (seconds < 0) return QStringLiteral("—");
    const auto hours = seconds / 3600;
    const auto minutes = (seconds % 3600) / 60;
    const auto remaining = seconds % 60;
    return hours > 0
        ? QStringLiteral("%1:%2:%3").arg(hours).arg(minutes, 2, 10, QLatin1Char('0')).arg(remaining, 2, 10, QLatin1Char('0'))
        : QStringLiteral("%1:%2").arg(minutes).arg(remaining, 2, 10, QLatin1Char('0'));
}

bool isActiveStatus(DownloadStatus status)
{
    return status == DownloadStatus::Connecting || status == DownloadStatus::Downloading;
}

bool isResumableStatus(DownloadStatus status)
{
    return status == DownloadStatus::Paused || status == DownloadStatus::Failed
        || status == DownloadStatus::Canceled;
}

bool isStoppableStatus(DownloadStatus status)
{
    return status == DownloadStatus::Queued || isActiveStatus(status)
        || status == DownloadStatus::Paused;
}

QIcon staticActionIcon(const QString& name)
{
    return QIcon(QStringLiteral(":/qtidm/icons/actions/%1.png").arg(name));
}
}

MainWindow::MainWindow(CurlEpollDownloader& downloader, DownloadScheduler& scheduler, DownloadRepository& repository, ThemeManager& themeManager, QWidget* parent)
    : QMainWindow(parent)
    , downloader_(downloader)
    , mediaDownloader_(this)
    , credentialVault_()
    , networkMonitor_()
    , socialMediaExtractor_()
    , scheduler_(scheduler)
    , repository_(repository)
    , themeManager_(themeManager)
{
    QSettings settings;
    transferClock_.start();
    const bool alternateLimitEnabled =
        settings.value(QStringLiteral("downloads/alternateSpeedLimitEnabled"), false).toBool();
    const qint64 alternateLimit = static_cast<qint64>(
        qMax(1, settings.value(QStringLiteral("downloads/alternateSpeedLimitKib"), 1024).toInt())) * 1024;
    downloader_.setAlternateSpeedLimit(alternateLimitEnabled ? alternateLimit : 0);
    scheduler_.setCredentialVault(&credentialVault_);
    scheduler_.setMeteredNetworkPolicy(static_cast<MeteredNetworkPolicy>(
        qBound(0, settings.value(QStringLiteral("downloads/meteredPolicy"), 0).toInt(), 2)));
    scheduler_.setNetworkMetered(networkMonitor_.isMetered());
    connect(&networkMonitor_, &MeteredNetworkMonitor::meteredChanged,
        &scheduler_, &DownloadScheduler::setNetworkMetered);
    setWindowTitle(QStringLiteral("qtIDM"));
    resize(1080, 720);
    buildActions();
    buildLayout();
    setAlternateSpeedLimit(alternateLimitEnabled, alternateLimit);
    buildTray();
    loadPersistedDownloads();
    downloads_->setSortingEnabled(true);
    applyTheme();
    configureClipboardMonitor();

    connect(&downloader_, &CurlEpollDownloader::downloadAdded, this, &MainWindow::onDownloadAdded);
    connect(&downloader_, &CurlEpollDownloader::progressChanged, this, &MainWindow::onProgressChanged);
    connect(&downloader_, &CurlEpollDownloader::segmentsChanged, this, &MainWindow::onSegmentsChanged);
    connect(&downloader_, &CurlEpollDownloader::metadataChanged, this,
        [this](const QString& id, qint64 total, const QString& entityTag, const QString& lastModified) {
            const auto records = repository_.listDownloads();
            const auto it = std::find_if(records.cbegin(), records.cend(), [&id](const DownloadRecord& record) {
                return record.id == id;
            });
            if (it == records.cend()) {
                return;
            }
            auto updated = *it;
            if (total >= 0) {
                updated.totalBytes = total;
                updated.request.expectedTotalBytes = total;
            }
            updated.request.entityTag = entityTag;
            updated.request.lastModified = lastModified;
            updated.updatedAt = QDateTime::currentDateTimeUtc();
            repository_.upsertDownload(updated);
        });
    connect(&downloader_, &CurlEpollDownloader::statusChanged, this, &MainWindow::onStatusChanged);
    connect(&mediaDownloader_, &MediaDownloader::downloadAdded, this, &MainWindow::onDownloadAdded);
    connect(&mediaDownloader_, &MediaDownloader::progressChanged, this, &MainWindow::onProgressChanged);
    connect(&mediaDownloader_, &MediaDownloader::statusChanged, this, &MainWindow::onStatusChanged);
    connect(&themeManager_, &ThemeManager::themeChanged, this, &MainWindow::applyTheme);
    connect(&scheduler_, &DownloadScheduler::completionFileMoved, this,
            [this](const QString& id, const QString& targetPath) {
                const auto records = repository_.listDownloads();
                const auto it = std::find_if(records.cbegin(), records.cend(), [&id](const DownloadRecord& record) {
                    return record.id == id;
                });
                if (it != records.cend()) {
                    auto updated = *it;
                    updated.targetPath = targetPath;
                    updated.request.targetPath = targetPath;
                    updated.updatedAt = QDateTime::currentDateTimeUtc();
                    repository_.upsertDownload(updated);
                }
                const int row = rowForId(id);
                if (row >= 0) {
                    downloads_->item(row, 2)->setText(targetPath);
                    refreshDownloadDetailsFor(id);
                }
            });
    connect(&scheduler_, &DownloadScheduler::completionAutomationFailed, this,
            [this](const QString& id, const QString& message) {
                Logger::error(QStringLiteral("Completion automation %1").arg(id), message);
                statusBar()->showMessage(message, 8000);
            });
    connect(&scheduler_, &DownloadScheduler::archiveExtractionFinished, this,
        [this](const QString&, const QString& destination) {
            statusBar()->showMessage(QStringLiteral("Archive extracted to %1").arg(destination), 8000);
        });
    connect(&scheduler_, &DownloadScheduler::credentialLookupFailed, this,
        [this](const QString&, const QString& message) {
            statusBar()->showMessage(message, 10000);
        });
    connect(&scheduler_, &DownloadScheduler::historyRemovalRequested, this, [this](const QString& id) {
        repository_.removeDownload(id);
        const int row = rowForId(id);
        if (row >= 0) {
            downloads_->removeRow(row);
            updateDownloadActionStates();
        }
    });
}

bool MainWindow::addUrl(QString url, QVariantMap headers)
{
    if (url.isEmpty()) {
        bool ok = false;
        url = QInputDialog::getText(this, QStringLiteral("Add URL"), QStringLiteral("Address:"), QLineEdit::Normal, {}, &ok);
        if (!ok || url.isEmpty()) {
            return false;
        }
    }
    const QUrl parsed = QUrl::fromUserInput(url);
    if (!parsed.isValid() || (parsed.scheme() != QStringLiteral("http")
        && parsed.scheme() != QStringLiteral("https") && parsed.scheme() != QStringLiteral("ftp"))) {
        QMessageBox::warning(this, QStringLiteral("Invalid URL"), QStringLiteral("Enter a valid HTTP, HTTPS, or FTP URL."));
        return false;
    }
    if (hasDuplicateUrl(parsed)
        && QMessageBox::question(this, QStringLiteral("Duplicate URL"),
               QStringLiteral("This URL is already in the download list. Add it again?"))
            != QMessageBox::Yes) {
        return false;
    }

    const auto mediaTypeHint = takeHeader(&headers, u"_qtidmMediaType").trimmed().toUpper();
    const auto suggestedFilename = safeSuggestedFilename(takeHeader(&headers, u"_qtidmSuggestedFilename"));
    const auto httpMethod = takeHeader(&headers, u"_qtidmHttpMethod").trimmed().toUpper();
    const auto requestBody = QByteArray::fromBase64(takeHeader(&headers, u"_qtidmRequestBody").toLatin1());
    const bool browserPost = httpMethod == QStringLiteral("POST");
    const bool adaptiveMedia = MediaDownloader::supports(parsed, mediaTypeHint);
    QString name = suggestedFilename;
    if (name.isEmpty()) {
        name = parsed.fileName().isEmpty() ? QStringLiteral("download.bin") : parsed.fileName();
    }
    if (adaptiveMedia) {
        const auto baseName = QFileInfo(name).completeBaseName();
        name = (baseName.isEmpty() ? QStringLiteral("stream") : baseName) + QStringLiteral(".mkv");
    }
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Download Options"));
    auto* dialogLayout = new QVBoxLayout(&dialog);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    auto* formContents = new QWidget;
    auto* form = new QFormLayout(formContents);
    scroll->setWidget(formContents);
    dialogLayout->addWidget(scroll);
    auto* categoryEdit = new QLineEdit(adaptiveMedia ? QStringLiteral("Video") : categoryForName(name));
    auto* targetEdit = new QLineEdit(defaultDownloadPath(name, categoryEdit->text()));
    auto* queueEdit = new QLineEdit(QStringLiteral("Main"));
    auto* segments = new QSpinBox;
    segments->setRange(1, 32);
    QSettings settings;
    segments->setValue(settings.value(QStringLiteral("downloads/defaultSegments"), 8).toInt());
    auto* dynamicSegments = new QCheckBox(QStringLiteral("Queue additional ranges and refill completed connections"));
    dynamicSegments->setChecked(true);
    if (browserPost) {
        segments->setValue(1);
        segments->setEnabled(false);
        dynamicSegments->setChecked(false);
        dynamicSegments->setEnabled(false);
    }
    auto* hostConnections = new QSpinBox;
    hostConnections->setRange(1, 128);
    hostConnections->setValue(settings.value(QStringLiteral("downloads/defaultHostConnections"), 16).toInt());
    auto* speedLimit = new QSpinBox;
    speedLimit->setRange(0, 1024 * 1024);
    speedLimit->setSuffix(QStringLiteral(" KB/s"));
    speedLimit->setValue(settings.value(QStringLiteral("downloads/defaultSpeedLimitKib"), 0).toInt());
    auto* proxyEdit = new QLineEdit(settings.value(QStringLiteral("downloads/defaultProxy")).toString());
    auto* priority = new QSpinBox;
    priority->setRange(-100, 100);
    priority->setValue(0);
    auto* repeatMinutes = new QSpinBox;
    repeatMinutes->setRange(0, 525600);
    repeatMinutes->setSuffix(QStringLiteral(" min"));
    auto* checksumType = new QComboBox;
    checksumType->addItems({ QStringLiteral("MD5"), QStringLiteral("SHA-1"), QStringLiteral("SHA-256"), QStringLiteral("SHA-512") });
    checksumType->setCurrentText(QStringLiteral("SHA-256"));
    auto* checksumEdit = new QLineEdit;
    checksumEdit->setPlaceholderText(QStringLiteral("Optional expected checksum"));
    auto* checksumRow = new QWidget;
    auto* checksumLayout = new QHBoxLayout(checksumRow);
    checksumLayout->setContentsMargins(0, 0, 0, 0);
    checksumLayout->addWidget(checksumType);
    checksumLayout->addWidget(checksumEdit, 1);
    auto* headerEdit = new QPlainTextEdit;
    headerEdit->setPlaceholderText(QStringLiteral("Optional request headers, one Name: value per line"));
    headerEdit->setMaximumHeight(80);
    QStringList initialHeaderLines;
    for (auto it = headers.cbegin(); it != headers.cend(); ++it) {
        initialHeaderLines.append(it.key() + QStringLiteral(": ") + it.value().toString());
    }
    headerEdit->setPlainText(initialHeaderLines.join(QLatin1Char('\n')));
    auto* completionCommand = new QLineEdit;
    completionCommand->setPlaceholderText(QStringLiteral("Optional executable; arguments may use {file}, {dir}, {url}"));
    auto* completionMoveDirectory = new QLineEdit;
    completionMoveDirectory->setPlaceholderText(QStringLiteral("Optional directory"));
    auto* extractArchive = new QCheckBox(QStringLiteral("Extract supported archives after download"));
    extractArchive->setChecked(settings.value(QStringLiteral("downloads/defaultExtractArchives"), false).toBool());
    auto* archiveDestination = new QLineEdit;
    archiveDestination->setPlaceholderText(QStringLiteral("Automatic folder beside the archive"));
    auto* deleteArchive = new QCheckBox(QStringLiteral("Delete archive after successful extraction"));
    deleteArchive->setChecked(settings.value(QStringLiteral("downloads/defaultDeleteArchives"), false).toBool());
    archiveDestination->setEnabled(extractArchive->isChecked());
    deleteArchive->setEnabled(extractArchive->isChecked());
    connect(extractArchive, &QCheckBox::toggled, archiveDestination, &QWidget::setEnabled);
    connect(extractArchive, &QCheckBox::toggled, deleteArchive, &QWidget::setEnabled);
    auto* removeOnCompletion = new QCheckBox(QStringLiteral("Remove the list entry after a successful download"));
    auto* conflictPolicy = new QComboBox;
    conflictPolicy->addItem(QStringLiteral("Rename automatically"), static_cast<int>(FileConflictPolicy::AutoRename));
    conflictPolicy->addItem(QStringLiteral("Overwrite existing file"), static_cast<int>(FileConflictPolicy::Overwrite));
    conflictPolicy->addItem(QStringLiteral("Skip download"), static_cast<int>(FileConflictPolicy::Skip));
    conflictPolicy->setCurrentIndex(qBound(0, settings.value(QStringLiteral("downloads/defaultConflictPolicy"), 0).toInt(), 2));
    auto* userEdit = new QLineEdit;
    auto* passwordEdit = new QLineEdit;
    passwordEdit->setEchoMode(QLineEdit::Password);
    auto* loadCredential = new QPushButton(QStringLiteral("Load saved"));
    auto* passwordRow = new QWidget;
    auto* passwordLayout = new QHBoxLayout(passwordRow);
    passwordLayout->setContentsMargins(0, 0, 0, 0);
    passwordLayout->addWidget(passwordEdit, 1);
    passwordLayout->addWidget(loadCredential);
    auto* storeCredential = new QCheckBox(QStringLiteral("Store and reuse through the system credential vault"));
    storeCredential->setChecked(credentialVault_.isAvailable());
    storeCredential->setEnabled(credentialVault_.isAvailable());
    if (!credentialVault_.isAvailable()) {
        storeCredential->setToolTip(QStringLiteral("Install secret-tool/libsecret to enable Secret Service storage."));
        loadCredential->setEnabled(false);
    }
    connect(loadCredential, &QPushButton::clicked, this, [this, parsed, userEdit, passwordEdit, storeCredential] {
        const auto key = credentialVault_.keyFor(parsed, userEdit->text());
        QString error;
        const auto password = credentialVault_.lookup(key, &error);
        if (password.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("Credential Vault"),
                error.isEmpty() ? QStringLiteral("No saved credential matched this site and username.") : error);
            return;
        }
        passwordEdit->setText(password);
        storeCredential->setChecked(true);
    });
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
    form->addRow(QStringLiteral("Checksum:"), checksumRow);
    form->addRow(QStringLiteral("HTTP headers:"), headerEdit);
    form->addRow(QStringLiteral("After completion:"), completionCommand);
    form->addRow(QStringLiteral("Move after completion:"), completionMoveDirectory);
    form->addRow(QStringLiteral("Archives:"), extractArchive);
    form->addRow(QStringLiteral("Extract to:"), archiveDestination);
    form->addRow(QStringLiteral("After extraction:"), deleteArchive);
    form->addRow(QStringLiteral("Successful download:"), removeOnCompletion);
    form->addRow(QStringLiteral("If file exists:"), conflictPolicy);
    form->addRow(QStringLiteral("User:"), userEdit);
    form->addRow(QStringLiteral("Password:"), passwordRow);
    form->addRow(QStringLiteral("Credential storage:"), storeCredential);
    form->addRow(QStringLiteral("Start at:"), scheduledAt);
    form->addRow(QStringLiteral("Schedule window:"), restrictWindow);
    form->addRow(QStringLiteral("Allowed from:"), windowStart);
    form->addRow(QStringLiteral("Allowed until:"), windowEnd);
    form->addRow(QStringLiteral("Allowed days:"), weekdays);
    if (adaptiveMedia) {
        segments->setEnabled(false);
        speedLimit->setEnabled(false);
        scheduledAt->setEnabled(false);
        proxyEdit->setEnabled(false);
        checksumType->setEnabled(false);
        checksumEdit->setEnabled(false);
        completionCommand->setEnabled(false);
        completionMoveDirectory->setEnabled(false);
        extractArchive->setEnabled(false);
        archiveDestination->setEnabled(false);
        deleteArchive->setEnabled(false);
        removeOnCompletion->setEnabled(false);
        userEdit->setEnabled(false);
        passwordEdit->setEnabled(false);
        loadCredential->setEnabled(false);
        storeCredential->setEnabled(false);
        form->addRow(QStringLiteral("Media:"), new QLabel(QStringLiteral("HLS/DASH audio and video will be downloaded and muxed with FFmpeg.")));
    }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addWidget(buttons);
    dialog.resize(760, 780);
    if (dialog.exec() != QDialog::Accepted || targetEdit->text().isEmpty()) {
        return false;
    }
    if (!QDir().mkpath(QFileInfo(targetEdit->text()).absolutePath())) {
        QMessageBox::warning(this, QStringLiteral("Download Options"),
            QStringLiteral("Could not create the destination folder."));
        return false;
    }

    QVariantMap editedHeaders = std::move(headers);
    QString headerError;
    if (!mergeHeaderLines(headerEdit->toPlainText(), &editedHeaders, &headerError)) {
        QMessageBox::warning(this, QStringLiteral("Invalid Headers"), headerError);
        return false;
    }
    if (adaptiveMedia) {
        mediaDownloader_.enqueue(parsed, targetEdit->text(), editedHeaders);
        return true;
    }

    DownloadRequest request;
    request.url = parsed;
    request.targetPath = targetEdit->text();
    request.category = categoryEdit->text();
    request.queueName = queueEdit->text().trimmed();
    request.priority = priority->value();
    request.repeatIntervalSeconds = repeatMinutes->value() * 60;
    request.headers = std::move(editedHeaders);
    request.httpMethod = browserPost ? QStringLiteral("POST") : QString {};
    request.requestBody = browserPost ? requestBody : QByteArray {};
    request.segments = segments->value();
    request.dynamicSegmentation = dynamicSegments->isChecked();
    request.perHostConnectionLimit = hostConnections->value();
    request.proxyUrl = proxyEdit->text();
    request.checksumAlgorithm = checksumType->currentText().toLower().remove(QLatin1Char('-'));
    request.expectedChecksum = checksumEdit->text().simplified().remove(QLatin1Char(' ')).toLower();
    request.completionCommand = completionCommand->text().trimmed();
    request.completionMoveDirectory = completionMoveDirectory->text().trimmed();
    request.extractArchive = extractArchive->isChecked();
    request.archiveDestination = archiveDestination->text().trimmed();
    request.deleteArchiveAfterExtraction = deleteArchive->isChecked();
    request.removeRecordOnCompletion = removeOnCompletion->isChecked();
    request.fileConflictPolicy = static_cast<FileConflictPolicy>(conflictPolicy->currentData().toInt());
    request.username = userEdit->text();
    request.password = passwordEdit->text();
    if (request.extractArchive && !ArchiveExtractor::supports(request.targetPath)) {
        QMessageBox::warning(this, QStringLiteral("Download Options"),
            QStringLiteral("Automatic extraction supports ZIP, 7z, RAR, TAR, GZip, BZip2, and XZ archives."));
        return false;
    }
    if (storeCredential->isChecked() && !request.password.isEmpty()) {
        request.credentialVaultKey = credentialVault_.keyFor(request.url, request.username);
        QString vaultError;
        if (!credentialVault_.store(request.credentialVaultKey, request.url,
                request.username, request.password, &vaultError)) {
            QMessageBox::warning(this, QStringLiteral("Credential Vault"), vaultError);
            return false;
        }
        request.password.clear();
    }
    request.speedLimitBytesPerSecond = static_cast<qint64>(speedLimit->value()) * 1024;
    applyRequestDefaults(request);
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
            return false;
        }
    }
    scheduler_.schedule(std::move(request));
    return true;
}

bool MainWindow::addBrowserDownloads(QVariantList downloads)
{
    // QMessageBox/QDialog::exec() runs a nested event loop. Without this guard,
    // another D-Bus request can enter this method and stack a second modal
    // dialog, eventually making the application impossible to control.
    if (browserRequestActive_ || downloads.isEmpty() || downloads.size() > 100) {
        return false;
    }
    QScopedValueRollback requestGuard(browserRequestActive_, true);

    QStringList urls;
    urls.reserve(downloads.size());
    for (const auto& value : std::as_const(downloads)) {
        const auto urlText = value.toMap().value(QStringLiteral("url")).toString().trimmed();
        const QUrl url(urlText);
        if (!url.isValid() || (url.scheme() != QStringLiteral("http")
            && url.scheme() != QStringLiteral("https") && url.scheme() != QStringLiteral("ftp"))) {
            // Browser/native-messaging validation failures are returned to the
            // sender instead of shown modally. A broken extension may retry
            // continuously, but it must never take over the desktop UI.
            return false;
        }
        urls.append(urlText);
    }
    // Keep protocol-specific metadata out of the editable header field.  The
    // batch dialog consumes it per row after the user accepts shared options.
    return addUrls(std::move(urls), { { QStringLiteral("_qtidmBrowserDownloads"), downloads } });
}

bool MainWindow::addUrls(QStringList urls, QVariantMap headers)
{
    const auto browserDownloads = headers.take(QStringLiteral("_qtidmBrowserDownloads")).toList();
    if (browserDownloads.isEmpty()) {
        urls.removeDuplicates();
    }
    QList<QUrl> validUrls;
    for (const auto& value : std::as_const(urls)) {
        const auto url = QUrl::fromUserInput(value.trimmed());
        if (url.isValid() && (url.scheme() == QStringLiteral("http")
            || url.scheme() == QStringLiteral("https") || url.scheme() == QStringLiteral("ftp"))) {
            validUrls.append(url);
        }
    }
    if (validUrls.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Batch Download"), QStringLiteral("The batch does not contain any valid HTTP, HTTPS, or FTP URLs."));
        return false;
    }
    const auto duplicateCount = std::count_if(validUrls.cbegin(), validUrls.cend(), [this](const QUrl& value) {
        return hasDuplicateUrl(value);
    });
    if (duplicateCount > 0
        && QMessageBox::question(this, QStringLiteral("Duplicate URLs"),
               QStringLiteral("%1 URL(s) already exist in the download list. Add them again?").arg(duplicateCount))
            != QMessageBox::Yes) {
        return false;
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

    auto* categoryEdit = new QLineEdit(QStringLiteral("Auto"));
    categoryEdit->setPlaceholderText(QStringLiteral("Auto or a custom category"));
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
    completionCommand->setPlaceholderText(QStringLiteral("Optional executable; arguments may use {file}, {dir}, {url}"));
    auto* completionMoveDirectory = new QLineEdit;
    completionMoveDirectory->setPlaceholderText(QStringLiteral("Optional directory"));
    auto* removeOnCompletion = new QCheckBox(QStringLiteral("Remove successful list entries"));
    auto* extractArchives = new QCheckBox(QStringLiteral("Extract supported archive files"));
    extractArchives->setChecked(settings.value(QStringLiteral("downloads/defaultExtractArchives"), false).toBool());
    auto* deleteArchives = new QCheckBox(QStringLiteral("Delete archives after successful extraction"));
    deleteArchives->setChecked(settings.value(QStringLiteral("downloads/defaultDeleteArchives"), false).toBool());
    deleteArchives->setEnabled(extractArchives->isChecked());
    connect(extractArchives, &QCheckBox::toggled, deleteArchives, &QWidget::setEnabled);
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
    form->addRow(QStringLiteral("Move after completion:"), completionMoveDirectory);
    form->addRow(QStringLiteral("Successful downloads:"), removeOnCompletion);
    form->addRow(QStringLiteral("Archives:"), extractArchives);
    form->addRow(QStringLiteral("After extraction:"), deleteArchives);
    form->addRow(QStringLiteral("Schedule window:"), restrictWindow);
    form->addRow(QStringLiteral("Allowed from:"), windowStart);
    form->addRow(QStringLiteral("Allowed until:"), windowEnd);
    form->addRow(QStringLiteral("Allowed days:"), weekdays);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted || directoryEdit->text().trimmed().isEmpty()) {
        return false;
    }
    if (restrictWindow->isChecked()
        && std::any_of(validUrls.cbegin(), validUrls.cend(), [](const QUrl& url) {
            return MediaDownloader::supports(url);
        })) {
        QMessageBox::warning(this, QStringLiteral("Batch Download"),
                             QStringLiteral("Schedule windows are not yet available for adaptive HLS/DASH jobs. Remove those URLs or disable the window."));
        return false;
    }
    if (std::any_of(validUrls.cbegin(), validUrls.cend(), [](const QUrl& url) {
            return MediaDownloader::supports(url);
        })
        && (!completionCommand->text().trimmed().isEmpty()
            || !completionMoveDirectory->text().trimmed().isEmpty()
            || removeOnCompletion->isChecked())) {
        QMessageBox::warning(this, QStringLiteral("Batch Download"),
            QStringLiteral("Post-download automation is not available for adaptive HLS/DASH jobs. Remove those URLs or clear the automation options."));
        return false;
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
            return false;
        }
    }

    QSet<QString> names;
    int unnamed = 1;
    for (qsizetype index = 0; index < validUrls.size(); ++index) {
        const auto& url = validUrls.at(index);
        QVariantMap requestHeaders = headers;
        QVariantMap browserDownload;
        if (index < browserDownloads.size()) {
            browserDownload = browserDownloads.at(index).toMap();
            requestHeaders = browserDownload.value(QStringLiteral("headers")).toMap();
            const auto method = browserDownload.value(QStringLiteral("method")).toString().trimmed().toUpper();
            if (method == QStringLiteral("POST")) {
                requestHeaders.insert(QStringLiteral("_qtidmHttpMethod"), method);
                requestHeaders.insert(QStringLiteral("_qtidmRequestBody"), browserDownload.value(QStringLiteral("body")).toString());
            }
            const auto suggested = safeSuggestedFilename(browserDownload.value(QStringLiteral("suggestedFilename")).toString());
            if (!suggested.isEmpty()) requestHeaders.insert(QStringLiteral("_qtidmSuggestedFilename"), suggested);
        }
        const auto mediaTypeHint = takeHeader(&requestHeaders, u"_qtidmMediaType").trimmed().toUpper();
        const bool adaptiveMedia = MediaDownloader::supports(url, mediaTypeHint);
        QString name = safeSuggestedFilename(takeHeader(&requestHeaders, u"_qtidmSuggestedFilename"));
        if (name.isEmpty()) name = url.fileName();
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
            mediaDownloader_.enqueue(url, targetPath, requestHeaders);
            continue;
        }
        DownloadRequest request;
        request.url = url;
        request.targetPath = targetPath;
        request.category = categoryEdit->text().trimmed().compare(QStringLiteral("Auto"), Qt::CaseInsensitive) == 0
            ? categoryForName(name)
            : categoryEdit->text().trimmed();
        request.queueName = queueEdit->text().trimmed();
        const auto httpMethod = takeHeader(&requestHeaders, u"_qtidmHttpMethod").trimmed().toUpper();
        const auto requestBody = QByteArray::fromBase64(takeHeader(&requestHeaders, u"_qtidmRequestBody").toLatin1());
        request.headers = std::move(requestHeaders);
        request.httpMethod = httpMethod == QStringLiteral("POST") ? httpMethod : QString {};
        request.requestBody = request.httpMethod == QStringLiteral("POST") ? requestBody : QByteArray {};
        if (request.httpMethod == QStringLiteral("POST")) {
            request.segments = 1;
            request.dynamicSegmentation = false;
        }
        else {
            request.segments = segments->value();
            request.dynamicSegmentation = dynamicSegments->isChecked();
        }
        request.perHostConnectionLimit = hostConnections->value();
        request.fileConflictPolicy = static_cast<FileConflictPolicy>(conflictPolicy->currentData().toInt());
        request.completionCommand = completionCommand->text().trimmed();
        request.completionMoveDirectory = completionMoveDirectory->text().trimmed();
        request.removeRecordOnCompletion = removeOnCompletion->isChecked();
        request.extractArchive = extractArchives->isChecked() && ArchiveExtractor::supports(targetPath);
        request.deleteArchiveAfterExtraction = request.extractArchive && deleteArchives->isChecked();
        applyRequestDefaults(request);
        if (restrictWindow->isChecked()) {
            request.windowStart = windowStart->time();
            request.windowEnd = windowEnd->time();
            request.allowedWeekdays = allowedWeekdays;
        }
        scheduler_.schedule(std::move(request));
    }
    statusBar()->showMessage(QStringLiteral("%1 batch downloads added").arg(validUrls.size()), 5000);
    return true;
}

void MainWindow::raiseAndActivate()
{
    showNormal();
    raise();
    activateWindow();
}

void MainWindow::onDownloadAdded(const DownloadRecord& record)
{
    transferMetrics_.insert(record.id, TransferMetric {
        .lastReceived = record.completedBytes,
        .lastSampleMs = qMax<qint64>(1, transferClock_.elapsed()),
        .lastDataMs = qMax<qint64>(1, transferClock_.elapsed())
    });
    if (isActiveStatus(record.status)) {
        activeTransfers_.insert(record.id);
    }
    const bool sortingEnabled = downloads_->isSortingEnabled();
    downloads_->setSortingEnabled(false);
    repository_.upsertDownload(record);
    int row = rowForId(record.id);
    if (row < 0) {
        row = downloads_->rowCount();
        downloads_->insertRow(row);
    }
    auto* idItem = new QTableWidgetItem(record.id);
    idItem->setData(Qt::UserRole, record.category);
    idItem->setData(Qt::UserRole + 1, statusText(record.status));
    downloads_->setItem(row, 0, idItem);
    downloads_->setItem(row, 1, new QTableWidgetItem(record.url.toString()));
    downloads_->setItem(row, 2, new QTableWidgetItem(record.targetPath));
    downloads_->setItem(row, 3, new QTableWidgetItem(statusText(record.status)));
    const int percent = record.totalBytes > 0 ? static_cast<int>((record.completedBytes * 100) / record.totalBytes) : 0;
    downloads_->setItem(row, 4, new QTableWidgetItem(QString::number(percent) + QLatin1Char('%')));
    downloads_->setItem(row, 5, new QTableWidgetItem(formatBytes(record.totalBytes)));
    downloads_->setItem(row, 6, new QTableWidgetItem(QStringLiteral("0 B/s")));
    downloads_->setItem(row, 7, new QTableWidgetItem(QStringLiteral("—")));
    auto* grid = new SegmentGrid(downloads_);
    grid->setSegments(record.segments);
    downloads_->setCellWidget(row, 8, grid);
    downloads_->setSortingEnabled(sortingEnabled);
    applyCategoryFilter();
    updateDownloadActionStates();
    statusBar()->showMessage(QStringLiteral("Download started"), 2000);
}

void MainWindow::onProgressChanged(const QString& id, qint64 received, qint64 total, double bytesPerSecond)
{
    Q_UNUSED(bytesPerSecond);
    const qint64 now = transferClock_.elapsed();
    auto& metric = transferMetrics_[id];
    if (metric.lastSampleMs <= 0) {
        metric.lastReceived = received;
        metric.lastSampleMs = now;
    } else {
        const qint64 elapsedMs = now - metric.lastSampleMs;
        const qint64 receivedDelta = qMax<qint64>(0, received - metric.lastReceived);
        if (elapsedMs > 0) {
            const double sample = receivedDelta * 1000.0 / elapsedMs;
            constexpr double smoothingWindowMs = 5000.0;
            const double alpha = 1.0 - std::exp(-static_cast<double>(elapsedMs) / smoothingWindowMs);
            if (!metric.hasSpeed && receivedDelta > 0) {
                metric.smoothedBytesPerSecond = sample;
                metric.hasSpeed = true;
            } else if (metric.hasSpeed) {
                metric.smoothedBytesPerSecond += alpha * (sample - metric.smoothedBytesPerSecond);
            }
            if (receivedDelta > 0) {
                metric.lastDataMs = now;
            } else if (metric.hasSpeed && now - metric.lastDataMs >= 5000) {
                metric.smoothedBytesPerSecond = 0.0;
                metric.hasSpeed = false;
            }
            sessionDownloadedBytes_ += receivedDelta;
        }
        metric.lastReceived = received;
        metric.lastSampleMs = now;
    }
    activeTransfers_.insert(id);

    repository_.updateProgress(id, received, total, total > 0 && received >= total ? DownloadStatus::Completed : DownloadStatus::Downloading);
    const int row = rowForId(id);
    if (row < 0) {
        updateSessionStatus();
        return;
    }
    const bool refreshVisibleMetrics = now - metric.lastUiUpdateMs >= 750
        || (total > 0 && received >= total);
    if (!refreshVisibleMetrics) {
        return;
    }
    metric.lastUiUpdateMs = now;
    const double displaySpeed = metric.hasSpeed ? metric.smoothedBytesPerSecond : 0.0;
    const bool sortingEnabled = downloads_->isSortingEnabled();
    downloads_->setSortingEnabled(false);
    const int percent = total > 0 ? static_cast<int>((received * 100) / total) : 0;
    downloads_->item(row, 4)->setText(QString::number(percent) + QLatin1Char('%'));
    downloads_->item(row, 5)->setText(formatBytes(total));
    downloads_->item(row, 6)->setText(formatBytes(static_cast<qint64>(displaySpeed)) + QStringLiteral("/s"));
    const auto eta = total > received && displaySpeed > 0
        ? static_cast<qint64>((total - received) / displaySpeed) : -1;
    downloads_->item(row, 7)->setText(formatDuration(eta));
    downloads_->setSortingEnabled(sortingEnabled);
    refreshDownloadProgressDetails(id, received, total, displaySpeed);
    updateSessionStatus();
}

void MainWindow::onSegmentsChanged(const QString& id, QVector<SegmentInfo> segments)
{
    const int row = rowForId(id);
    if (row < 0) {
        return;
    }
    auto* grid = qobject_cast<SegmentGrid*>(downloads_->cellWidget(row, 8));
    if (grid) {
        grid->setSegments(segments);
    }
    repository_.updateSegments(id, segments);
    refreshDownloadDetailsFor(id);
}

void MainWindow::onStatusChanged(const QString& id, DownloadStatus status, const QString& message)
{
    repository_.updateStatus(id, status);
    if (message.isEmpty()) {
        statusMessages_.remove(id);
    } else {
        statusMessages_.insert(id, message);
    }
    const int row = rowForId(id);
    QString targetName;
    if (row >= 0) {
        const bool sortingEnabled = downloads_->isSortingEnabled();
        downloads_->setSortingEnabled(false);
        targetName = QFileInfo(downloads_->item(row, 2)->text()).fileName();
        downloads_->item(row, 3)->setText(statusText(status));
        downloads_->item(row, 0)->setData(Qt::UserRole + 1, statusText(status));
        if (!isActiveStatus(status)) {
            downloads_->item(row, 6)->setText(QStringLiteral("0 B/s"));
            downloads_->item(row, 7)->setText(QStringLiteral("—"));
        }
        downloads_->setSortingEnabled(sortingEnabled);
        applyCategoryFilter();
        refreshDownloadDetailsFor(id);
        updateDownloadActionStates();
    }
    if (isActiveStatus(status)) {
        activeTransfers_.insert(id);
    } else {
        activeTransfers_.remove(id);
        if (auto metric = transferMetrics_.find(id); metric != transferMetrics_.end()) {
            metric->smoothedBytesPerSecond = 0.0;
            metric->hasSpeed = false;
        }
    }
    updateSessionStatus();
    if (!message.isEmpty()) {
        statusBar()->showMessage(message, 5000);
    }
    if (status == DownloadStatus::Failed) {
        Logger::error(QStringLiteral("Download %1").arg(id),
                      message.isEmpty() ? QStringLiteral("Download failed without a reported cause") : message);
    }
    if (tray_ && QSettings().value(QStringLiteral("notifications/enabled"), true).toBool()
        && (status == DownloadStatus::Completed || status == DownloadStatus::Failed)) {
        const auto name = targetName.isEmpty() ? id : targetName;
        tray_->showMessage(status == DownloadStatus::Completed
                ? QStringLiteral("Download complete") : QStringLiteral("Download failed"),
            message.isEmpty() ? name : QStringLiteral("%1\n%2").arg(name, message),
            status == DownloadStatus::Completed ? QSystemTrayIcon::Information : QSystemTrayIcon::Warning,
            5000);
    }
}

void MainWindow::updateDownloadActionStates()
{
    if (!startAction_ || !pauseAction_ || !stopAction_ || !deleteAction_ || !editAction_) {
        return;
    }

    const auto rows = selectedRows();
    const auto records = repository_.listDownloads();
    QHash<QString, DownloadStatus> statuses;
    statuses.reserve(records.size());
    for (const auto& record : records) {
        statuses.insert(record.id, record.status);
    }

    bool canStart = false;
    bool canPause = false;
    bool canStop = false;
    bool singleSelectionIsActive = false;
    int validSelections = 0;
    for (const int row : rows) {
        const auto* item = downloads_->item(row, 0);
        if (!item || !statuses.contains(item->text())) {
            continue;
        }
        const auto status = statuses.value(item->text());
        canStart = canStart || isResumableStatus(status);
        canPause = canPause || isActiveStatus(status);
        canStop = canStop || isStoppableStatus(status);
        singleSelectionIsActive = isActiveStatus(status);
        ++validSelections;
    }

    startAction_->setEnabled(canStart);
    pauseAction_->setEnabled(canPause);
    stopAction_->setEnabled(canStop);
    deleteAction_->setEnabled(!rows.isEmpty());
    editAction_->setEnabled(validSelections == 1 && !singleSelectionIsActive);

    startAction_->setToolTip(canStart
            ? QStringLiteral("Start or resume the selected stopped downloads")
            : QStringLiteral("Select a paused, failed, or canceled download"));
    pauseAction_->setToolTip(canPause
            ? QStringLiteral("Pause the selected active downloads")
            : QStringLiteral("Select a connecting or downloading entry"));
    stopAction_->setToolTip(canStop
            ? QStringLiteral("Stop the selected queued, active, or paused downloads")
            : QStringLiteral("Select a queued, active, or paused download"));
    deleteAction_->setToolTip(rows.isEmpty()
            ? QStringLiteral("Select one or more downloads")
            : QStringLiteral("Remove the selected downloads"));
    editAction_->setToolTip(validSelections == 1 && singleSelectionIsActive
            ? QStringLiteral("Pause the active download before editing transfer settings")
            : validSelections == 1
                ? QStringLiteral("Edit the selected download")
                : QStringLiteral("Select one download to edit"));
}

void MainWindow::refreshDownloadDetailsFor(const QString& id)
{
    const auto rows = selectedRows();
    if (rows.size() == 1 && downloads_->item(rows.first(), 0)->text() == id) {
        refreshDownloadDetails();
    }
}

void MainWindow::refreshDownloadProgressDetails(const QString& id, qint64 received,
                                                qint64 total, double bytesPerSecond)
{
    const auto rows = selectedRows();
    if (rows.size() != 1 || downloads_->item(rows.first(), 0)->text() != id) {
        return;
    }
    const int percent = total > 0 ? static_cast<int>((received * 100) / total) : 0;
    const auto eta = total > received && bytesPerSecond > 0
        ? static_cast<qint64>((total - received) / bytesPerSecond) : -1;
    const QHash<QString, QString> values {
        { QStringLiteral("Progress"),
          QStringLiteral("%1 of %2 (%3%)")
              .arg(formatBytes(received), formatBytes(total)).arg(percent) },
        { QStringLiteral("Download speed"),
          formatBytes(static_cast<qint64>(bytesPerSecond)) + QStringLiteral("/s") },
        { QStringLiteral("ETA"), formatDuration(eta) }
    };
    for (int index = 0; index < generalDetails_->topLevelItemCount(); ++index) {
        auto* item = generalDetails_->topLevelItem(index);
        if (values.contains(item->text(0))) {
            item->setText(1, values.value(item->text(0)));
        }
    }
}

void MainWindow::refreshDownloadDetails()
{
    if (!generalDetails_ || !requestDetails_ || !segmentDetails_ || !detailSegmentGrid_) {
        return;
    }

    generalDetails_->clear();
    requestDetails_->clear();
    segmentDetails_->setRowCount(0);
    detailSegmentGrid_->setSegments({});

    const auto addProperty = [](QTreeWidget* view, const QString& name, const QString& value) {
        auto* item = new QTreeWidgetItem({ name, value.isEmpty() ? QStringLiteral("—") : value });
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        view->addTopLevelItem(item);
    };
    const auto rows = selectedRows();
    if (rows.size() != 1) {
        addProperty(generalDetails_, QStringLiteral("Selection"),
                    rows.isEmpty() ? QStringLiteral("Select a download to inspect it")
                                   : QStringLiteral("Select one download to inspect its properties"));
        return;
    }

    const int row = rows.first();
    const auto id = downloads_->item(row, 0)->text();
    const auto records = repository_.listDownloads();
    const auto it = std::find_if(records.cbegin(), records.cend(), [&id](const DownloadRecord& record) {
        return record.id == id;
    });
    if (it == records.cend()) {
        addProperty(generalDetails_, QStringLiteral("Selection"),
                    QStringLiteral("This download no longer exists in the database"));
        return;
    }

    const auto& record = *it;
    const int percent = record.totalBytes > 0
        ? static_cast<int>((record.completedBytes * 100) / record.totalBytes) : 0;
    addProperty(generalDetails_, QStringLiteral("Name"), QFileInfo(record.targetPath).fileName());
    addProperty(generalDetails_, QStringLiteral("Status"), statusText(record.status));
    addProperty(generalDetails_, QStringLiteral("Progress"),
                QStringLiteral("%1 of %2 (%3%)")
                    .arg(formatBytes(record.completedBytes), formatBytes(record.totalBytes))
                    .arg(percent));
    addProperty(generalDetails_, QStringLiteral("Download speed"), downloads_->item(row, 6)->text());
    addProperty(generalDetails_, QStringLiteral("ETA"), downloads_->item(row, 7)->text());
    addProperty(generalDetails_, QStringLiteral("Source URL"),
                record.url.toString(QUrl::FullyEncoded));
    addProperty(generalDetails_, QStringLiteral("Destination"), record.targetPath);
    addProperty(generalDetails_, QStringLiteral("Category"), record.category);
    addProperty(generalDetails_, QStringLiteral("Added"),
                record.createdAt.toLocalTime().toString(Qt::ISODate));
    addProperty(generalDetails_, QStringLiteral("Updated"),
                record.updatedAt.toLocalTime().toString(Qt::ISODate));
    addProperty(generalDetails_, QStringLiteral("Download ID"), record.id);
    if (statusMessages_.contains(id)) {
        addProperty(generalDetails_, record.status == DownloadStatus::Failed
                ? QStringLiteral("Last error") : QStringLiteral("Last message"),
            statusMessages_.value(id));
    }

    const auto& request = record.request;
    addProperty(requestDetails_, QStringLiteral("HTTP method"),
                request.httpMethod.isEmpty() ? QStringLiteral("GET") : request.httpMethod);
    addProperty(requestDetails_, QStringLiteral("Queue"), request.queueName);
    addProperty(requestDetails_, QStringLiteral("Connections"),
                QStringLiteral("%1 segments, %2 per host")
                    .arg(request.segments).arg(request.perHostConnectionLimit));
    addProperty(requestDetails_, QStringLiteral("Dynamic segmentation"),
                request.dynamicSegmentation ? QStringLiteral("Enabled") : QStringLiteral("Disabled"));
    addProperty(requestDetails_, QStringLiteral("Retries"),
                QStringLiteral("%1, base delay %2 ms")
                    .arg(request.maxRetries).arg(request.retryBaseDelayMs));
    addProperty(requestDetails_, QStringLiteral("Timeouts"),
                QStringLiteral("%1 s connect, %2 s low speed")
                    .arg(request.connectTimeoutSeconds).arg(request.lowSpeedTimeoutSeconds));
    addProperty(requestDetails_, QStringLiteral("Redirect limit"),
                QString::number(request.maximumRedirects));
    addProperty(requestDetails_, QStringLiteral("Speed limit"),
                request.speedLimitBytesPerSecond > 0
                    ? formatBytes(request.speedLimitBytesPerSecond) + QStringLiteral("/s")
                    : QStringLiteral("Unlimited"));
    QString proxy = request.proxyUrl;
    if (!proxy.isEmpty()) {
        const QUrl proxyUrl(proxy);
        if (proxyUrl.isValid() && !proxyUrl.password().isEmpty()) {
            proxy = proxyUrl.toString(QUrl::RemovePassword);
        }
    }
    addProperty(requestDetails_, QStringLiteral("Proxy"),
                proxy.isEmpty() ? QStringLiteral("None") : proxy);
    addProperty(requestDetails_, QStringLiteral("Username"),
                request.username.isEmpty() ? QStringLiteral("None") : request.username);
    addProperty(requestDetails_, QStringLiteral("Credential"),
                !request.credentialVaultKey.isEmpty() ? QStringLiteral("Stored in credential vault")
                : !request.password.isEmpty() ? QStringLiteral("Set for this download")
                                              : QStringLiteral("None"));
    const auto checksum = request.expectedChecksum.isEmpty()
        ? request.expectedSha256 : request.expectedChecksum;
    addProperty(requestDetails_, QStringLiteral("Expected checksum"),
                checksum.isEmpty() ? QStringLiteral("None")
                                   : QStringLiteral("%1: %2")
                                         .arg(request.checksumAlgorithm.toUpper(), checksum));
    addProperty(requestDetails_, QStringLiteral("POST body"),
                request.requestBody.isEmpty()
                    ? QStringLiteral("None")
                    : QStringLiteral("%1 bytes").arg(request.requestBody.size()));
    for (auto header = request.headers.cbegin(); header != request.headers.cend(); ++header) {
        const auto lower = header.key().toLower();
        const bool sensitive = lower == QStringLiteral("authorization")
            || lower == QStringLiteral("cookie")
            || lower == QStringLiteral("proxy-authorization");
        addProperty(requestDetails_, QStringLiteral("Header: %1").arg(header.key()),
                    sensitive ? QStringLiteral("<redacted>") : header.value().toString());
    }

    detailSegmentGrid_->setSegments(record.segments);
    segmentDetails_->setRowCount(record.segments.size());
    for (int index = 0; index < record.segments.size(); ++index) {
        const auto& segment = record.segments.at(index);
        segmentDetails_->setItem(index, 0, new QTableWidgetItem(QString::number(segment.index + 1)));
        segmentDetails_->setItem(index, 1, new QTableWidgetItem(statusText(segment.status)));
        segmentDetails_->setItem(index, 2, new QTableWidgetItem(
            segment.end >= 0
                ? QStringLiteral("%1–%2").arg(segment.start).arg(segment.end)
                : QStringLiteral("%1–").arg(segment.start)));
        segmentDetails_->setItem(index, 3, new QTableWidgetItem(formatBytes(segment.written)));
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

void MainWindow::importLinks()
{
    const auto path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Import Links"), {}, QStringLiteral("Text files (*.txt *.list);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(this, QStringLiteral("Import Links"), file.errorString());
        return;
    }
    QStringList urls;
    QTextStream stream(&file);
    while (!stream.atEnd()) {
        const auto line = stream.readLine().trimmed();
        if (!line.isEmpty() && !line.startsWith(QLatin1Char('#'))) {
            urls.append(line);
        }
    }
    addUrls(urls);
}

void MainWindow::exportLinks()
{
    const auto path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export Links"), QStringLiteral("qtidm-links.txt"), QStringLiteral("Text files (*.txt)"));
    if (path.isEmpty()) {
        return;
    }
    auto rows = selectedRows();
    if (rows.isEmpty()) {
        for (int row = 0; row < downloads_->rowCount(); ++row) {
            rows.append(row);
        }
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, QStringLiteral("Export Links"), file.errorString());
        return;
    }
    QTextStream stream(&file);
    for (const int row : rows) {
        if (const auto* item = downloads_->item(row, 1)) {
            stream << item->text() << '\n';
        }
    }
    stream.flush();
    if (!file.commit()) {
        QMessageBox::warning(this, QStringLiteral("Export Links"), file.errorString());
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
    const int depth = QInputDialog::getInt(this, QStringLiteral("Site Grabber"),
        QStringLiteral("Same-origin recursion depth:"), 1, 0, 10, 1, &ok);
    if (!ok) {
        return;
    }

    SiteGrabberOptions options;
    options.depth = depth;
    options.renderJavaScript = QMessageBox::question(this, QStringLiteral("Site Grabber"),
        QStringLiteral("Render JavaScript before discovering links?\n\n"
                       "This uses an installed Chrome or Chromium browser and saves the rendered DOM for HTML pages."),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes) == QMessageBox::Yes;

    auto result = SiteGrabber().grab(QUrl(root), target, options);
    if (!result.error.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Site Grabber"), result.error);
        return;
    }

    int renderedCount = 0;
    for (const auto& document : result.renderedDocuments) {
        auto queueStaticFallback = [&result, &document] {
            DownloadRequest request;
            request.url = document.url;
            request.targetPath = document.targetPath;
            request.category = QStringLiteral("Site");
            request.segments = 4;
            result.requests.append(std::move(request));
        };
        if (!QDir().mkpath(QFileInfo(document.targetPath).absolutePath())) {
            result.warnings.append(QStringLiteral("Could not create the folder for %1.").arg(document.targetPath));
            queueStaticFallback();
            continue;
        }
        QSaveFile output(document.targetPath);
        if (!output.open(QIODevice::WriteOnly)
            || output.write(document.html) != document.html.size()
            || !output.commit()) {
            result.warnings.append(QStringLiteral("Could not save rendered page %1: %2")
                                       .arg(document.targetPath, output.errorString()));
            queueStaticFallback();
            continue;
        }
        ++renderedCount;
    }
    for (auto request : result.requests) {
        scheduler_.schedule(std::move(request));
    }

    statusBar()->showMessage(QStringLiteral("Site grab queued %1 resource(s) and saved %2 rendered page(s).")
                                 .arg(result.requests.size())
                                 .arg(renderedCount),
        6000);
    if (!result.warnings.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Site Grabber"),
            QStringLiteral("The grab continued with these warnings:\n\n%1").arg(result.warnings.join(QLatin1Char('\n'))));
    }
}

void MainWindow::extractSocialMedia()
{
    bool ok = false;
    const auto value = QInputDialog::getText(this, QStringLiteral("Social Media"),
        QStringLiteral("Post or media URL:"), QLineEdit::Normal, {}, &ok).trimmed();
    if (!ok || value.isEmpty()) {
        return;
    }
    const auto url = QUrl::fromUserInput(value);
    if (url.host().contains(QStringLiteral("youtube.com"), Qt::CaseInsensitive)
        || url.host().contains(QStringLiteral("youtu.be"), Qt::CaseInsensitive)) {
        QMessageBox::warning(this, QStringLiteral("Social Media"),
            QStringLiteral("YouTube extraction is intentionally unavailable, matching the reference app's policy."));
        return;
    }
    if (!SocialMediaExtractor::supports(url)) {
        QMessageBox::warning(this, QStringLiteral("Social Media"),
            QStringLiteral("Supported sites include Instagram, Facebook, X/Twitter, TikTok, Reddit, Vimeo, "
                           "Dailymotion, Twitch, and SoundCloud."));
        return;
    }
    if (!socialMediaExtractor_.isAvailable()) {
        QMessageBox::warning(this, QStringLiteral("Social Media"),
            QStringLiteral("Install yt-dlp to enable maintained site-specific extraction."));
        return;
    }

    const QStringList cookieChoices {
        QStringLiteral("No browser cookies"),
        QStringLiteral("Firefox"),
        QStringLiteral("Chrome"),
        QStringLiteral("Chromium")
    };
    const auto cookieChoice = QInputDialog::getItem(this, QStringLiteral("Social Media"),
        QStringLiteral("Use logged-in browser cookies for private posts:"), cookieChoices, 0, false, &ok);
    if (!ok) {
        return;
    }
    const auto browserCookies = cookieChoice == cookieChoices[0] ? QString {} : cookieChoice.toLower();
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString error;
    const auto extraction = socialMediaExtractor_.extract(url, browserCookies, &error);
    QApplication::restoreOverrideCursor();
    if (!error.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Social Media"), error);
        return;
    }

    QStringList labels;
    for (const auto& format : extraction.formats) {
        labels.append(QStringLiteral("%1 — %2").arg(format.id, format.label));
    }
    const auto selected = QInputDialog::getItem(this, QStringLiteral("Social Media"),
        QStringLiteral("%1\nChoose a format:").arg(extraction.title), labels, 0, false, &ok);
    if (!ok) {
        return;
    }
    const int formatIndex = labels.indexOf(selected);
    if (formatIndex < 0) {
        return;
    }
    const auto& format = extraction.formats[formatIndex];
    auto safeTitle = extraction.title;
    safeTitle.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._ -]")), QStringLiteral("_"));
    safeTitle = safeTitle.simplified();
    if (safeTitle.isEmpty()) {
        safeTitle = QStringLiteral("social-media");
    }
    const auto defaultPath = QDir(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation))
                                 .filePath(safeTitle + QLatin1Char('.')
                                     + (format.extension.isEmpty() ? QStringLiteral("mp4") : format.extension));
    const auto target = QFileDialog::getSaveFileName(this, QStringLiteral("Save Social Media"), defaultPath);
    if (target.isEmpty()) {
        return;
    }
    DownloadRequest request;
    request.url = format.url;
    request.targetPath = target;
    request.category = format.label.contains(QStringLiteral("audio only"))
        ? QStringLiteral("Music") : QStringLiteral("Video");
    request.headers = format.headers;
    applyRequestDefaults(request);
    if (format.adaptive || MediaDownloader::supports(format.url)) {
        mediaDownloader_.enqueue(request.url, request.targetPath, request.headers);
    } else {
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
    concurrency->setRange(0, 64);
    concurrency->setSpecialValueText(QStringLiteral("Unlimited"));
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
    auto* concurrentDownloads = new QSpinBox;
    concurrentDownloads->setRange(0, 64);
    concurrentDownloads->setSpecialValueText(QStringLiteral("Unlimited"));
    concurrentDownloads->setValue(scheduler_.queueConcurrency(QStringLiteral("Main")));
    auto* retryCount = new QSpinBox;
    retryCount->setRange(0, 999);
    retryCount->setSpecialValueText(QStringLiteral("No retries"));
    retryCount->setValue(settings.value(QStringLiteral("downloads/maxRetries"), 5).toInt());
    auto* retryDelay = new QSpinBox;
    retryDelay->setRange(1, 3600);
    retryDelay->setSuffix(QStringLiteral(" s"));
    retryDelay->setValue(settings.value(QStringLiteral("downloads/retryDelaySeconds"), 1).toInt());
    auto* connectTimeout = new QSpinBox;
    connectTimeout->setRange(1, 3600);
    connectTimeout->setSuffix(QStringLiteral(" s"));
    connectTimeout->setValue(settings.value(QStringLiteral("downloads/connectTimeoutSeconds"), 15).toInt());
    auto* lowSpeedTimeout = new QSpinBox;
    lowSpeedTimeout->setRange(1, 3600);
    lowSpeedTimeout->setSuffix(QStringLiteral(" s"));
    lowSpeedTimeout->setValue(settings.value(QStringLiteral("downloads/lowSpeedTimeoutSeconds"), 60).toInt());
    auto* maximumRedirects = new QSpinBox;
    maximumRedirects->setRange(0, 100);
    maximumRedirects->setSpecialValueText(QStringLiteral("Do not follow"));
    maximumRedirects->setValue(settings.value(QStringLiteral("downloads/maximumRedirects"), 20).toInt());
    auto* userAgent = new QLineEdit(settings.value(QStringLiteral("downloads/defaultUserAgent")).toString());
    userAgent->setPlaceholderText(QStringLiteral("Use libcurl default"));
    auto* defaultProxy = new QLineEdit(settings.value(QStringLiteral("downloads/defaultProxy")).toString());
    defaultProxy->setPlaceholderText(QStringLiteral("http://user:pass@host:port or socks5://host:port"));
    auto* sessionDataLimit = new QSpinBox;
    sessionDataLimit->setRange(0, 2 * 1024 * 1024);
    sessionDataLimit->setSuffix(QStringLiteral(" MiB"));
    sessionDataLimit->setSpecialValueText(QStringLiteral("Unlimited"));
    sessionDataLimit->setValue(settings.value(QStringLiteral("downloads/sessionDataLimitMiB"), 0).toInt());
    auto* sessionUsageRow = new QWidget;
    auto* sessionUsageLayout = new QHBoxLayout(sessionUsageRow);
    sessionUsageLayout->setContentsMargins(0, 0, 0, 0);
    auto* sessionUsage = new QLabel(formatBytes(downloader_.sessionBytesReceived()));
    auto* resetSessionUsage = new QPushButton(QStringLiteral("Reset"));
    sessionUsageLayout->addWidget(sessionUsage, 1);
    sessionUsageLayout->addWidget(resetSessionUsage);
    connect(resetSessionUsage, &QPushButton::clicked, &dialog, [this, sessionUsage] {
        downloader_.resetSessionBytesReceived();
        sessionDownloadedBytes_ = 0;
        sessionUsage->setText(formatBytes(0));
        updateSessionStatus();
    });
    auto* meteredPolicy = new QComboBox;
    meteredPolicy->addItem(QStringLiteral("Allow downloads"), static_cast<int>(MeteredNetworkPolicy::Allow));
    meteredPolicy->addItem(QStringLiteral("Hold new downloads"), static_cast<int>(MeteredNetworkPolicy::HoldNew));
    meteredPolicy->addItem(QStringLiteral("Pause active and hold new downloads"),
        static_cast<int>(MeteredNetworkPolicy::PauseActive));
    meteredPolicy->setCurrentIndex(qBound(0,
        settings.value(QStringLiteral("downloads/meteredPolicy"), 0).toInt(), 2));
    auto* defaultExtractArchives = new QCheckBox(QStringLiteral("Extract supported archives after download"));
    defaultExtractArchives->setChecked(
        settings.value(QStringLiteral("downloads/defaultExtractArchives"), false).toBool());
    auto* defaultDeleteArchives = new QCheckBox(QStringLiteral("Delete archives after successful extraction"));
    defaultDeleteArchives->setChecked(
        settings.value(QStringLiteral("downloads/defaultDeleteArchives"), false).toBool());
    defaultDeleteArchives->setEnabled(defaultExtractArchives->isChecked());
    connect(defaultExtractArchives, &QCheckBox::toggled, defaultDeleteArchives, &QWidget::setEnabled);
    auto* credentialVaultStatus = new QLabel(credentialVault_.isAvailable()
            ? QStringLiteral("Secret Service is available; passwords can be stored outside qtIDM's database.")
            : QStringLiteral("Unavailable — install secret-tool/libsecret for secure password storage."));
    credentialVaultStatus->setWordWrap(true);
    auto* clipboardMonitor = new QCheckBox(QStringLiteral("Clipboard monitor"));
    clipboardMonitor->setChecked(settings.value(QStringLiteral("integration/clipboardMonitor"), false).toBool());
    auto* notifications = new QCheckBox(QStringLiteral("Show completion and error notifications"));
    notifications->setChecked(settings.value(QStringLiteral("notifications/enabled"), true).toBool());
    form->addRow(QStringLiteral("Default segments:"), defaultSegments);
    form->addRow(QStringLiteral("Default per-host connections:"), defaultHostConnections);
    form->addRow(QStringLiteral("Default speed limit:"), defaultSpeedLimit);
    form->addRow(QStringLiteral("If file exists:"), defaultConflictPolicy);
    form->addRow(QStringLiteral("Concurrent downloads:"), concurrentDownloads);
    form->addRow(QStringLiteral("Retry count:"), retryCount);
    form->addRow(QStringLiteral("Initial retry delay:"), retryDelay);
    form->addRow(QStringLiteral("Connection timeout:"), connectTimeout);
    form->addRow(QStringLiteral("Low-speed timeout:"), lowSpeedTimeout);
    form->addRow(QStringLiteral("Maximum redirects:"), maximumRedirects);
    form->addRow(QStringLiteral("Default User-Agent:"), userAgent);
    form->addRow(QStringLiteral("Default proxy:"), defaultProxy);
    form->addRow(QStringLiteral("HTTP/FTP session data limit:"), sessionDataLimit);
    form->addRow(QStringLiteral("HTTP/FTP session data used:"), sessionUsageRow);
    form->addRow(QStringLiteral("Metered networks:"), meteredPolicy);
    form->addRow(QStringLiteral("Automatic extraction:"), defaultExtractArchives);
    form->addRow(QStringLiteral("After extraction:"), defaultDeleteArchives);
    form->addRow(QStringLiteral("Credential vault:"), credentialVaultStatus);
    form->addRow(QStringLiteral("Integration:"), clipboardMonitor);
    form->addRow(QStringLiteral("Notifications:"), notifications);
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
    settings.setValue(QStringLiteral("downloads/maxRetries"), retryCount->value());
    settings.setValue(QStringLiteral("downloads/retryDelaySeconds"), retryDelay->value());
    settings.setValue(QStringLiteral("downloads/connectTimeoutSeconds"), connectTimeout->value());
    settings.setValue(QStringLiteral("downloads/lowSpeedTimeoutSeconds"), lowSpeedTimeout->value());
    settings.setValue(QStringLiteral("downloads/maximumRedirects"), maximumRedirects->value());
    settings.setValue(QStringLiteral("downloads/defaultUserAgent"), userAgent->text().trimmed());
    settings.setValue(QStringLiteral("downloads/defaultProxy"), defaultProxy->text().trimmed());
    settings.setValue(QStringLiteral("downloads/sessionDataLimitMiB"), sessionDataLimit->value());
    settings.setValue(QStringLiteral("downloads/meteredPolicy"), meteredPolicy->currentIndex());
    settings.setValue(QStringLiteral("downloads/defaultExtractArchives"), defaultExtractArchives->isChecked());
    settings.setValue(QStringLiteral("downloads/defaultDeleteArchives"), defaultDeleteArchives->isChecked());
    settings.setValue(QStringLiteral("integration/clipboardMonitor"), clipboardMonitor->isChecked());
    settings.setValue(QStringLiteral("notifications/enabled"), notifications->isChecked());
    scheduler_.setQueueConcurrency(QStringLiteral("Main"), concurrentDownloads->value());
    scheduler_.setMeteredNetworkPolicy(static_cast<MeteredNetworkPolicy>(meteredPolicy->currentData().toInt()));
    configureClipboardMonitor();
}

void MainWindow::showAbout()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("About qtIDM"));
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(24, 24, 24, 18);
    layout->setSpacing(20);

    auto* content = new QHBoxLayout;
    content->setSpacing(28);
    auto* logo = new QLabel;
    logo->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    logo->setPixmap(QApplication::windowIcon().pixmap(112, 112));
    logo->setFixedSize(112, 112);
    logo->setAccessibleName(QStringLiteral("qtIDM logo"));
    content->addWidget(logo, 0, Qt::AlignTop);

    auto* text = new QVBoxLayout;
    text->setSpacing(7);
    auto* title = new QLabel(QStringLiteral("qtIDM %1").arg(
        QCoreApplication::applicationVersion()));
    auto titleFont = title->font();
    titleFont.setBold(true);
    titleFont.setPointSize(titleFont.pointSize() + 4);
    title->setFont(titleFont);
    text->addWidget(title);

    auto* description = new QLabel(
        QStringLiteral("A Linux-native Qt download manager."));
    description->setWordWrap(true);
    text->addWidget(description);

    auto* details = new QFormLayout;
    details->setContentsMargins(0, 5, 0, 0);
    details->setHorizontalSpacing(14);
    details->setVerticalSpacing(7);
    details->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    details->addRow(QStringLiteral("Version:"),
                    new QLabel(QCoreApplication::applicationVersion()));
    details->addRow(QStringLiteral("Build time:"),
                    new QLabel(QStringLiteral(QTIDM_BUILD_TIMESTAMP)));
    details->addRow(QStringLiteral("Qt runtime:"),
                    new QLabel(QString::fromLatin1(qVersion())));
    auto* projectLink = new QLabel(
        QStringLiteral("<a href=\"%1\">%1</a>").arg(QStringLiteral(QTIDM_PROJECT_URL)));
    projectLink->setTextFormat(Qt::RichText);
    projectLink->setTextInteractionFlags(Qt::TextBrowserInteraction);
    projectLink->setOpenExternalLinks(true);
    details->addRow(QStringLiteral("Project:"), projectLink);
    text->addLayout(details);
    text->addStretch();
    content->addLayout(text, 1);
    layout->addLayout(content);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.setMinimumWidth(600);
    dialog.exec();
}

void MainWindow::resumeSelected()
{
    const auto records = repository_.listDownloads();
    for (const int row : selectedRows()) {
        const auto* idItem = downloads_->item(row, 0);
        if (!idItem) continue;
        const auto it = std::find_if(records.cbegin(), records.cend(), [idItem](const DownloadRecord& record) {
            return record.id == idItem->text();
        });
        if (it == records.cend()) continue;
        const auto& record = *it;
        if (!isResumableStatus(record.status)) continue;
        if (mediaDownloader_.contains(record.id)) {
            mediaDownloader_.resume(record.id);
            continue;
        }
        if (MediaDownloader::supports(record.url)) {
            mediaDownloader_.enqueue(record.url, record.targetPath, record.request.headers, record.id);
            continue;
        }
        DownloadRequest request = record.request;
        request.existingId = record.id;
        request.url = record.url;
        request.targetPath = record.targetPath;
        request.category = record.category;
        request.expectedTotalBytes = record.totalBytes;
        request.resumeSegments = record.segments;
        request.segments = record.segments.isEmpty() ? 8 : record.segments.size();
        applyRequestDefaults(request);
        scheduler_.schedule(std::move(request));
    }
}

void MainWindow::pauseSelected()
{
    const auto records = repository_.listDownloads();
    for (const int row : selectedRows()) {
        const auto id = downloads_->item(row, 0)->text();
        const auto it = std::find_if(records.cbegin(), records.cend(), [&id](const DownloadRecord& record) {
            return record.id == id;
        });
        if (it == records.cend() || !isActiveStatus(it->status)) continue;
        if (mediaDownloader_.contains(id)) {
            mediaDownloader_.pause(id);
        } else {
            downloader_.pause(id);
        }
    }
}

void MainWindow::cancelSelected()
{
    const auto records = repository_.listDownloads();
    for (const int row : selectedRows()) {
        const auto id = downloads_->item(row, 0)->text();
        const auto it = std::find_if(records.cbegin(), records.cend(), [&id](const DownloadRecord& record) {
            return record.id == id;
        });
        if (it == records.cend() || !isStoppableStatus(it->status)) continue;
        if (mediaDownloader_.contains(id)) {
            mediaDownloader_.cancel(id);
        } else {
            downloader_.cancel(id);
        }
    }
}

void MainWindow::deleteSelected()
{
    auto rows = selectedRows();
    if (rows.isEmpty()) {
        return;
    }
    QMessageBox prompt(QMessageBox::Question, QStringLiteral("Delete Downloads"),
        QStringLiteral("Remove %1 selected download(s) from the list?").arg(rows.size()),
        QMessageBox::Yes | QMessageBox::Cancel, this);
    auto* deleteFiles = new QCheckBox(QStringLiteral("Also delete downloaded and partial files"));
    prompt.setCheckBox(deleteFiles);
    if (prompt.exec() != QMessageBox::Yes) {
        return;
    }
    std::sort(rows.begin(), rows.end(), std::greater<int>());
    for (const int row : rows) {
        const auto id = downloads_->item(row, 0)->text();
        const auto path = downloads_->item(row, 2)->text();
        if (mediaDownloader_.contains(id)) {
            mediaDownloader_.cancel(id);
        } else {
            downloader_.cancel(id);
        }
        repository_.removeDownload(id);
        if (deleteFiles->isChecked()) {
            QFile::remove(path);
            QFile::remove(path + QStringLiteral(".part"));
        }
        downloads_->removeRow(row);
    }
    updateDownloadActionStates();
}

void MainWindow::openSelectedFile()
{
    const auto rows = selectedRows();
    if (!rows.isEmpty()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(downloads_->item(rows.first(), 2)->text()));
    }
}

void MainWindow::openSelectedFolder()
{
    const auto rows = selectedRows();
    if (!rows.isEmpty()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(downloads_->item(rows.first(), 2)->text()).absolutePath()));
    }
}

void MainWindow::copySelectedUrl()
{
    const auto rows = selectedRows();
    if (!rows.isEmpty()) {
        QApplication::clipboard()->setText(downloads_->item(rows.first(), 1)->text());
    }
}

void MainWindow::copySelectedPath()
{
    const auto rows = selectedRows();
    if (!rows.isEmpty()) {
        QApplication::clipboard()->setText(downloads_->item(rows.first(), 2)->text());
    }
}

void MainWindow::calculateSelectedChecksum()
{
    const auto rows = selectedRows();
    if (rows.isEmpty()) {
        return;
    }
    const auto path = downloads_->item(rows.first(), 2)->text();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, QStringLiteral("Calculate Checksum"),
            QStringLiteral("Could not open %1: %2").arg(path, file.errorString()));
        return;
    }
    bool ok = false;
    const auto name = QInputDialog::getItem(this, QStringLiteral("Calculate Checksum"),
        QStringLiteral("Algorithm:"), { QStringLiteral("MD5"), QStringLiteral("SHA-1"),
            QStringLiteral("SHA-256"), QStringLiteral("SHA-512") }, 2, false, &ok);
    if (!ok) {
        return;
    }
    QCryptographicHash hash(checksumAlgorithm(name));
    if (!hash.addData(&file)) {
        QMessageBox::warning(this, QStringLiteral("Calculate Checksum"), QStringLiteral("Could not read the selected file."));
        return;
    }
    const auto result = QString::fromLatin1(hash.result().toHex());
    QApplication::clipboard()->setText(result);
    QMessageBox::information(this, QStringLiteral("Calculate Checksum"),
        QStringLiteral("%1\n\n%2\n\nThe checksum was copied to the clipboard.").arg(name, result));
}

void MainWindow::clearFinished()
{
    QList<int> rows;
    for (int row = 0; row < downloads_->rowCount(); ++row) {
        const auto status = downloads_->item(row, 0)->data(Qt::UserRole + 1).toString();
        if (status == QStringLiteral("Complete") || status == QStringLiteral("Canceled")) {
            rows.append(row);
        }
    }
    if (rows.isEmpty() || QMessageBox::question(this, QStringLiteral("Clear Finished"),
            QStringLiteral("Remove %1 completed/canceled item(s) from the list? Files will be kept.").arg(rows.size()))
            != QMessageBox::Yes) {
        return;
    }
    std::sort(rows.begin(), rows.end(), std::greater<int>());
    for (const int row : rows) {
        repository_.removeDownload(downloads_->item(row, 0)->text());
        downloads_->removeRow(row);
    }
    updateDownloadActionStates();
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
        QMessageBox::warning(this, QStringLiteral("Edit Download Properties"), QStringLiteral("The selected download no longer exists in the database."));
        return;
    }
    if (it->status == DownloadStatus::Downloading || it->status == DownloadStatus::Connecting) {
        detailsTabs_->setCurrentWidget(generalDetails_);
        generalDetails_->setFocus();
        statusBar()->showMessage(
            QStringLiteral("Properties are shown below. Pause the download before editing transfer settings."),
            6000);
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Edit Download Properties"));
    auto* form = new QFormLayout(&dialog);
    auto* urlEdit = new QLineEdit(it->url.toString());
    auto* categoryEdit = new QLineEdit(it->category);
    auto* destination = new QLineEdit(it->targetPath);
    destination->setReadOnly(true);
    auto* proxyEdit = new QLineEdit(it->request.proxyUrl);
    auto* userEdit = new QLineEdit(it->request.username);
    auto* passwordEdit = new QLineEdit(it->request.password);
    passwordEdit->setEchoMode(QLineEdit::Password);
    auto* storeCredential = new QCheckBox(QStringLiteral("Store through the system credential vault"));
    storeCredential->setEnabled(credentialVault_.isAvailable());
    storeCredential->setChecked(!it->request.credentialVaultKey.isEmpty());
    if (!it->request.credentialVaultKey.isEmpty() && credentialVault_.isAvailable()) {
        QString ignoredError;
        passwordEdit->setText(credentialVault_.lookup(it->request.credentialVaultKey, &ignoredError));
    }
    auto* speedLimit = new QSpinBox;
    speedLimit->setRange(0, 1024 * 1024);
    speedLimit->setSuffix(QStringLiteral(" KB/s"));
    speedLimit->setValue(static_cast<int>(it->request.speedLimitBytesPerSecond / 1024));
    auto* checksumType = new QComboBox;
    checksumType->addItems({ QStringLiteral("MD5"), QStringLiteral("SHA-1"), QStringLiteral("SHA-256"), QStringLiteral("SHA-512") });
    const auto storedAlgorithm = it->request.checksumAlgorithm.toLower();
    checksumType->setCurrentText(storedAlgorithm == QStringLiteral("md5") ? QStringLiteral("MD5")
        : storedAlgorithm == QStringLiteral("sha1") ? QStringLiteral("SHA-1")
        : storedAlgorithm == QStringLiteral("sha512") ? QStringLiteral("SHA-512")
        : QStringLiteral("SHA-256"));
    auto* checksumEdit = new QLineEdit(it->request.expectedChecksum.isEmpty()
        ? it->request.expectedSha256 : it->request.expectedChecksum);
    auto* checksumRow = new QWidget;
    auto* checksumLayout = new QHBoxLayout(checksumRow);
    checksumLayout->setContentsMargins(0, 0, 0, 0);
    checksumLayout->addWidget(checksumType);
    checksumLayout->addWidget(checksumEdit, 1);
    auto* headersEdit = new QPlainTextEdit;
    headersEdit->setMaximumHeight(90);
    QStringList headerLines;
    for (auto header = it->request.headers.cbegin(); header != it->request.headers.cend(); ++header) {
        headerLines.append(header.key() + QStringLiteral(": ") + header.value().toString());
    }
    headersEdit->setPlainText(headerLines.join(QLatin1Char('\n')));
    auto* completionCommand = new QLineEdit(it->request.completionCommand);
    completionCommand->setPlaceholderText(QStringLiteral("Arguments may use {file}, {dir}, {url}"));
    auto* completionMoveDirectory = new QLineEdit(it->request.completionMoveDirectory);
    auto* extractArchive = new QCheckBox(QStringLiteral("Extract after successful download"));
    extractArchive->setChecked(it->request.extractArchive);
    auto* archiveDestination = new QLineEdit(it->request.archiveDestination);
    archiveDestination->setPlaceholderText(QStringLiteral("Automatic folder beside archive"));
    auto* deleteArchive = new QCheckBox(QStringLiteral("Delete archive after successful extraction"));
    deleteArchive->setChecked(it->request.deleteArchiveAfterExtraction);
    archiveDestination->setEnabled(extractArchive->isChecked());
    deleteArchive->setEnabled(extractArchive->isChecked());
    connect(extractArchive, &QCheckBox::toggled, archiveDestination, &QWidget::setEnabled);
    connect(extractArchive, &QCheckBox::toggled, deleteArchive, &QWidget::setEnabled);
    auto* removeOnCompletion = new QCheckBox(QStringLiteral("Remove the list entry after success"));
    removeOnCompletion->setChecked(it->request.removeRecordOnCompletion);
    auto* progress = new QLabel(QStringLiteral("%1 / %2 bytes preserved")
                                    .arg(it->completedBytes)
                                    .arg(it->totalBytes));
    progress->setWordWrap(true);
    form->addRow(QStringLiteral("Source URL:"), urlEdit);
    form->addRow(QStringLiteral("Destination:"), destination);
    form->addRow(QStringLiteral("Category:"), categoryEdit);
    form->addRow(QStringLiteral("Proxy:"), proxyEdit);
    form->addRow(QStringLiteral("User:"), userEdit);
    form->addRow(QStringLiteral("Password:"), passwordEdit);
    form->addRow(QStringLiteral("Credential storage:"), storeCredential);
    form->addRow(QStringLiteral("Speed limit:"), speedLimit);
    form->addRow(QStringLiteral("Checksum:"), checksumRow);
    form->addRow(QStringLiteral("HTTP headers:"), headersEdit);
    form->addRow(QStringLiteral("After completion:"), completionCommand);
    form->addRow(QStringLiteral("Move after completion:"), completionMoveDirectory);
    form->addRow(QStringLiteral("Archives:"), extractArchive);
    form->addRow(QStringLiteral("Extract to:"), archiveDestination);
    form->addRow(QStringLiteral("After extraction:"), deleteArchive);
    form->addRow(QStringLiteral("Successful download:"), removeOnCompletion);
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
        || (replacement.scheme() != QStringLiteral("http")
            && replacement.scheme() != QStringLiteral("https")
            && replacement.scheme() != QStringLiteral("ftp"))) {
        QMessageBox::warning(this, QStringLiteral("Edit Download Properties"), QStringLiteral("The replacement must be a valid HTTP, HTTPS, or FTP URL."));
        return;
    }
    auto updated = *it;
    updated.url = replacement;
    updated.category = categoryEdit->text().trimmed();
    updated.request.url = replacement;
    updated.request.category = updated.category;
    updated.request.targetPath = updated.targetPath;
    updated.request.proxyUrl = proxyEdit->text().trimmed();
    updated.request.username = userEdit->text();
    updated.request.password = passwordEdit->text();
    updated.request.credentialVaultKey = storeCredential->isChecked()
        ? it->request.credentialVaultKey : QString {};
    if (storeCredential->isChecked() && !updated.request.password.isEmpty()) {
        updated.request.credentialVaultKey = credentialVault_.keyFor(replacement, updated.request.username);
        QString vaultError;
        if (!credentialVault_.store(updated.request.credentialVaultKey, replacement,
                updated.request.username, updated.request.password, &vaultError)) {
            QMessageBox::warning(this, QStringLiteral("Credential Vault"), vaultError);
            return;
        }
        updated.request.password.clear();
    }
    updated.request.speedLimitBytesPerSecond = static_cast<qint64>(speedLimit->value()) * 1024;
    updated.request.checksumAlgorithm = checksumType->currentText().toLower().remove(QLatin1Char('-'));
    updated.request.expectedChecksum = checksumEdit->text().simplified().remove(QLatin1Char(' ')).toLower();
    updated.request.expectedSha256.clear();
    updated.request.completionCommand = completionCommand->text().trimmed();
    updated.request.completionMoveDirectory = completionMoveDirectory->text().trimmed();
    updated.request.extractArchive = extractArchive->isChecked();
    updated.request.archiveDestination = archiveDestination->text().trimmed();
    updated.request.deleteArchiveAfterExtraction = deleteArchive->isChecked();
    if (updated.request.extractArchive && !ArchiveExtractor::supports(updated.targetPath)) {
        QMessageBox::warning(this, QStringLiteral("Edit Download Properties"),
            QStringLiteral("The destination filename is not a supported archive type."));
        return;
    }
    updated.request.removeRecordOnCompletion = removeOnCompletion->isChecked();
    updated.request.headers.clear();
    QString headerError;
    if (!mergeHeaderLines(headersEdit->toPlainText(), &updated.request.headers, &headerError)) {
        QMessageBox::warning(this, QStringLiteral("Invalid Headers"), headerError);
        return;
    }
    updated.updatedAt = QDateTime::currentDateTimeUtc();
    if (!repository_.upsertDownload(updated)) {
        QMessageBox::warning(this, QStringLiteral("Edit Download Properties"), repository_.lastError());
        return;
    }
    downloads_->item(row, 1)->setText(replacement.toString());
    downloads_->item(row, 0)->setData(Qt::UserRole, updated.category);
    applyCategoryFilter();
    refreshDownloadDetails();
    statusBar()->showMessage(QStringLiteral("Download properties updated; use Start to resume"), 5000);
}

void MainWindow::buildActions()
{
    auto* toolbar = addToolBar(QStringLiteral("Main"));
    toolbar->setMovable(false);

    auto addAction = toolbar->addAction(staticActionIcon(QStringLiteral("add")), QStringLiteral("Add"));
    addAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_N));
    connect(addAction, &QAction::triggered, this, [this] { addUrl(); });

    startAction_ = new QAction(staticActionIcon(QStringLiteral("start")),
                               QStringLiteral("Start/Resume"), this);
    pauseAction_ = new QAction(staticActionIcon(QStringLiteral("pause")),
                               QStringLiteral("Pause"), this);
    stopAction_ = new QAction(staticActionIcon(QStringLiteral("stop")),
                              QStringLiteral("Stop"), this);
    deleteAction_ = new QAction(staticActionIcon(QStringLiteral("delete")),
                                QStringLiteral("Delete…"), this);
    editAction_ = new QAction(staticActionIcon(QStringLiteral("edit")),
                              QStringLiteral("Edit properties…"), this);
    deleteAction_->setShortcut(QKeySequence::Delete);
    connect(startAction_, &QAction::triggered, this, &MainWindow::resumeSelected);
    connect(pauseAction_, &QAction::triggered, this, &MainWindow::pauseSelected);
    connect(stopAction_, &QAction::triggered, this, &MainWindow::cancelSelected);
    connect(deleteAction_, &QAction::triggered, this, &MainWindow::deleteSelected);
    connect(editAction_, &QAction::triggered, this, &MainWindow::propertiesSelected);
    toolbar->addActions({ startAction_, pauseAction_, stopAction_, deleteAction_, editAction_ });
    toolbar->addSeparator();
    toolbar->addAction(staticActionIcon(QStringLiteral("queue")),
                       QStringLiteral("Queue"), this, &MainWindow::showQueue);
    toolbar->addAction(staticActionIcon(QStringLiteral("import")),
                       QStringLiteral("Import"), this, &MainWindow::importHistory);
    toolbar->addAction(staticActionIcon(QStringLiteral("export")),
                       QStringLiteral("Export"), this, &MainWindow::exportHistory);
    toolbar->addAction(staticActionIcon(QStringLiteral("links")),
                       QStringLiteral("Links"), this, &MainWindow::importLinks);
    toolbar->addAction(staticActionIcon(QStringLiteral("grabber")),
                       QStringLiteral("Grabber"), this, &MainWindow::grabSite);
    toolbar->addAction(staticActionIcon(QStringLiteral("social")),
                       QStringLiteral("Social"), this, &MainWindow::extractSocialMedia);
    toolbar->addAction(staticActionIcon(QStringLiteral("zip")),
                       QStringLiteral("ZIP"), this, &MainWindow::previewZip);
    toolbar->addSeparator();
    toolbar->addAction(staticActionIcon(QStringLiteral("options")),
                       QStringLiteral("Options"), this, &MainWindow::showOptions);
    toolbar->addAction(staticActionIcon(QStringLiteral("about")),
                       QStringLiteral("About"), this, &MainWindow::showAbout);

    for (auto* action : toolbar->actions()) {
        auto* button = qobject_cast<QToolButton*>(toolbar->widgetForAction(action));
        if (!button) {
            continue;
        }
        const auto syncCursor = [button, action] {
            button->setCursor(action->isEnabled() ? Qt::PointingHandCursor : Qt::ForbiddenCursor);
        };
        syncCursor();
        connect(action, &QAction::changed, button, syncCursor);
    }

    auto* fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
    fileMenu->addAction(QStringLiteral("&Add URL…"), QKeySequence(Qt::CTRL | Qt::Key_N), this, [this] { addUrl(); });
    fileMenu->addAction(QStringLiteral("Import &link list…"), this, &MainWindow::importLinks);
    fileMenu->addAction(QStringLiteral("Export selected/all l&inks…"), this, &MainWindow::exportLinks);
    fileMenu->addSeparator();
    fileMenu->addAction(QStringLiteral("Import history…"), this, &MainWindow::importHistory);
    fileMenu->addAction(QStringLiteral("Export history…"), this, &MainWindow::exportHistory);
    fileMenu->addSeparator();
    fileMenu->addAction(QStringLiteral("&Quit"), QKeySequence::Quit, qApp, &QApplication::quit);

    auto* downloadMenu = menuBar()->addMenu(QStringLiteral("&Downloads"));
    downloadMenu->addActions({ startAction_, pauseAction_, stopAction_, deleteAction_, editAction_ });
    downloadMenu->addSeparator();
    downloadMenu->addAction(QStringLiteral("Clear finished…"), this, &MainWindow::clearFinished);
}

void MainWindow::buildLayout()
{
    auto* central = new QWidget(this);
    auto* centralLayout = new QVBoxLayout(central);
    centralLayout->setContentsMargins(6, 6, 6, 6);
    auto* filterLayout = new QHBoxLayout;
    search_ = new QLineEdit;
    search_->setClearButtonEnabled(true);
    search_->setPlaceholderText(QStringLiteral("Search URL, destination, or category"));
    statusFilter_ = new QComboBox;
    statusFilter_->addItems({ QStringLiteral("All statuses"), QStringLiteral("Queued"),
        QStringLiteral("Connecting"), QStringLiteral("Downloading"), QStringLiteral("Paused"),
        QStringLiteral("Complete"), QStringLiteral("Error"), QStringLiteral("Canceled") });
    filterLayout->addWidget(new QLabel(QStringLiteral("Filter:")));
    filterLayout->addWidget(search_, 1);
    filterLayout->addWidget(statusFilter_);
    centralLayout->addLayout(filterLayout);

    navigationSplitter_ = new QSplitter(Qt::Horizontal, central);
    navigationSplitter_->setHandleWidth(3);
    navigationSplitter_->setOpaqueResize(true);
    categories_ = new QTreeWidget;
    categories_->setHeaderHidden(true);
    categories_->addTopLevelItem(new QTreeWidgetItem(QStringList { QStringLiteral("All Downloads") }));
    categories_->addTopLevelItem(new QTreeWidgetItem(QStringList { QStringLiteral("General") }));
    categories_->addTopLevelItem(new QTreeWidgetItem(QStringList { QStringLiteral("Compressed") }));
    categories_->addTopLevelItem(new QTreeWidgetItem(QStringList { QStringLiteral("Documents") }));
    categories_->addTopLevelItem(new QTreeWidgetItem(QStringList { QStringLiteral("Images") }));
    categories_->addTopLevelItem(new QTreeWidgetItem(QStringList { QStringLiteral("Music") }));
    categories_->addTopLevelItem(new QTreeWidgetItem(QStringList { QStringLiteral("Programs") }));
    categories_->addTopLevelItem(new QTreeWidgetItem(QStringList { QStringLiteral("Video") }));
    categories_->setMinimumWidth(140);
    categories_->setCurrentItem(categories_->topLevelItem(0));
    connect(categories_, &QTreeWidget::currentItemChanged, this, [this] { applyCategoryFilter(); });
    connect(search_, &QLineEdit::textChanged, this, [this] { applyCategoryFilter(); });
    connect(statusFilter_, &QComboBox::currentTextChanged, this, [this] { applyCategoryFilter(); });

    downloadSplitter_ = new QSplitter(Qt::Vertical);
    downloadSplitter_->setHandleWidth(3);
    downloadSplitter_->setOpaqueResize(true);
    downloads_ = new QTableWidget;
    downloads_->setColumnCount(9);
    downloads_->setHorizontalHeaderLabels({ QStringLiteral("ID"), QStringLiteral("Source URL"), QStringLiteral("Save To"),
        QStringLiteral("Status"), QStringLiteral("Progress"), QStringLiteral("Size"),
        QStringLiteral("Download Speed"), QStringLiteral("ETA"), QStringLiteral("Segments") });
    downloads_->setColumnHidden(0, true);
    downloads_->setSelectionBehavior(QAbstractItemView::SelectRows);
    downloads_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    downloads_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    downloads_->setAlternatingRowColors(true);
    downloads_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(downloads_, &QTableWidget::customContextMenuRequested, this, [this](const QPoint& point) {
        if (const auto* item = downloads_->itemAt(point); item && !item->isSelected()) {
            downloads_->clearSelection();
            downloads_->selectRow(item->row());
        }
        updateDownloadActionStates();
        QMenu menu(this);
        menu.addActions({ startAction_, pauseAction_, stopAction_ });
        menu.addSeparator();
        menu.addAction(QStringLiteral("Open file"), this, &MainWindow::openSelectedFile);
        menu.addAction(QStringLiteral("Open containing folder"), this, &MainWindow::openSelectedFolder);
        menu.addAction(QStringLiteral("Copy URL"), this, &MainWindow::copySelectedUrl);
        menu.addAction(QStringLiteral("Copy path"), this, &MainWindow::copySelectedPath);
        menu.addAction(QStringLiteral("Calculate checksum…"), this, &MainWindow::calculateSelectedChecksum);
        menu.addSeparator();
        menu.addActions({ editAction_, deleteAction_ });
        menu.exec(downloads_->viewport()->mapToGlobal(point));
    });
    connect(downloads_, &QTableWidget::itemDoubleClicked, this, [this] { openSelectedFile(); });
    connect(downloads_, &QTableWidget::itemSelectionChanged, this, [this] {
        refreshDownloadDetails();
        updateDownloadActionStates();
    });
    downloads_->horizontalHeader()->setStretchLastSection(true);
    downloads_->horizontalHeader()->resizeSection(1, 260);
    downloads_->horizontalHeader()->resizeSection(2, 320);
    downloads_->horizontalHeader()->resizeSection(8, 180);

    detailsTabs_ = new QTabWidget;
    detailsTabs_->setDocumentMode(true);
    detailsTabs_->setMinimumHeight(170);
    const auto makeDetailsTree = [] {
        auto* view = new QTreeWidget;
        view->setColumnCount(2);
        view->setHeaderLabels({ QStringLiteral("Property"), QStringLiteral("Value") });
        view->setRootIsDecorated(false);
        view->setAlternatingRowColors(true);
        view->setEditTriggers(QAbstractItemView::NoEditTriggers);
        view->setSelectionMode(QAbstractItemView::SingleSelection);
        view->header()->resizeSection(0, 175);
        view->header()->setStretchLastSection(true);
        return view;
    };
    generalDetails_ = makeDetailsTree();
    requestDetails_ = makeDetailsTree();
    detailsTabs_->addTab(generalDetails_, QStringLiteral("General"));
    detailsTabs_->addTab(requestDetails_, QStringLiteral("Request"));

    auto* segmentsPage = new QWidget;
    auto* segmentsLayout = new QVBoxLayout(segmentsPage);
    segmentsLayout->setContentsMargins(6, 6, 6, 6);
    detailSegmentGrid_ = new SegmentGrid;
    detailSegmentGrid_->setMinimumHeight(34);
    detailSegmentGrid_->setMaximumHeight(42);
    segmentDetails_ = new QTableWidget;
    segmentDetails_->setColumnCount(4);
    segmentDetails_->setHorizontalHeaderLabels({
        QStringLiteral("#"), QStringLiteral("Status"),
        QStringLiteral("Byte range"), QStringLiteral("Downloaded")
    });
    segmentDetails_->setSelectionBehavior(QAbstractItemView::SelectRows);
    segmentDetails_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    segmentDetails_->setAlternatingRowColors(true);
    segmentDetails_->horizontalHeader()->setStretchLastSection(true);
    segmentDetails_->horizontalHeader()->resizeSection(0, 55);
    segmentDetails_->horizontalHeader()->resizeSection(1, 120);
    segmentDetails_->horizontalHeader()->resizeSection(2, 220);
    segmentsLayout->addWidget(detailSegmentGrid_);
    segmentsLayout->addWidget(segmentDetails_, 1);
    detailsTabs_->addTab(segmentsPage, QStringLiteral("Segments"));

    downloadSplitter_->addWidget(downloads_);
    downloadSplitter_->addWidget(detailsTabs_);
    downloadSplitter_->setStretchFactor(0, 3);
    downloadSplitter_->setStretchFactor(1, 2);
    navigationSplitter_->addWidget(categories_);
    navigationSplitter_->addWidget(downloadSplitter_);
    navigationSplitter_->setStretchFactor(1, 1);
    centralLayout->addWidget(navigationSplitter_, 1);

    QSettings settings;
    if (!navigationSplitter_->restoreState(
            settings.value(QStringLiteral("ui/navigationSplitter")).toByteArray())) {
        navigationSplitter_->setSizes({ 180, 900 });
    }
    if (!downloadSplitter_->restoreState(
            settings.value(QStringLiteral("ui/downloadDetailsSplitter")).toByteArray())) {
        downloadSplitter_->setSizes({ 420, 240 });
    }
    connect(navigationSplitter_, &QSplitter::splitterMoved, this, [this] {
        QSettings().setValue(QStringLiteral("ui/navigationSplitter"),
                             navigationSplitter_->saveState());
    });
    connect(downloadSplitter_, &QSplitter::splitterMoved, this, [this] {
        QSettings().setValue(QStringLiteral("ui/downloadDetailsSplitter"),
                             downloadSplitter_->saveState());
    });
    refreshDownloadDetails();
    updateDownloadActionStates();
    setCentralWidget(central);

    sessionRateLabel_ = new QLabel;
    sessionRateLabel_->setToolTip(QStringLiteral("Combined smoothed speed of active downloads"));
    sessionSizeLabel_ = new QLabel;
    sessionSizeLabel_->setToolTip(QStringLiteral("Data downloaded since qtIDM was started"));
    alternateLimitButton_ = new QToolButton;
    alternateLimitButton_->setCursor(Qt::PointingHandCursor);
    alternateLimitButton_->setPopupMode(QToolButton::MenuButtonPopup);
    alternateLimitButton_->setIcon(staticActionIcon(QStringLiteral("speed-limit")));
    alternateLimitButton_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    auto* limitMenu = new QMenu(alternateLimitButton_);
    for (const int kib : { 256, 512, 1024, 2048, 5120, 10240 }) {
        const auto text = formatBytes(static_cast<qint64>(kib) * 1024) + QStringLiteral("/s");
        limitMenu->addAction(text, this, [this, kib] {
            setAlternateSpeedLimit(true, static_cast<qint64>(kib) * 1024);
        });
    }
    limitMenu->addSeparator();
    limitMenu->addAction(QStringLiteral("Custom…"), this, [this] {
        QSettings settings;
        bool ok = false;
        const int current = qMax(1, settings.value(
            QStringLiteral("downloads/alternateSpeedLimitKib"), 1024).toInt());
        const int kib = QInputDialog::getInt(
            this, QStringLiteral("Alternate Download Limit"),
            QStringLiteral("Limit for each download (KiB/s):"),
            current, 1, 1024 * 1024, 1, &ok);
        if (ok) {
            setAlternateSpeedLimit(true, static_cast<qint64>(kib) * 1024);
        }
    });
    alternateLimitButton_->setMenu(limitMenu);
    connect(alternateLimitButton_, &QToolButton::clicked, this, [this] {
        const bool enable = downloader_.alternateSpeedLimit() == 0;
        const qint64 configured = static_cast<qint64>(qMax(1, QSettings().value(
            QStringLiteral("downloads/alternateSpeedLimitKib"), 1024).toInt())) * 1024;
        setAlternateSpeedLimit(enable, configured);
    });
    statusBar()->addPermanentWidget(sessionRateLabel_);
    statusBar()->addPermanentWidget(sessionSizeLabel_);
    statusBar()->addPermanentWidget(alternateLimitButton_);
    updateSessionStatus();
    statusBar()->showMessage(QStringLiteral("Ready"));
}

void MainWindow::updateSessionStatus()
{
    double combinedSpeed = 0.0;
    for (const auto& id : std::as_const(activeTransfers_)) {
        const auto metric = transferMetrics_.constFind(id);
        if (metric != transferMetrics_.cend() && metric->hasSpeed) {
            combinedSpeed += metric->smoothedBytesPerSecond;
        }
    }
    if (sessionRateLabel_) {
        sessionRateLabel_->setText(
            QStringLiteral("Speed: %1/s").arg(formatBytes(static_cast<qint64>(combinedSpeed))));
    }
    if (sessionSizeLabel_) {
        sessionSizeLabel_->setText(
            QStringLiteral("Session: %1").arg(formatBytes(sessionDownloadedBytes_)));
    }
}

void MainWindow::setAlternateSpeedLimit(bool enabled, qint64 bytesPerSecond)
{
    const qint64 limit = qMax<qint64>(1024, bytesPerSecond);
    downloader_.setAlternateSpeedLimit(enabled ? limit : 0);
    QSettings settings;
    settings.setValue(QStringLiteral("downloads/alternateSpeedLimitEnabled"), enabled);
    settings.setValue(QStringLiteral("downloads/alternateSpeedLimitKib"), limit / 1024);
    if (alternateLimitButton_) {
        alternateLimitButton_->setIcon(staticActionIcon(
            enabled ? QStringLiteral("speed-limit-active") : QStringLiteral("speed-limit")));
        alternateLimitButton_->setText(enabled
            ? QStringLiteral("Limit: %1/s").arg(formatBytes(limit))
            : QStringLiteral("Limit: Off"));
        alternateLimitButton_->setToolTip(enabled
            ? QStringLiteral("Alternate per-download limit is enabled; click to disable")
            : QStringLiteral("Alternate per-download limit is disabled; click to enable"));
    }
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
        if (text == lastClipboardUrl_) {
            return;
        }
        QStringList urls;
        for (const auto& line : text.split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts)) {
            const QUrl url(line.trimmed());
            if (url.isValid() && (url.scheme() == QStringLiteral("http")
                || url.scheme() == QStringLiteral("https") || url.scheme() == QStringLiteral("ftp"))) {
                urls.append(line.trimmed());
            }
        }
        if (urls.isEmpty()) {
            return;
        }
        urls.removeDuplicates();
        lastClipboardUrl_ = text;
        QTimer::singleShot(0, this, [this, urls] {
            if (urls.size() == 1) {
                addUrl(urls.first());
            } else {
                addUrls(urls);
            }
        });
    });
}

void MainWindow::applyCategoryFilter()
{
    if (!downloads_ || !categories_ || !categories_->currentItem()) {
        return;
    }
    const auto selected = categories_->currentItem()->text(0);
    const auto status = statusFilter_ ? statusFilter_->currentText() : QStringLiteral("All statuses");
    const auto query = search_ ? search_->text().trimmed() : QString {};
    for (int row = 0; row < downloads_->rowCount(); ++row) {
        const auto* idItem = downloads_->item(row, 0);
        const auto category = idItem ? idItem->data(Qt::UserRole).toString() : QString {};
        const auto rowStatus = idItem ? idItem->data(Qt::UserRole + 1).toString() : QString {};
        const bool categoryMismatch = selected != QStringLiteral("All Downloads")
            && category.compare(selected, Qt::CaseInsensitive) != 0;
        const bool statusMismatch = status != QStringLiteral("All statuses")
            && status.compare(rowStatus, Qt::CaseInsensitive) != 0;
        bool searchMismatch = false;
        if (!query.isEmpty()) {
            searchMismatch = true;
            for (const int column : { 1, 2 }) {
                const auto* item = downloads_->item(row, column);
                if (item && item->text().contains(query, Qt::CaseInsensitive)) {
                    searchMismatch = false;
                    break;
                }
            }
            if (searchMismatch && category.contains(query, Qt::CaseInsensitive)) {
                searchMismatch = false;
            }
        }
        const bool hidden = categoryMismatch || statusMismatch || searchMismatch;
        downloads_->setRowHidden(row, hidden);
        if (hidden && downloads_->selectionModel()->isRowSelected(
                row, downloads_->rootIndex())) {
            downloads_->selectionModel()->select(
                downloads_->model()->index(row, 0),
                QItemSelectionModel::Deselect | QItemSelectionModel::Rows);
        }
    }
    updateDownloadActionStates();
}

void MainWindow::buildTray()
{
    tray_ = new QSystemTrayIcon(QApplication::windowIcon(), this);
    auto* menu = new QMenu(this);
    menu->addAction(QStringLiteral("Open qtIDM"), this, &MainWindow::raiseAndActivate);
    menu->addAction(QStringLiteral("Add URL"), this, [this] { addUrl(); });
    menu->addSeparator();
    menu->addAction(QStringLiteral("Quit"), this, [this] {
        quitting_ = true;
        qApp->quit();
    });
    tray_->setContextMenu(menu);
    tray_->show();
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (quitting_ || !tray_ || !tray_->isVisible()) {
        event->accept();
        return;
    }

    const auto records = repository_.listDownloads();
    const auto active = std::count_if(records.cbegin(), records.cend(), [](const DownloadRecord& record) {
        return record.status == DownloadStatus::Queued || record.status == DownloadStatus::Connecting
            || record.status == DownloadStatus::Downloading;
    });
    if (active > 0) {
        QMessageBox prompt(QMessageBox::Question, QStringLiteral("Downloads are active"),
                            QStringLiteral("%1 download%2 %3 still active.")
                                .arg(active).arg(active == 1 ? QString {} : QStringLiteral("s"))
                                .arg(active == 1 ? QStringLiteral("is") : QStringLiteral("are")),
                            QMessageBox::NoButton, this);
        prompt.setInformativeText(QStringLiteral("Keep qtIDM running in the system tray, or quit and stop the active downloads."));
        auto* keepRunning = prompt.addButton(QStringLiteral("Keep Running"), QMessageBox::AcceptRole);
        auto* quit = prompt.addButton(QStringLiteral("Quit qtIDM"), QMessageBox::DestructiveRole);
        prompt.addButton(QMessageBox::Cancel);
        prompt.exec();
        if (prompt.clickedButton() == quit) {
            quitting_ = true;
            event->accept();
            return;
        }
        if (prompt.clickedButton() != keepRunning) {
            event->ignore();
            return;
        }
    }

    hide();
    tray_->showMessage(QStringLiteral("qtIDM is still running"),
                       QStringLiteral("Use the system tray icon to reopen qtIDM or quit it."),
                       QSystemTrayIcon::Information, 4000);
    event->ignore();
}

void MainWindow::loadPersistedDownloads()
{
    for (const auto& record : repository_.listDownloads()) {
        onDownloadAdded(record);
    }
}

void MainWindow::applyRequestDefaults(DownloadRequest& request) const
{
    const QSettings settings;
    request.maxRetries = qBound(0, settings.value(QStringLiteral("downloads/maxRetries"), 5).toInt(), 999);
    request.retryBaseDelayMs = qBound(1, settings.value(QStringLiteral("downloads/retryDelaySeconds"), 1).toInt(), 3600) * 1000;
    request.connectTimeoutSeconds = qBound(1, settings.value(QStringLiteral("downloads/connectTimeoutSeconds"), 15).toInt(), 3600);
    request.lowSpeedTimeoutSeconds = qBound(1, settings.value(QStringLiteral("downloads/lowSpeedTimeoutSeconds"), 60).toInt(), 3600);
    request.maximumRedirects = qBound(0, settings.value(QStringLiteral("downloads/maximumRedirects"), 20).toInt(), 100);
    if (request.proxyUrl.trimmed().isEmpty()) {
        request.proxyUrl = settings.value(QStringLiteral("downloads/defaultProxy")).toString().trimmed();
    }
    if (request.speedLimitBytesPerSecond == 0) {
        request.speedLimitBytesPerSecond =
            static_cast<qint64>(settings.value(QStringLiteral("downloads/defaultSpeedLimitKib"), 0).toInt()) * 1024;
    }
    request.sessionDataLimitBytes =
        static_cast<qint64>(qMax(0, settings.value(QStringLiteral("downloads/sessionDataLimitMiB"), 0).toInt()))
        * 1024 * 1024;
    const auto userAgent = settings.value(QStringLiteral("downloads/defaultUserAgent")).toString().trimmed();
    if (!userAgent.isEmpty() && !containsHeader(request.headers, u"User-Agent")) {
        request.headers.insert(QStringLiteral("User-Agent"), userAgent);
    }
}

QList<int> MainWindow::selectedRows() const
{
    QList<int> result;
    if (!downloads_ || !downloads_->selectionModel()) {
        return result;
    }
    for (const auto& index : downloads_->selectionModel()->selectedRows()) {
        result.append(index.row());
    }
    std::sort(result.begin(), result.end());
    return result;
}

bool MainWindow::hasDuplicateUrl(const QUrl& url) const
{
    auto normalized = url.adjusted(QUrl::NormalizePathSegments | QUrl::RemoveFragment);
    for (const auto& record : repository_.listDownloads()) {
        if (record.url.adjusted(QUrl::NormalizePathSegments | QUrl::RemoveFragment) == normalized) {
            return true;
        }
    }
    return false;
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
