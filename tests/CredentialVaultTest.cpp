#include "integration/CredentialVault.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>

class CredentialVaultTest final : public QObject {
    Q_OBJECT
private slots:
    void storesLooksUpAndClearsWithoutCommandLineSecrets()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto storage = dir.path() + QStringLiteral("/secret");
        const auto arguments = dir.path() + QStringLiteral("/arguments");
        qputenv("QTIDM_FAKE_SECRET_STORAGE", storage.toUtf8());
        qputenv("QTIDM_FAKE_SECRET_ARGUMENTS", arguments.toUtf8());

        QFile tool(dir.path() + QStringLiteral("/secret-tool"));
        QVERIFY(tool.open(QIODevice::WriteOnly));
        tool.write(
            "#!/bin/sh\n"
            "printf '%s\\n' \"$*\" >> \"$QTIDM_FAKE_SECRET_ARGUMENTS\"\n"
            "case \"$1\" in\n"
            "  store) cat > \"$QTIDM_FAKE_SECRET_STORAGE\" ;;\n"
            "  lookup) cat \"$QTIDM_FAKE_SECRET_STORAGE\" ;;\n"
            "  clear) : > \"$QTIDM_FAKE_SECRET_STORAGE\" ;;\n"
            "esac\n");
        tool.close();
        QVERIFY(tool.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));

        qtidm::CredentialVault vault(tool.fileName());
        const QUrl url(QStringLiteral("https://secure.example.test/file"));
        const auto key = vault.keyFor(url, QStringLiteral("ameya"));
        QVERIFY(!key.isEmpty());
        QString error;
        QVERIFY2(vault.store(key, url, QStringLiteral("ameya"), QStringLiteral("top-secret"), &error),
            qPrintable(error));
        QCOMPARE(vault.lookup(key, &error), QStringLiteral("top-secret"));

        QFile argumentLog(arguments);
        QVERIFY(argumentLog.open(QIODevice::ReadOnly));
        QVERIFY(!argumentLog.readAll().contains("top-secret"));
        QVERIFY(vault.remove(key, &error));
        QVERIFY(vault.lookup(key, &error).isEmpty());
    }
};

QTEST_MAIN(CredentialVaultTest)
#include "CredentialVaultTest.moc"
