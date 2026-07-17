#include "theme/ThemeManager.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusReply>
#include <QVariant>

namespace qtidm {

ThemeManager::ThemeManager(QObject* parent)
    : QObject(parent)
{
    QDBusConnection::sessionBus().connect(QStringLiteral("org.freedesktop.portal.Desktop"),
                                          QStringLiteral("/org/freedesktop/portal/desktop"),
                                          QStringLiteral("org.freedesktop.portal.Settings"),
                                          QStringLiteral("SettingChanged"),
                                          this,
                                          SLOT(refreshFromPortal()));
    refreshFromPortal();
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
            "QToolBar{background:#2b2d31;border-bottom:1px solid #3c4043;padding:3px;spacing:4px;}"
            "QToolButton{padding:4px 8px;border:1px solid transparent;border-radius:2px;}"
            "QToolButton:hover{background:#3c4043;border-color:#5f6368;}"
            "QTreeWidget,QTableWidget{background:#1f1f1f;color:#f1f3f4;gridline-color:#3c4043;border:1px solid #3c4043;}"
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

void ThemeManager::refreshFromPortal()
{
    QDBusInterface iface(QStringLiteral("org.freedesktop.portal.Desktop"),
                         QStringLiteral("/org/freedesktop/portal/desktop"),
                         QStringLiteral("org.freedesktop.portal.Settings"),
                         QDBusConnection::sessionBus());
    const QDBusReply<QVariant> reply = iface.call(QStringLiteral("Read"),
                                                  QStringLiteral("org.freedesktop.appearance"),
                                                  QStringLiteral("color-scheme"));
    const auto next = reply.isValid() && reply.value().toUInt() == 1 ? ThemeMode::Dark : ThemeMode::Light;
    if (next == mode_) {
        return;
    }
    mode_ = next;
    emit themeChanged();
}

}
