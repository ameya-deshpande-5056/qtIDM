#pragma once

#include <QObject>
#include <functional>
#include <QVariantMap>
#include <QVariantList>

#include <memory>
#include <QLockFile>

namespace qtidm {

class SingleInstance final : public QObject {
    Q_OBJECT
#ifndef Q_OS_WIN
    Q_CLASSINFO("D-Bus Interface", "io.qtidm.Qtidm.Application")
#endif
public:
    explicit SingleInstance(QObject* parent = nullptr);
    bool acquire();
    bool notifyExistingInstance(const QStringList& urls);
    void setUrlHandler(std::function<bool(QString, QVariantMap)> handler);
    void setUrlsHandler(std::function<bool(QStringList, QVariantMap)> handler);
    void setDownloadsHandler(std::function<bool(QVariantList)> handler);

#ifndef Q_OS_WIN
public slots:
    Q_SCRIPTABLE void Activate();
    Q_SCRIPTABLE bool AddUrl(const QString& url, const QVariantMap& headers);
    Q_SCRIPTABLE bool AddUrls(const QStringList& urls, const QVariantMap& headers);
    Q_SCRIPTABLE bool AddDownloads(const QVariantList& downloads);
    Q_SCRIPTABLE bool AddDownloadsJson(const QString& downloadsJson);
#endif

signals:
    void activateRequested();
    void urlReceived(QString url, QVariantMap headers);
    void urlsReceived(QStringList urls, QVariantMap headers);

private:
    std::function<bool(QString, QVariantMap)> urlHandler_;
    std::function<bool(QStringList, QVariantMap)> urlsHandler_;
    std::function<bool(QVariantList)> downloadsHandler_;
#ifndef Q_OS_WIN
    // D-Bus slots implementation provided in SingleInstance.cpp
#else
    std::unique_ptr<QLockFile> lockFile_;
#endif
};

}