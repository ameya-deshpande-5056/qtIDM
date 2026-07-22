#include "theme/ThemeManager.h"

#include <QApplication>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QDir>
#include <QPalette>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QVariant>

#ifndef Q_OS_WIN
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusReply>
#else
#include <QSettings>
#endif

namespace qtidm {

namespace {

QString commandOutput(const QString& program, const QStringList& arguments)
{
    QProcess process;
    process.start(program, arguments);
    if (!process.waitForFinished(1000) || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        return {};
    }
    return QString::fromUtf8(process.readAllStandardOutput()).trimmed().toLower();
}

ThemeMode themeFromName(const QString& name, ThemeMode fallback)
{
    if (name.contains(QStringLiteral("dark"))) {
        return ThemeMode::Dark;
    }
    if (name.contains(QStringLiteral("light"))) {
        return ThemeMode::Light;
    }
    return fallback;
}

}

ThemeManager::ThemeManager(QObject* parent)
    : QObject(parent)
{
    watcher_ = new QFileSystemWatcher(this);
    refreshTimer_ = new QTimer(this);
    refreshTimer_->setSingleShot(true);
    refreshTimer_->setInterval(100);
    connect(watcher_, &QFileSystemWatcher::fileChanged, this, [this](const QString&) { scheduleThemeRefresh(); });
    connect(watcher_, &QFileSystemWatcher::directoryChanged, this, [this](const QString&) { scheduleThemeRefresh(); });
    connect(refreshTimer_, &QTimer::timeout, this, &ThemeManager::refreshFromSystemTheme);
    qApp->installEventFilter(this);
    setupThemeWatchers();
    mode_ = detectSystemTheme();
#ifndef Q_OS_WIN
    QDBusConnection::sessionBus().connect(QStringLiteral("org.freedesktop.portal.Desktop"),
                                          QStringLiteral("/org/freedesktop/portal/desktop"),
                                          QStringLiteral("org.freedesktop.portal.Settings"),
                                          QStringLiteral("SettingChanged"),
                                          this,
                                          SLOT(refreshFromPortal()));
    refreshFromPortal();
#endif
}

ThemeMode ThemeManager::mode() const
{
    return mode_;
}

QString ThemeManager::styleSheet() const
{
    if (mode_ == ThemeMode::Dark) {
        return QStringLiteral(
            "QMainWindow,QDialog{background:#202124;color:#f1f3f4;}"
            "QWidget{color:#f1f3f4;}"
            "QToolBar{background:#2b2d31;border-bottom:1px solid #3c4043;padding:3px;spacing:4px;}"
            "QToolButton{padding:4px 8px;border:1px solid transparent;border-radius:2px;}"
            "QToolButton:hover{background:#3c4043;border-color:#5f6368;}"
            "QPushButton,QLineEdit,QPlainTextEdit,QSpinBox,QComboBox{background:#2b2d31;color:#f1f3f4;border:1px solid #5f6368;border-radius:2px;padding:3px;}"
            "QPushButton:hover{background:#3c4043;}"
            "QTreeWidget,QTableWidget,QTreeView,QTableView{background:#1f1f1f;color:#f1f3f4;gridline-color:#3c4043;border:1px solid #3c4043;}"
            "QAbstractItemView::item:selected{background:#3c6e9f;color:#fff;}"
            "QHeaderView::section{background:#2b2d31;color:#f1f3f4;border:0;border-right:1px solid #3c4043;padding:4px;}"
            "QStatusBar{background:#2b2d31;color:#f1f3f4;border-top:1px solid #3c4043;}"
            "QMenu{background:#2b2d31;color:#f1f3f4;border:1px solid #5f6368;}"
            "QMenu::item:selected{background:#3c4043;}");
    }
    return QStringLiteral(
        "QMainWindow,QDialog{background:#f0f0f0;color:#111;}"
        "QToolBar{background:#f6f6f6;border-bottom:1px solid #c8c8c8;padding:3px;spacing:4px;}"
        "QToolButton{padding:4px 8px;border:1px solid transparent;border-radius:2px;}"
        "QToolButton:hover{background:#e6f0ff;border-color:#7da2ce;}"
        "QTreeWidget,QTableWidget{background:#fff;color:#111;gridline-color:#d7d7d7;border:1px solid #c8c8c8;}"
        "QHeaderView::section{background:#f3f3f3;color:#111;border:0;border-right:1px solid #c8c8c8;padding:4px;}"
        "QStatusBar{background:#f6f6f6;color:#111;border-top:1px solid #c8c8c8;}");
}

ThemeMode ThemeManager::detectSystemTheme()
{
#ifndef Q_OS_WIN
    const auto desktop = qEnvironmentVariable("XDG_CURRENT_DESKTOP").toUpper();
    const auto window = QApplication::palette().color(QPalette::Window).lightness();
    const auto text = QApplication::palette().color(QPalette::WindowText).lightness();
    const auto paletteMode = window < text ? ThemeMode::Dark : ThemeMode::Light;

    if (desktop.contains(QStringLiteral("GNOME"))) {
        const auto preference = commandOutput(QStringLiteral("gsettings"),
                                              { QStringLiteral("get"), QStringLiteral("org.gnome.desktop.interface"), QStringLiteral("color-scheme") });
        if (preference.contains(QStringLiteral("prefer-dark"))) {
            return ThemeMode::Dark;
        }
        if (preference.contains(QStringLiteral("prefer-light"))) {
            return ThemeMode::Light;
        }
        return themeFromName(commandOutput(QStringLiteral("gsettings"),
                                           { QStringLiteral("get"), QStringLiteral("org.gnome.desktop.interface"), QStringLiteral("gtk-theme") }), paletteMode);
    }

    if (desktop.contains(QStringLiteral("CINNAMON"))) {
        return themeFromName(commandOutput(QStringLiteral("gsettings"),
                                           { QStringLiteral("get"), QStringLiteral("org.cinnamon.desktop.interface"), QStringLiteral("gtk-theme") }), paletteMode);
    }
    if (desktop.contains(QStringLiteral("KDE")) || desktop.contains(QStringLiteral("PLASMA"))) {
        const auto configFile = QStandardPaths::locate(QStandardPaths::ConfigLocation, QStringLiteral("kdeglobals"));
        if (!configFile.isEmpty()) {
            QSettings kdeGlobals(configFile, QSettings::IniFormat);
            kdeGlobals.beginGroup(QStringLiteral("General"));
            const auto colorScheme = kdeGlobals.value(QStringLiteral("ColorScheme")).toString().toLower();
            if (colorScheme.contains(QStringLiteral("dark"))) {
                return ThemeMode::Dark;
            }
            if (colorScheme.contains(QStringLiteral("light"))) {
                return ThemeMode::Light;
            }
        }
    }
    if (desktop.contains(QStringLiteral("XFCE"))) {
        return themeFromName(commandOutput(QStringLiteral("xfconf-query"),
                                           { QStringLiteral("-c"), QStringLiteral("xsettings"), QStringLiteral("-p"), QStringLiteral("/Net/ThemeName") }), paletteMode);
    }

    return paletteMode;
#else
    // Windows: read the personalization registry key.
    // HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize
    // AppsUseLightTheme: 0 = dark, 1 = light
    QSettings reg(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"),
                  QSettings::NativeFormat);
    const auto appsUseLight = reg.value(QStringLiteral("AppsUseLightTheme"), 1).toInt();
    const auto window = QApplication::palette().color(QPalette::Window).lightness();
    const auto text = QApplication::palette().color(QPalette::WindowText).lightness();
    const auto paletteMode = window < text ? ThemeMode::Dark : ThemeMode::Light;
    return appsUseLight == 0 ? ThemeMode::Dark : paletteMode;
#endif
}

bool ThemeManager::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == qApp && event->type() == QEvent::ApplicationPaletteChange) {
        scheduleThemeRefresh();
    }
    return QObject::eventFilter(watched, event);
}

void ThemeManager::setupThemeWatchers()
{
    const auto watchedFiles = watcher_->files();
    if (!watchedFiles.isEmpty()) {
        watcher_->removePaths(watchedFiles);
    }
    const auto watchedDirectories = watcher_->directories();
    if (!watchedDirectories.isEmpty()) {
        watcher_->removePaths(watchedDirectories);
    }

#ifndef Q_OS_WIN
    QStringList paths;
    const auto desktop = qEnvironmentVariable("XDG_CURRENT_DESKTOP").toUpper();
    const auto configDir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);

    if (desktop.contains(QStringLiteral("KDE")) || desktop.contains(QStringLiteral("PLASMA"))) {
        const auto kdeglobals = QStandardPaths::locate(QStandardPaths::ConfigLocation, QStringLiteral("kdeglobals"));
        if (!kdeglobals.isEmpty()) {
            paths << kdeglobals;
        }
    }

    if (desktop.contains(QStringLiteral("CINNAMON"))) {
        const auto gtk3 = configDir + QStringLiteral("/gtk-3.0/settings.ini");
        const auto gtk4 = configDir + QStringLiteral("/gtk-4.0/settings.ini");
        paths << gtk3 << gtk4;
    }

    if (desktop.contains(QStringLiteral("GNOME")) || desktop.contains(QStringLiteral("CINNAMON"))) {
        paths << configDir + QStringLiteral("/dconf/user");
    }

    if (desktop.contains(QStringLiteral("XFCE"))) {
        const auto xfceSettings = configDir + QStringLiteral("/xfce4/xfconf/xfce-perchannel-xml/xsettings.xml");
        paths << xfceSettings;
    }

    if (desktop.contains(QStringLiteral("LXQT"))) {
        const auto lxqtSession = configDir + QStringLiteral("/lxqt/session.conf");
        const auto lxqtSettings = configDir + QStringLiteral("/lxqt/lxqt.conf");
        paths << lxqtSession << lxqtSettings;
    }

    if (desktop.contains(QStringLiteral("LXDE"))) {
        const auto gtk2 = configDir + QStringLiteral("/gtk-2.0/gtkrc");
        const auto gtk3 = configDir + QStringLiteral("/gtk-3.0/settings.ini");
        paths << gtk2 << gtk3;
    }

    for (const auto& path : paths) {
        const QFileInfo info(path);
        if (info.exists()) {
            watcher_->addPath(path);
        } else if (!info.dir().path().isEmpty() && QFileInfo::exists(info.dir().path())) {
            watcher_->addPath(info.dir().path());
        }
    }
#else
    // Windows: no file-based watcher for registry. The palette change event
    // (ApplicationPaletteChange) is caught by eventFilter above.
    Q_UNUSED(watcher_);
#endif
}

void ThemeManager::scheduleThemeRefresh()
{
    if (refreshTimer_->isActive()) {
        refreshTimer_->stop();
    }
    refreshTimer_->start();
}

void ThemeManager::refreshTheme()
{
    const auto next = detectSystemTheme();
    if (next == mode_) {
        return;
    }
    mode_ = next;
    emit themeChanged();
}

void ThemeManager::refreshFromSystemTheme()
{
    if (watcher_) {
        setupThemeWatchers();
    }
    refreshTheme();
}

#ifndef Q_OS_WIN
void ThemeManager::refreshFromPortal()
{
    QDBusInterface iface(QStringLiteral("org.freedesktop.portal.Desktop"),
                         QStringLiteral("/org/freedesktop/portal/desktop"),
                         QStringLiteral("org.freedesktop.portal.Settings"),
                         QDBusConnection::sessionBus());
    const QDBusReply<QVariant> reply = iface.call(QStringLiteral("Read"),
                                                  QStringLiteral("org.freedesktop.appearance"),
                                                  QStringLiteral("color-scheme"));
    const auto preference = reply.isValid() ? reply.value().toUInt() : 0;
    const auto next = preference == 1 ? ThemeMode::Dark
        : preference == 2 ? ThemeMode::Light
        : detectSystemTheme();
    if (next == mode_) {
        return;
    }
    mode_ = next;
    emit themeChanged();
}
#endif

}