#include "app/SingleInstance.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QJsonArray>
#include <QJsonDocument>

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

void SingleInstance::Activate()
{
    emit activateRequested();
}

bool SingleInstance::AddUrl(const QString& url, const QVariantMap& headers)
{
    if (urlHandler_) {
        return urlHandler_(url, headers);
    }
    emit urlReceived(url, headers);
    return true;
}

bool SingleInstance::AddUrls(const QStringList& urls, const QVariantMap& headers)
{
    if (urlsHandler_) {
        return urlsHandler_(urls, headers);
    }
    emit urlsReceived(urls, headers);
    return true;
}

bool SingleInstance::AddDownloads(const QVariantList& downloads)
{
    return downloadsHandler_ ? downloadsHandler_(downloads) : false;
}

bool SingleInstance::AddDownloadsJson(const QString& downloadsJson)
{
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(downloadsJson.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !document.isArray()) {
        return false;
    }
    return AddDownloads(document.array().toVariantList());
}

}
