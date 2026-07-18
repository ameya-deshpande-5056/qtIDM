#include "integration/MeteredNetworkMonitor.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDBusVariant>

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
}

}
