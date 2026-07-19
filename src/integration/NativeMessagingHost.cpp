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
    // POST bodies are encoded as base64, so allow a bounded 4 MiB binary body
    // plus protocol metadata instead of the former 1 MiB request envelope.
    if (!std::cin || size == 0 || size > 8 * 1024 * 1024) return {};
    QByteArray payload(static_cast<qsizetype>(size), Qt::Uninitialized);
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

QJsonObject errorReply(const QString& code, const QString& message)
{
    qtidm::Logger::error(QStringLiteral("NativeMessagingHost"), code + QStringLiteral(": ") + message);
    return { { QStringLiteral("ok"), false }, { QStringLiteral("code"), code }, { QStringLiteral("message"), message } };
}

QString executablePath()
{
    const auto sibling = QCoreApplication::applicationDirPath() + QStringLiteral("/qtIDM");
    return QFileInfo::exists(sibling) ? sibling : QStandardPaths::findExecutable(QStringLiteral("qtIDM"));
}

bool validUrl(const QString& value)
{
    const QUrl url(value);
    return url.isValid() && (url.scheme() == QStringLiteral("http") || url.scheme() == QStringLiteral("https") || url.scheme() == QStringLiteral("ftp"));
}

QJsonObject handle(const QJsonObject& object)
{
    if (object.value(QStringLiteral("type")).toString() == QStringLiteral("status"))
        return { { QStringLiteral("ok"), true }, { QStringLiteral("status"), QStringLiteral("ready") }, { QStringLiteral("persistent"), true } };
    QVariantList downloads;
    QStringList urls;
    QString downloadsJson;
    if (object.value(QStringLiteral("downloads")).isArray()) {
        const auto values = object.value(QStringLiteral("downloads")).toArray();
        if (values.isEmpty() || values.size() > 100) return errorReply(QStringLiteral("invalid-downloads"), QStringLiteral("A browser batch must contain between 1 and 100 downloads."));
        for (const auto& value : values) {
            const auto item = value.toObject();
            const auto url = item.value(QStringLiteral("url")).toString();
            if (!validUrl(url)) return errorReply(QStringLiteral("invalid-url"), QStringLiteral("qtIDM can redirect only valid HTTP, HTTPS, or FTP download URLs."));
            QVariantMap headers;
            const auto headerObject = item.value(QStringLiteral("headers")).toObject();
            for (auto it = headerObject.begin(); it != headerObject.end(); ++it) {
                const auto header = it.value().toString();
                if (it.key().contains(QLatin1Char('\n')) || it.key().contains(QLatin1Char('\r')) || header.contains(QLatin1Char('\n')) || header.contains(QLatin1Char('\r')))
                    return errorReply(QStringLiteral("invalid-headers"), QStringLiteral("Browser sent unsafe request headers."));
                headers.insert(it.key(), header);
            }
            const auto method = item.value(QStringLiteral("method")).toString().trimmed().toUpper();
            const auto body = item.value(QStringLiteral("body")).toString().toLatin1();
            if (!method.isEmpty() && method != QStringLiteral("POST")) return errorReply(QStringLiteral("invalid-method"), QStringLiteral("Browser sent an unsupported HTTP method."));
            const auto decodedBody = QByteArray::fromBase64(body);
            if (body.size() > 6 * 1024 * 1024 || decodedBody.size() > 4 * 1024 * 1024
                || (!body.isEmpty() && decodedBody.isEmpty())) return errorReply(QStringLiteral("invalid-body"), QStringLiteral("Browser sent an invalid or oversized POST body."));
            downloads.append(QVariantMap { { QStringLiteral("url"), url }, { QStringLiteral("headers"), headers }, { QStringLiteral("method"), method }, { QStringLiteral("body"), QString::fromLatin1(body) }, { QStringLiteral("suggestedFilename"), item.value(QStringLiteral("suggestedFilename")).toString().trimmed() } });
        }
        // Nested QVariant maps can arrive from D-Bus as opaque QDBusArgument
        // wrappers. JSON uses an unambiguous string signature and preserves
        // the browser payload that was validated above.
        downloadsJson = QString::fromUtf8(QJsonDocument(values).toJson(QJsonDocument::Compact));
    } else if (object.value(QStringLiteral("urls")).isArray()) {
        for (const auto& value : object.value(QStringLiteral("urls")).toArray()) urls.append(value.toString());
    } else if (!object.value(QStringLiteral("prepare")).toBool()) urls.append(object.value(QStringLiteral("url")).toString());
    if (!object.value(QStringLiteral("prepare")).toBool() && urls.isEmpty() && downloads.isEmpty()) return errorReply(QStringLiteral("invalid-url"), QStringLiteral("No download URLs were supplied."));
    for (const auto& url : urls) if (!validUrl(url)) return errorReply(QStringLiteral("invalid-url"), QStringLiteral("qtIDM can redirect only valid HTTP, HTTPS, or FTP download URLs."));

    QString lastError;
    bool launched = false;
    for (int attempt = 0; attempt < 50; ++attempt) {
        QDBusInterface iface(QStringLiteral("io.qtidm.Qtidm"), QStringLiteral("/io/qtidm/Qtidm/Application"), QStringLiteral("io.qtidm.Qtidm.Application"), QDBusConnection::sessionBus());
        if (iface.isValid()) {
            if (object.value(QStringLiteral("prepare")).toBool()) return { { QStringLiteral("ok"), true }, { QStringLiteral("prepared"), true }, { QStringLiteral("persistent"), true } };
            const auto reply = !downloads.isEmpty() ? iface.call(QStringLiteral("AddDownloadsJson"), downloadsJson) : (urls.size() == 1 ? iface.call(QStringLiteral("AddUrl"), urls.first(), QVariantMap {}) : iface.call(QStringLiteral("AddUrls"), urls, QVariantMap {}));
            if (reply.type() != QDBusMessage::ErrorMessage) return { { QStringLiteral("ok"), true }, { QStringLiteral("accepted"), reply.arguments().value(0).toBool() } };
            lastError = reply.errorMessage();
        } else lastError = QStringLiteral("qtIDM D-Bus service is not available yet.");
        if (!launched) {
            launched = true;
            const auto executable = executablePath();
            if (executable.isEmpty() || !QProcess::startDetached(executable)) return errorReply(QStringLiteral("launch-failed"), QStringLiteral("Could not start qtIDM. Reinstall the qtIDM package and try again."));
        }
        QThread::msleep(100);
    }
    return errorReply(QStringLiteral("application-unavailable"), QStringLiteral("qtIDM started but did not accept the download request: %1").arg(lastError));
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    qtidm::Paths::ensureRuntimeDirs();
    qtidm::Logger::install();
    // Native messaging ports are streams, not one-request processes.  Keep the
    // host alive and correlate every response so browsers can safely pipeline.
    while (true) {
        const auto payload = readNativeMessage();
        if (payload.isEmpty()) break;
        QJsonParseError parseError;
        const auto doc = QJsonDocument::fromJson(payload, &parseError);
        QJsonObject reply = parseError.error == QJsonParseError::NoError && doc.isObject()
            ? handle(doc.object()) : errorReply(QStringLiteral("invalid-message"), QStringLiteral("Browser sent an invalid native-messaging request."));
        if (doc.isObject() && doc.object().contains(QStringLiteral("id"))) reply.insert(QStringLiteral("id"), doc.object().value(QStringLiteral("id")));
        writeNativeMessage(reply);
    }
    return 0;
}
