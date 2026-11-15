#include "app/SingleInstance.h"

#include "app/AutomationNames.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QJsonArray>
#include <QJsonDocument>

namespace {
constexpr auto objectPath = qtidm::automation::objectPath;
constexpr auto interfaceName = qtidm::automation::interface;

QString unavailableAutomation()
{
    return QStringLiteral("{\"error\":\"The qtIDM automation API is not available in this process.\","
                          "\"code\":\"unavailable\"}");
}
}

namespace qtidm {

SingleInstance::SingleInstance(QString serviceName, QObject* parent)
    : QObject(parent)
    , serviceName_(serviceName.isEmpty() ? QString::fromLatin1(qtidm::automation::service)
                                         : std::move(serviceName))
{
}

bool SingleInstance::acquire()
{
    auto bus = QDBusConnection::sessionBus();
    if (!bus.registerService(serviceName_)) {
        return false;
    }
    return bus.registerObject(QString::fromLatin1(objectPath), this, QDBusConnection::ExportAllSlots);
}

bool SingleInstance::notifyExistingInstance(const QStringList& urls)
{
    QDBusInterface iface(serviceName_,
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

void SingleInstance::setSocialMediaHandler(std::function<bool(QString)> handler)
{
    socialMediaHandler_ = std::move(handler);
}

void SingleInstance::setAutomationHandler(AutomationHandler handler)
{
    automationHandler_ = std::move(handler);
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

bool SingleInstance::ExtractSocialMedia(const QString& url)
{
    return socialMediaHandler_ ? socialMediaHandler_(url) : false;
}

QString SingleInstance::ListDownloads()
{
    return automationHandler_ ? automationHandler_(QStringLiteral("list"), {}) : unavailableAutomation();
}

QString SingleInstance::PauseDownloads(const QStringList& ids)
{
    return automationHandler_ ? automationHandler_(QStringLiteral("pause"), ids) : unavailableAutomation();
}

QString SingleInstance::StartDownloads(const QStringList& ids)
{
    return automationHandler_ ? automationHandler_(QStringLiteral("start"), ids) : unavailableAutomation();
}

QString SingleInstance::RemoveDownloads(const QStringList& ids)
{
    return automationHandler_ ? automationHandler_(QStringLiteral("remove"), ids) : unavailableAutomation();
}

QString SingleInstance::DeleteDownloads(const QStringList& ids)
{
    return automationHandler_ ? automationHandler_(QStringLiteral("delete"), ids) : unavailableAutomation();
}

QString SingleInstance::StartQueue(const QString& queueName)
{
    return automationHandler_ ? automationHandler_(QStringLiteral("start-queue"), { queueName })
                              : unavailableAutomation();
}

QString SingleInstance::StopQueue(const QString& queueName)
{
    return automationHandler_ ? automationHandler_(QStringLiteral("stop-queue"), { queueName })
                              : unavailableAutomation();
}

}
