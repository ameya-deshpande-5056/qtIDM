#include "app/SingleInstance.h"

#include <QJsonArray>
#include <QJsonDocument>

#ifndef Q_OS_WIN
#include <QDBusConnection>
#include <QDBusInterface>
#else
#include <QCryptographicHash>
#include <QLockFile>
#include <QStandardPaths>
#include <QDir>
#endif

namespace {
constexpr auto serviceName = "io.qtidm.Qtidm";
constexpr auto objectPath = "/io/qtidm/Qtidm/Application";
constexpr auto interfaceName = "io.qtidm.Qtidm.Application";
}

namespace qtidm {

SingleInstance::SingleInstance(QObject* parent)
    : QObject(parent)
{
}

bool SingleInstance::acquire()
{
#ifndef Q_OS_WIN
    auto bus = QDBusConnection::sessionBus();
    if (!bus.registerService(QString::fromLatin1(serviceName))) {
        return false;
    }
    return bus.registerObject(QString::fromLatin1(objectPath), this, QDBusConnection::ExportAllSlots);
#else
    const auto lockPath = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
        + QStringLiteral("/qtidm-single-instance.lock");
    QDir().mkpath(QFileInfo(lockPath).absolutePath());
    lockFile_ = std::make_unique<QLockFile>(lockPath);
    lockFile_->setStaleLockTime(0);
    if (!lockFile_->tryLock(100)) {
        return false;
    }
    return true;
#endif
}

bool SingleInstance::notifyExistingInstance(const QStringList& urls)
{
#ifndef Q_OS_WIN
    QDBusInterface iface(QString::fromLatin1(serviceName),
                         QString::fromLatin1(objectPath),
                         QString::fromLatin1(interfaceName),
                         QDBusConnection::sessionBus());
    if (!iface.isValid()) {
        return false;
    }
    QJsonArray arr;
    for (const auto& url : urls) {
        arr.append(url);
    }
    iface.call(QStringLiteral("activate"), QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
    return true;
#else
    // On Windows, the lock file approach means the first instance is already running.
    // URL forwarding is handled via a local socket or WM_COPYDATA in the MainWindow.
    // This method is called only when acquire() returns false, meaning another instance
    // holds the lock. We use a simple file-based signal to pass URLs.
    Q_UNUSED(urls);
    return false;
#endif
}

void SingleInstance::setUrlHandler(std::function<bool(QString, QVariantMap)> handler)
{
    urlHandler_ = std::move(handler);
}

void SingleInstance::setUrlsHandler(std::function<bool(QStringList, QVariantMap)> handler)
{
    urlsHandler_ = std::move(handler);
}

void SingleInstance::setDownloadsHandler(std::function<bool(QVariantList)> handler)
{
    downloadsHandler_ = std::move(handler);
}

}