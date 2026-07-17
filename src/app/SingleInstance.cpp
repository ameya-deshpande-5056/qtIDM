#include "app/SingleInstance.h"

#include <QDBusConnection>
#include <QDBusInterface>

namespace {
constexpr auto serviceName = "io.github.qtidm";
constexpr auto objectPath = "/io/github/qtidm/Application";
constexpr auto interfaceName = "io.github.qtidm.Application";
}

namespace qtidm {

SingleInstance::SingleInstance(QObject* parent)
    : QObject(parent)
{
}

bool SingleInstance::acquire()
{
    auto bus = QDBusConnection::sessionBus();
    if (!bus.registerService(QString::fromLatin1(serviceName))) {
        return false;
    }
    return bus.registerObject(QString::fromLatin1(objectPath), this, QDBusConnection::ExportAllSlots);
}

bool SingleInstance::notifyExistingInstance(const QStringList& urls)
{
    QDBusInterface iface(QString::fromLatin1(serviceName),
                         QString::fromLatin1(objectPath),
                         QString::fromLatin1(interfaceName),
                         QDBusConnection::sessionBus());
    if (!iface.isValid()) {
        return false;
    }
    if (urls.isEmpty()) {
        iface.call(QStringLiteral("Activate"));
    } else {
        for (const auto& url : urls) {
            iface.call(QStringLiteral("AddUrl"), url, QVariantMap {});
        }
    }
    return true;
}

void SingleInstance::Activate()
{
    emit activateRequested();
}

void SingleInstance::AddUrl(const QString& url, const QVariantMap& headers)
{
    emit urlReceived(url, headers);
}

}
