#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QThread>
#include <QVariantMap>
#include <iostream>

namespace {
QByteArray readNativeMessage()
{
    quint32 size = 0;
    std::cin.read(reinterpret_cast<char*>(&size), sizeof(size));
    if (!std::cin || size == 0 || size > 1024 * 1024) {
        return {};
    }
    QByteArray payload;
    payload.resize(static_cast<qsizetype>(size));
    std::cin.read(payload.data(), size);
    return payload;
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const auto payload = readNativeMessage();
    const auto doc = QJsonDocument::fromJson(payload);
    const auto object = doc.object();
    const auto url = object.value(QStringLiteral("url")).toString();
    if (url.isEmpty()) {
        return 1;
    }

    QVariantMap headers;
    const auto headerObject = object.value(QStringLiteral("headers")).toObject();
    for (auto it = headerObject.begin(); it != headerObject.end(); ++it) {
        headers.insert(it.key(), it.value().toVariant());
    }
    for (int attempt = 0; attempt < 50; ++attempt) {
        QDBusInterface iface(QStringLiteral("io.github.qtidm"),
                             QStringLiteral("/io/github/qtidm/Application"),
                             QStringLiteral("io.github.qtidm.Application"),
                             QDBusConnection::sessionBus());
        if (iface.isValid()) {
            const auto reply = iface.call(QStringLiteral("AddUrl"), url, headers);
            if (reply.type() != QDBusMessage::ErrorMessage) {
                return 0;
            }
        }
        if (attempt == 0) {
            QProcess::startDetached(QStringLiteral("qtIDM"));
        }
        QThread::msleep(100);
    }
    return 2;
}
