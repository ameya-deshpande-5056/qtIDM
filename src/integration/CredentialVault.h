#pragma once

#include <QString>
#include <QUrl>

namespace qtidm {

class CredentialVault final {
public:
    explicit CredentialVault(QString executable = {});

    bool isAvailable() const;
    QString keyFor(const QUrl& url, const QString& username) const;
    bool store(const QString& key, const QUrl& url, const QString& username,
        const QString& password, QString* error = nullptr) const;
    QString lookup(const QString& key, QString* error = nullptr) const;
    bool remove(const QString& key, QString* error = nullptr) const;

private:
    QString executable() const;
    bool run(const QStringList& arguments, const QByteArray& input,
        QByteArray* output, QString* error) const;

    QString executable_;
};

}
