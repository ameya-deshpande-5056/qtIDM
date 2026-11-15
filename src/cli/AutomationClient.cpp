#include "cli/AutomationClient.h"

#include "app/AutomationIpc.h"
#include "app/Paths.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QFile>
#include <QJsonDocument>

namespace qtidm {

AutomationClient::AutomationClient(QString directory)
    : directory_(std::move(directory))
{
}

bool AutomationClient::resolve(QString* error) const
{
    if (!resolved_) {
        resolved_ = true;
        const auto directory = directory_.isEmpty() ? Paths::dataDir() : directory_;
        discoveryPath_ = directory + QLatin1Char('/') + QString::fromLatin1(AutomationIpc::fileName);
        QFile file(discoveryPath_);
        if (!file.exists()) {
            resolveError_ = QStringLiteral("qtIDM is not running (%1 is missing).").arg(discoveryPath_);
        } else if (!file.open(QIODevice::ReadOnly)) {
            resolveError_ = QStringLiteral("Cannot read %1: %2").arg(discoveryPath_, file.errorString());
        } else if (!QJsonDocument::fromJson(file.readAll()).isObject()) {
            resolveError_ = QStringLiteral("%1 is not a valid qtIDM discovery file.").arg(discoveryPath_);
        }
    }
    if (!resolveError_.isEmpty()) {
        if (error) {
            *error = resolveError_;
        }
        return false;
    }
    return true;
}

QString AutomationClient::call(const QString& method, const QVariantList& arguments,
                               QString* error) const
{
    if (!resolve(error)) {
        return {};
    }
    // Channel names are canonical constants, not values read from the file, so a
    // stale or edited discovery file cannot redirect the client.
    const auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        if (error) {
            *error = QStringLiteral("No session bus is available, so qtIDM cannot be reached.");
        }
        return {};
    }
    auto message = QDBusMessage::createMethodCall(QString::fromLatin1(AutomationIpc::serviceName),
                                                  QString::fromLatin1(AutomationIpc::objectPath),
                                                  QString::fromLatin1(AutomationIpc::interfaceName),
                                                  method);
    message.setArguments(arguments);
    const auto reply = bus.call(message, QDBus::Block, 30000);
    if (reply.type() == QDBusMessage::ErrorMessage) {
        if (error) {
            *error = reply.errorMessage();
        }
        return {};
    }
    return reply.arguments().value(0).toString();
}

}
