#include "ui/UpdateDialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QTableWidget>
#include <QVBoxLayout>

namespace qtidm {

UpdateDialog::UpdateDialog(UpdateCheckResult result, UpdateChecker* checker, QWidget* parent)
    : QDialog(parent)
    , result_(std::move(result))
    , checker_(checker)
{
    setWindowTitle(QStringLiteral("Software & Dependency Updates"));
    setMinimumWidth(680);
    setMinimumHeight(520);
    setupUi();
    populateData();

    if (checker_) {
        connect(checker_, &UpdateChecker::checkStarted, this, [this] {
            progressBar_->setVisible(true);
            progressBar_->setRange(0, 0); // indeterminate
            checkAgainButton_->setEnabled(false);
        });

        connect(checker_, &UpdateChecker::checkFinished, this, [this](const UpdateCheckResult& res) {
            progressBar_->setVisible(false);
            checkAgainButton_->setEnabled(true);
            updateResult(res);
        });

        connect(checker_, &UpdateChecker::checkFailed, this, [this](const QString& err) {
            progressBar_->setVisible(false);
            checkAgainButton_->setEnabled(true);
            logView_->setVisible(true);
            logView_->appendPlainText(QStringLiteral("Update check failed: %1").arg(err));
        });

        connect(checker_, &UpdateChecker::dependencyUpdateStarted, this, [this](const QString& dep) {
            progressBar_->setVisible(true);
            progressBar_->setRange(0, 0);
            logView_->setVisible(true);
            logView_->appendPlainText(QStringLiteral("--- Starting update for %1 ---").arg(dep));
        });

        connect(checker_, &UpdateChecker::dependencyUpdateOutput, this, [this](const QString&, const QString& text) {
            logView_->insertPlainText(text);
            logView_->ensureCursorVisible();
        });

        connect(checker_, &UpdateChecker::dependencyUpdateFinished, this, [this](const QString& /* dep */, bool success, const QString& msg) {
            progressBar_->setVisible(false);
            logView_->appendPlainText(QStringLiteral(">>> %1\n").arg(msg));
            if (success && checker_) {
                // Refresh status after successful update
                checker_->checkForUpdates(false);
            }
        });
    }
}

UpdateDialog::~UpdateDialog() = default;

void UpdateDialog::setupUi()
{
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(14);
    mainLayout->setContentsMargins(18, 18, 18, 18);

    // Header with application icon
    auto* headerLayout = new QHBoxLayout;
    auto* iconLabel = new QLabel;
    iconLabel->setPixmap(QApplication::windowIcon().pixmap(48, 48));
    headerLayout->addWidget(iconLabel);

    auto* headerTextLayout = new QVBoxLayout;
    auto* titleLabel = new QLabel(QStringLiteral("Software & Dependency Updates"));
    auto font = titleLabel->font();
    font.setBold(true);
    font.setPointSize(font.pointSize() + 2);
    titleLabel->setFont(font);

    auto* subtitleLabel = new QLabel(QStringLiteral("Inspect and apply updates for qtIDM and external tools."));
    subtitleLabel->setStyleSheet(QStringLiteral("color: gray;"));
    headerTextLayout->addWidget(titleLabel);
    headerTextLayout->addWidget(subtitleLabel);
    headerLayout->addLayout(headerTextLayout, 1);
    mainLayout->addLayout(headerLayout);

    // Group 1: Application (qtIDM)
    auto* appGroup = new QGroupBox(QStringLiteral("qtIDM Application"));
    auto* appLayout = new QVBoxLayout(appGroup);

    auto* appRowLayout = new QHBoxLayout;
    appStatusLabel_ = new QLabel;
    appStatusLabel_->setWordWrap(true);
    appUpdateButton_ = new QPushButton(QStringLiteral("Open Release Page…"));
    appUpdateButton_->setVisible(false);
    connect(appUpdateButton_, &QPushButton::clicked, this, [this] {
        if (!result_.app.releaseUrl.isEmpty()) {
            QDesktopServices::openUrl(QUrl(result_.app.releaseUrl));
        }
    });

    appRowLayout->addWidget(appStatusLabel_, 1);
    appRowLayout->addWidget(appUpdateButton_);
    appLayout->addLayout(appRowLayout);

    releaseNotesText_ = new QPlainTextEdit;
    releaseNotesText_->setReadOnly(true);
    releaseNotesText_->setMaximumHeight(100);
    releaseNotesText_->setVisible(false);
    appLayout->addWidget(releaseNotesText_);

    mainLayout->addWidget(appGroup);

    // Group 2: Dependencies Table
    auto* depsGroup = new QGroupBox(QStringLiteral("Dependencies (External Tools)"));
    auto* depsLayout = new QVBoxLayout(depsGroup);

    dependenciesTable_ = new QTableWidget;
    dependenciesTable_->setColumnCount(5);
    dependenciesTable_->setHorizontalHeaderLabels({
        QStringLiteral("Component"),
        QStringLiteral("Installed"),
        QStringLiteral("Latest"),
        QStringLiteral("Status"),
        QStringLiteral("Action")
    });
    dependenciesTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    dependenciesTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    dependenciesTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    dependenciesTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    dependenciesTable_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    dependenciesTable_->verticalHeader()->setVisible(false);
    dependenciesTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    dependenciesTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    depsLayout->addWidget(dependenciesTable_);

    auto* depsButtonLayout = new QHBoxLayout;
    updateAllDepsButton_ = new QPushButton(QStringLiteral("Update All Available Dependencies"));
    updateAllDepsButton_->setEnabled(false);
    connect(updateAllDepsButton_, &QPushButton::clicked, this, &UpdateDialog::updateAllDependencies);
    depsButtonLayout->addStretch();
    depsButtonLayout->addWidget(updateAllDepsButton_);
    depsLayout->addLayout(depsButtonLayout);

    mainLayout->addWidget(depsGroup);

    // Progress bar (hidden initially)
    progressBar_ = new QProgressBar;
    progressBar_->setTextVisible(false);
    progressBar_->setVisible(false);
    progressBar_->setFixedHeight(6);
    mainLayout->addWidget(progressBar_);

    // Output Log view (hidden initially)
    logView_ = new QPlainTextEdit;
    logView_->setReadOnly(true);
    logView_->setMaximumHeight(110);
    logView_->setVisible(false);
    mainLayout->addWidget(logView_);

    // Footer
    auto* footerLayout = new QHBoxLayout;
    QSettings settings;
    startupCheckCheckbox_ = new QCheckBox(QStringLiteral("Check for updates automatically at startup"));
    startupCheckCheckbox_->setChecked(settings.value(QStringLiteral("updates/checkAtStartup"), true).toBool());
    connect(startupCheckCheckbox_, &QCheckBox::toggled, [](bool checked) {
        QSettings s;
        s.setValue(QStringLiteral("updates/checkAtStartup"), checked);
    });
    footerLayout->addWidget(startupCheckCheckbox_);

    footerLayout->addStretch();

    checkAgainButton_ = new QPushButton(QStringLiteral("Check Again"));
    connect(checkAgainButton_, &QPushButton::clicked, this, [this] {
        if (checker_) {
            checker_->checkForUpdates(false);
        }
    });
    footerLayout->addWidget(checkAgainButton_);

    auto* closeButton = new QPushButton(QStringLiteral("Close"));
    connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);
    footerLayout->addWidget(closeButton);

    mainLayout->addLayout(footerLayout);
}

void UpdateDialog::populateData()
{
    // Application section
    if (result_.app.updateAvailable) {
        appStatusLabel_->setText(QStringLiteral("<b style=\"color: #1b5e20;\">A new version of qtIDM is available: %1</b> (Installed: %2)")
                                     .arg(result_.app.latestVersion, result_.app.currentVersion));
        appUpdateButton_->setVisible(true);
        if (!result_.app.releaseNotes.isEmpty()) {
            releaseNotesText_->setPlainText(result_.app.releaseNotes);
            releaseNotesText_->setVisible(true);
        } else {
            releaseNotesText_->setVisible(false);
        }
    } else if (result_.app.checkSucceeded) {
        appStatusLabel_->setText(QStringLiteral("<span style=\"color: #2e7d32;\">✔ qtIDM is up to date (version %1).</span>")
                                     .arg(result_.app.currentVersion));
        appUpdateButton_->setVisible(false);
        releaseNotesText_->setVisible(false);
    } else {
        appStatusLabel_->setText(QStringLiteral("<span style=\"color: #c62828;\">Could not check application updates: %1</span>")
                                     .arg(result_.app.errorMessage));
        appUpdateButton_->setVisible(false);
        releaseNotesText_->setVisible(false);
    }

    // Dependencies table
    dependenciesTable_->setRowCount(result_.dependencies.size());
    bool hasAutoUpdates = false;

    for (int i = 0; i < result_.dependencies.size(); ++i) {
        const auto& dep = result_.dependencies[i];

        auto* nameItem = new QTableWidgetItem(dep.name);
        nameItem->setFont([nameItem] { auto f = nameItem->font(); f.setBold(true); return f; }());
        dependenciesTable_->setItem(i, 0, nameItem);

        auto* instItem = new QTableWidgetItem(dep.installed ? dep.installedVersion : QStringLiteral("—"));
        dependenciesTable_->setItem(i, 1, instItem);

        auto* latestItem = new QTableWidgetItem(!dep.latestVersion.isEmpty() ? dep.latestVersion : QStringLiteral("—"));
        dependenciesTable_->setItem(i, 2, latestItem);

        auto* statusItem = new QTableWidgetItem(dep.statusText);
        if (dep.updateAvailable) {
            statusItem->setForeground(QBrush(QColor(QStringLiteral("#e65100")))); // Amber/Orange
        } else if (dep.installed) {
            statusItem->setForeground(QBrush(QColor(QStringLiteral("#2e7d32")))); // Green
        } else {
            statusItem->setForeground(QBrush(QColor(QStringLiteral("#757575")))); // Gray
        }
        dependenciesTable_->setItem(i, 3, statusItem);

        if (dep.updateAvailable && dep.canAutoUpdate) {
            hasAutoUpdates = true;
            auto* updateBtn = new QPushButton(QStringLiteral("Update"));
            const auto depName = dep.name;
            connect(updateBtn, &QPushButton::clicked, this, [this, depName] {
                startUpdate(depName);
            });
            dependenciesTable_->setCellWidget(i, 4, updateBtn);
        } else if (!dep.installed && !dep.installCommand.isEmpty()) {
            auto* infoBtn = new QPushButton(QStringLiteral("Install Info"));
            const auto cmd = dep.installCommand;
            connect(infoBtn, &QPushButton::clicked, this, [this, cmd] {
                logView_->setVisible(true);
                logView_->appendPlainText(QStringLiteral("To install this dependency, run:\n  %1\n").arg(cmd));
            });
            dependenciesTable_->setCellWidget(i, 4, infoBtn);
        } else {
            dependenciesTable_->setCellWidget(i, 4, nullptr);
        }
    }

    updateAllDepsButton_->setEnabled(hasAutoUpdates);
}

void UpdateDialog::updateResult(const UpdateCheckResult& result)
{
    result_ = result;
    populateData();
}

void UpdateDialog::startUpdate(const QString& dependencyName)
{
    if (checker_) {
        checker_->runDependencyUpdate(dependencyName);
    }
}

void UpdateDialog::updateAllDependencies()
{
    for (const auto& dep : result_.dependencies) {
        if (dep.updateAvailable && dep.canAutoUpdate) {
            startUpdate(dep.name);
        }
    }
}

} // namespace qtidm
