#pragma once

#include <QObject>
#include <functional>
#include <QVariantMap>
#include <QVariantList>

namespace qtidm {

class SingleInstance final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "io.github.qtidm.Application")
public:
    explicit SingleInstance(QObject* parent = nullptr);
    bool acquire();
    bool notifyExistingInstance(const QStringList& urls);
    void setUrlHandler(std::function<bool(QString, QVariantMap)> handler);
    void setUrlsHandler(std::function<bool(QStringList, QVariantMap)> handler);
    void setDownloadsHandler(std::function<bool(QVariantList)> handler);

public slots:
    Q_SCRIPTABLE void Activate();
    Q_SCRIPTABLE bool AddUrl(const QString& url, const QVariantMap& headers);
    Q_SCRIPTABLE bool AddUrls(const QStringList& urls, const QVariantMap& headers);
    Q_SCRIPTABLE bool AddDownloads(const QVariantList& downloads);
    Q_SCRIPTABLE bool AddDownloadsJson(const QString& downloadsJson);

signals:
    void activateRequested();
    void urlReceived(QString url, QVariantMap headers);
    void urlsReceived(QStringList urls, QVariantMap headers);

private:
    std::function<bool(QString, QVariantMap)> urlHandler_;
    std::function<bool(QStringList, QVariantMap)> urlsHandler_;
    std::function<bool(QVariantList)> downloadsHandler_;
};

}
