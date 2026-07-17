#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QJsonDocument>
#include <QJsonObject>
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

    QDBusInterface iface(QStringLiteral("io.github.qtidm"),
                         QStringLiteral("/io/github/qtidm/Application"),
                         QStringLiteral("io.github.qtidm.Application"),
                         QDBusConnection::sessionBus());
    QVariantMap headers;
    const auto headerObject = object.value(QStringLiteral("headers")).toObject();
    for (auto it = headerObject.begin(); it != headerObject.end(); ++it) {
        headers.insert(it.key(), it.value().toVariant());
    }
    iface.call(QStringLiteral("AddUrl"), url, headers);
    return 0;
}
