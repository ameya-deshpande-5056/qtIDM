#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <functional>
#include <QVariantMap>
#include <QVariantList>

namespace qtidm {

class SingleInstance final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "io.qtidm.Qtidm.Application")
public:
    explicit SingleInstance(QObject* parent = nullptr);
    bool acquire();
    bool notifyExistingInstance(const QStringList& urls);
    void setUrlHandler(std::function<bool(QString, QVariantMap)> handler);
    void setUrlsHandler(std::function<bool(QStringList, QVariantMap)> handler);
    void setDownloadsHandler(std::function<bool(QVariantList)> handler);
    void setSocialMediaHandler(std::function<bool(QString)> handler);
    // Automation verbs are answered outside this class so the D-Bus surface
    // stays free of engine and storage dependencies.
    using AutomationHandler = std::function<QString(const QString& verb, const QStringList& arguments)>;
    void setAutomationHandler(AutomationHandler handler);

public slots:
    Q_SCRIPTABLE void Activate();
    Q_SCRIPTABLE bool AddUrl(const QString& url, const QVariantMap& headers);
    Q_SCRIPTABLE bool AddUrls(const QStringList& urls, const QVariantMap& headers);
    Q_SCRIPTABLE bool AddDownloads(const QVariantList& downloads);
    Q_SCRIPTABLE bool AddDownloadsJson(const QString& downloadsJson);
    Q_SCRIPTABLE bool ExtractSocialMedia(const QString& url);
    Q_SCRIPTABLE QString ListDownloads();
    Q_SCRIPTABLE QString PauseDownloads(const QStringList& ids);
    Q_SCRIPTABLE QString StartDownloads(const QStringList& ids);
    Q_SCRIPTABLE QString RemoveDownloads(const QStringList& ids);
    Q_SCRIPTABLE QString DeleteDownloads(const QStringList& ids);
    Q_SCRIPTABLE QString StartQueue(const QString& queueName);
    Q_SCRIPTABLE QString StopQueue(const QString& queueName);

signals:
    void activateRequested();
    void urlReceived(QString url, QVariantMap headers);
    void urlsReceived(QStringList urls, QVariantMap headers);

private:
    std::function<bool(QString, QVariantMap)> urlHandler_;
    std::function<bool(QStringList, QVariantMap)> urlsHandler_;
    std::function<bool(QVariantList)> downloadsHandler_;
    std::function<bool(QString)> socialMediaHandler_;
    AutomationHandler automationHandler_;
};

}
