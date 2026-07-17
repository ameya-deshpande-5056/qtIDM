#pragma once

#include <QObject>
#include <QVariantMap>

namespace qtidm {

class SingleInstance final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "io.github.qtidm.Application")
public:
    explicit SingleInstance(QObject* parent = nullptr);
    bool acquire();
    bool notifyExistingInstance(const QStringList& urls);

public slots:
    Q_SCRIPTABLE void Activate();
    Q_SCRIPTABLE void AddUrl(const QString& url, const QVariantMap& headers);
    Q_SCRIPTABLE void AddUrls(const QStringList& urls, const QVariantMap& headers);

signals:
    void activateRequested();
    void urlReceived(QString url, QVariantMap headers);
    void urlsReceived(QStringList urls, QVariantMap headers);
};

}
