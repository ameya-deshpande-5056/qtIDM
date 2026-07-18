#include "integration/CredentialVault.h"

#include <QCryptographicHash>
#include <QProcess>
#include <QStandardPaths>

namespace qtidm {

CredentialVault::CredentialVault(QString executable)
    : executable_(std::move(executable))
{
}

QString CredentialVault::executable() const
{
    return executable_.isEmpty()
        ? QStandardPaths::findExecutable(QStringLiteral("secret-tool"))
        : executable_;
}

bool CredentialVault::isAvailable() const
{
    return !executable().isEmpty();
}

QString CredentialVault::keyFor(const QUrl& url, const QString& username) const
{
    const auto scope = url.scheme().toLower() + QStringLiteral("://")
        + url.host().toLower() + QLatin1Char(':') + QString::number(url.port(-1))
        + QLatin1Char('|') + username;
    return QString::fromLatin1(QCryptographicHash::hash(scope.toUtf8(), QCryptographicHash::Sha256).toHex());
}

bool CredentialVault::run(const QStringList& arguments, const QByteArray& input,
    QByteArray* output, QString* error) const
{
    const auto tool = executable();
    if (tool.isEmpty()) {
        if (error) {
            *error = QStringLiteral("Secret Service support requires the secret-tool command.");
        }
        return false;
    }
    QProcess process;
    process.setProgram(tool);
    process.setArguments(arguments);
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start();
    if (!process.waitForStarted(5000)) {
        if (error) {
            *error = process.errorString();
        }
        return false;
    }
    if (!input.isEmpty()) {
        process.write(input);
    }
    process.closeWriteChannel();
    if (!process.waitForFinished(30000)) {
        process.kill();
        process.waitForFinished(1000);
        if (error) {
            *error = QStringLiteral("The system credential vault timed out.");
        }
        return false;
    }
    if (output) {
        *output = process.readAllStandardOutput();
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (error) {
            const auto diagnostics = QString::fromUtf8(process.readAllStandardError()).trimmed();
            *error = diagnostics.isEmpty() ? QStringLiteral("The system credential vault rejected the request.")
                                           : diagnostics.right(1000);
        }
        return false;
    }
    return true;
}

bool CredentialVault::store(const QString& key, const QUrl& url, const QString& username,
    const QString& password, QString* error) const
{
    if (key.isEmpty() || password.isEmpty()) {
        if (error) {
            *error = QStringLiteral("A credential key and password are required.");
        }
        return false;
    }
    return run({
                   QStringLiteral("store"),
                   QStringLiteral("--label=qtIDM credential for %1").arg(url.host()),
                   QStringLiteral("application"), QStringLiteral("qtidm"),
                   QStringLiteral("key"), key,
                   QStringLiteral("host"), url.host().toLower(),
                   QStringLiteral("username"), username
               },
        password.toUtf8(), nullptr, error);
}

QString CredentialVault::lookup(const QString& key, QString* error) const
{
    QByteArray output;
    if (!run({
                 QStringLiteral("lookup"),
                 QStringLiteral("application"), QStringLiteral("qtidm"),
                 QStringLiteral("key"), key
             },
            {}, &output, error)) {
        return {};
    }
    while (output.endsWith('\n') || output.endsWith('\r')) {
        output.chop(1);
    }
    return QString::fromUtf8(output);
}

bool CredentialVault::remove(const QString& key, QString* error) const
{
    return run({
                   QStringLiteral("clear"),
                   QStringLiteral("application"), QStringLiteral("qtidm"),
                   QStringLiteral("key"), key
               },
        {}, nullptr, error);
}

}
