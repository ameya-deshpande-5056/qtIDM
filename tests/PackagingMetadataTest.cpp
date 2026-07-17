#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest/QtTest>

class PackagingMetadataTest final : public QObject {
    Q_OBJECT
private slots:
    void desktopFileUsesInstalledApplicationIcon()
    {
        const auto path = QStringLiteral(QTIDM_SOURCE_DIR) + QStringLiteral("/assets/io.github.qtidm.qtidm.desktop");
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto data = QString::fromUtf8(file.readAll());
        QVERIFY(data.contains(QStringLiteral("Exec=qtIDM %u")));
        QVERIFY(data.contains(QStringLiteral("Icon=io.github.qtidm.qtidm")));
        QVERIFY(data.contains(QStringLiteral("MimeType=x-scheme-handler/http;x-scheme-handler/https;x-scheme-handler/ftp;")));
    }

    void browserNativeManifestsAreValidJson()
    {
        const QStringList paths {
            QStringLiteral(QTIDM_SOURCE_DIR) + QStringLiteral("/browser/native/io.github.qtidm.native.chrome.json"),
            QStringLiteral(QTIDM_SOURCE_DIR) + QStringLiteral("/browser/native/io.github.qtidm.native.firefox.json")
        };
        for (const auto& path : paths) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::ReadOnly));
            const auto doc = QJsonDocument::fromJson(file.readAll());
            QVERIFY(doc.isObject());
            QCOMPARE(doc.object().value(QStringLiteral("name")).toString(), QStringLiteral("io.github.qtidm.native"));
            QCOMPARE(doc.object().value(QStringLiteral("type")).toString(), QStringLiteral("stdio"));
            QVERIFY(doc.object().value(QStringLiteral("path")).toString().endsWith(QStringLiteral("qtIDM-native-host")));
        }
    }

    void browserExtensionManifestsAreValidJson()
    {
        const QStringList paths {
            QStringLiteral(QTIDM_SOURCE_DIR) + QStringLiteral("/browser/chrome/manifest.json"),
            QStringLiteral(QTIDM_SOURCE_DIR) + QStringLiteral("/browser/firefox/manifest.json")
        };
        for (const auto& path : paths) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::ReadOnly));
            const auto doc = QJsonDocument::fromJson(file.readAll());
            QVERIFY(doc.isObject());
            QCOMPARE(doc.object().value(QStringLiteral("name")).toString(), QStringLiteral("qtIDM Integration"));
        }
    }
};

QTEST_MAIN(PackagingMetadataTest)
#include "PackagingMetadataTest.moc"
