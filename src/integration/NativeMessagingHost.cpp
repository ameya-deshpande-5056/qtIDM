#include "app/Logger.h"
#include "app/Paths.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QThread>
#include <QUrl>
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
    return std::cin ? payload : QByteArray {};
}

void writeNativeMessage(const QJsonObject& object)
{
    const auto payload = QJsonDocument(object).toJson(QJsonDocument::Compact);
    const auto size = static_cast<quint32>(payload.size());
    std::cout.write(reinterpret_cast<const char*>(&size), sizeof(size));
    std::cout.write(payload.constData(), payload.size());
    std::cout.flush();
}

int fail(const QString& code, const QString& message)
{
    qtidm::Logger::error(QStringLiteral("NativeMessagingHost"), code + QStringLiteral(": ") + message);
    writeNativeMessage({ { QStringLiteral("ok"), false }, { QStringLiteral("code"), code }, { QStringLiteral("message"), message } });
    // A valid protocol response must exit successfully or Chromium may discard
    // it and replace the useful error with a generic native-host failure.
    return 0;
}

QString executablePath()
{
    const auto sibling = QCoreApplication::applicationDirPath() + QStringLiteral("/qtIDM");
    if (QFileInfo::exists(sibling)) {
        return sibling;
    }
    return QStandardPaths::findExecutable(QStringLiteral("qtIDM"));
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    qtidm::Paths::ensureRuntimeDirs();
    qtidm::Logger::install();
    const auto payload = readNativeMessage();
    QJsonParseError parseError;
    const auto doc = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        return fail(QStringLiteral("invalid-message"), QStringLiteral("Browser sent an invalid native-messaging request."));
    }
    const auto object = doc.object();
    QStringList urls;
    if (object.value(QStringLiteral("urls")).isArray()) {
        const auto values = object.value(QStringLiteral("urls")).toArray();
        if (values.size() > 100) {
            return fail(QStringLiteral("too-many-urls"), QStringLiteral("A batch can contain at most 100 URLs."));
        }
        for (const auto& value : values) {
            urls.append(value.toString());
        }
    } else {
        urls.append(object.value(QStringLiteral("url")).toString());
    }
    if (urls.isEmpty()) {
        return fail(QStringLiteral("invalid-url"), QStringLiteral("No download URLs were supplied."));
    }
    for (const auto& url : urls) {
        const QUrl parsedUrl(url);
        if (!parsedUrl.isValid() || (parsedUrl.scheme() != QStringLiteral("http") && parsedUrl.scheme() != QStringLiteral("https"))) {
            return fail(QStringLiteral("invalid-url"), QStringLiteral("qtIDM can redirect only valid HTTP or HTTPS download URLs."));
        }
    }

    QVariantMap headers;
    const auto headerObject = object.value(QStringLiteral("headers")).toObject();
    for (auto it = headerObject.begin(); it != headerObject.end(); ++it) {
        const auto value = it.value().toString();
        if (it.key().contains(QLatin1Char('\n')) || it.key().contains(QLatin1Char('\r'))
            || value.contains(QLatin1Char('\n')) || value.contains(QLatin1Char('\r'))) {
            return fail(QStringLiteral("invalid-headers"), QStringLiteral("Browser sent unsafe request headers."));
        }
        headers.insert(it.key(), value);
    }
    const auto mediaType = object.value(QStringLiteral("mediaType")).toString().trimmed().toUpper();
    if (!mediaType.isEmpty() && mediaType != QStringLiteral("HLS") && mediaType != QStringLiteral("DASH")) {
        return fail(QStringLiteral("invalid-media-type"), QStringLiteral("Browser sent an unsupported media type."));
    }
    if (!mediaType.isEmpty()) {
        headers.insert(QStringLiteral("_qtidmMediaType"), mediaType);
    }
    const auto suggestedFilename = object.value(QStringLiteral("suggestedFilename")).toString().trimmed();
    if (suggestedFilename.size() > 4096 || suggestedFilename.contains(QChar::Null)
        || suggestedFilename.contains(QLatin1Char('\n')) || suggestedFilename.contains(QLatin1Char('\r'))) {
        return fail(QStringLiteral("invalid-filename"), QStringLiteral("Browser sent an unsafe suggested filename."));
    }
    if (!suggestedFilename.isEmpty() && urls.size() == 1) {
        headers.insert(QStringLiteral("_qtidmSuggestedFilename"), suggestedFilename);
    }
    bool launchAttempted = false;
    QString lastError;
    for (int attempt = 0; attempt < 50; ++attempt) {
        QDBusInterface iface(QStringLiteral("io.github.qtidm"),
                             QStringLiteral("/io/github/qtidm/Application"),
                             QStringLiteral("io.github.qtidm.Application"),
                             QDBusConnection::sessionBus());
        if (iface.isValid()) {
            const auto reply = urls.size() == 1
                ? iface.call(QStringLiteral("AddUrl"), urls.first(), headers)
                : iface.call(QStringLiteral("AddUrls"), urls, headers);
            if (reply.type() != QDBusMessage::ErrorMessage) {
                qtidm::Logger::info(QStringLiteral("NativeMessagingHost"), QStringLiteral("Delivered browser download request to qtIDM."));
                writeNativeMessage({ { QStringLiteral("ok"), true } });
                return 0;
            }
            lastError = reply.errorMessage();
        } else {
            lastError = QStringLiteral("qtIDM D-Bus service is not available yet.");
        }
        if (!launchAttempted) {
            launchAttempted = true;
            const auto executable = executablePath();
            if (executable.isEmpty() || !QProcess::startDetached(executable)) {
                return fail(QStringLiteral("launch-failed"), QStringLiteral("Could not start qtIDM. Reinstall the qtIDM package and try again."));
            }
        }
        QThread::msleep(100);
    }
    return fail(QStringLiteral("application-unavailable"),
                QStringLiteral("qtIDM started but did not accept the download request: %1").arg(lastError));
}
