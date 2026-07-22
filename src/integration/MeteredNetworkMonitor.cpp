#include "integration/MeteredNetworkMonitor.h"

#ifndef Q_OS_WIN
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDBusVariant>
#else
#include <QProcess>
#endif

namespace qtidm {

MeteredNetworkMonitor::MeteredNetworkMonitor(QObject* parent)
    : QObject(parent)
{
    timer_.setInterval(5000);
    connect(&timer_, &QTimer::timeout, this, &MeteredNetworkMonitor::refresh);
    timer_.start();
    refresh();
}

bool MeteredNetworkMonitor::isMetered() const
{
    return metered_;
}

bool MeteredNetworkMonitor::valueMeansMetered(uint value)
{
    // NetworkManager: 1 = yes, 3 = guessed yes.
    return value == 1 || value == 3;
}

void MeteredNetworkMonitor::refresh()
{
#ifndef Q_OS_WIN
    QDBusInterface properties(
        QStringLiteral("org.freedesktop.NetworkManager"),
        QStringLiteral("/org/freedesktop/NetworkManager"),
        QStringLiteral("org.freedesktop.DBus.Properties"),
        QDBusConnection::systemBus());
    if (!properties.isValid()) {
        return;
    }
    const auto reply = properties.call(
        QStringLiteral("Get"),
        QStringLiteral("org.freedesktop.NetworkManager"),
        QStringLiteral("Metered"));
    if (reply.type() == QDBusMessage::ErrorMessage || reply.arguments().isEmpty()) {
        return;
    }
    const auto value = qvariant_cast<QDBusVariant>(reply.arguments().constFirst()).variant().toUInt();
    const bool metered = valueMeansMetered(value);
    if (metered_ != metered) {
        metered_ = metered;
        emit meteredChanged(metered_);
    }
#else
    // Windows: attempt to detect metered network via PowerShell.
    // Falls back to unmetered if detection fails.
    QProcess proc;
    proc.start(QStringLiteral("powershell"),
               { QStringLiteral("-NoProfile"),
                 QStringLiteral("-Command"),
                 QStringLiteral("(Get-NetConnectionProfile | Where-Object { $_.IPv4Connectivity -eq 2 }).Metered") });
    if (!proc.waitForFinished(2000)) {
        return;
    }
    const auto output = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
    bool ok = false;
    const uint value = output.toUInt(&ok);
    if (!ok) {
        return;
    }
    const bool metered = valueMeansMetered(value);
    if (metered_ != metered) {
        metered_ = metered;
        emit meteredChanged(metered_);
    }
#endif
}

}