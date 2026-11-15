#include "app/AutomationIpc.h"

#include "app/Paths.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

namespace qtidm {

AutomationIpc::AutomationIpc(QString directory)
    : directory_(std::move(directory))
{
}

bool AutomationIpc::start()
{
    const auto directory = directory_.isEmpty() ? Paths::dataDir() : directory_;
    if (!QDir().mkpath(directory)) {
        lastError_ = QStringLiteral("Could not create %1").arg(directory);
        return false;
    }
    token_ = newToken();
    QFile file(path());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        lastError_ = file.errorString();
        return false;
    }
    const QJsonObject document {
        { QStringLiteral("token"), token_ },
        { QStringLiteral("pid"), static_cast<qint64>(QCoreApplication::applicationPid()) },
        { QStringLiteral("service"), QString::fromLatin1(serviceName) },
        { QStringLiteral("object"), QString::fromLatin1(objectPath) },
        { QStringLiteral("interface"), QString::fromLatin1(interfaceName) },
        // Reserved for the loopback HTTP transport; the D-Bus channel needs no port.
        { QStringLiteral("port"), QJsonValue::Null },
        { QStringLiteral("version"), QCoreApplication::applicationVersion() },
        { QStringLiteral("updated"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate) }
    };
    file.write(QJsonDocument(document).toJson(QJsonDocument::Indented));
    file.close();
    // The token lets a caller act on this process; keep it owner-readable only.
    if (!QFile::setPermissions(path(), QFile::ReadOwner | QFile::WriteOwner)) {
        lastError_ = QStringLiteral("Could not restrict permissions on %1").arg(path());
        return false;
    }
    return true;
}

void AutomationIpc::stop()
{
    token_.clear();
    QFile::remove(path());
}

QString AutomationIpc::path() const
{
    const auto directory = directory_.isEmpty() ? Paths::dataDir() : directory_;
    return directory + QLatin1Char('/') + QString::fromLatin1(fileName);
}

QString AutomationIpc::token() const
{
    return token_;
}

QString AutomationIpc::lastError() const
{
    return lastError_;
}

QString AutomationIpc::newToken()
{
    // 122 random bits, hex, with the UUID's dashes stripped.
    return QUuid::createUuid().toString(QUuid::WithoutBraces).remove(QLatin1Char('-'));
}

}
