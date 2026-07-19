#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest/QtTest>

class PackagingMetadataTest final : public QObject {
    Q_OBJECT
private slots:
    void desktopFileUsesInstalledApplicationIcon()
    {
        const auto path = QStringLiteral(QTIDM_SOURCE_DIR) + QStringLiteral("/assets/io.qtidm.Qtidm.desktop");
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto data = QString::fromUtf8(file.readAll());
        QVERIFY(data.contains(QStringLiteral("Exec=qtIDM %u")));
        QVERIFY(data.contains(QStringLiteral("Icon=io.qtidm.Qtidm")));
        QVERIFY(data.contains(QStringLiteral("MimeType=x-scheme-handler/http;x-scheme-handler/https;x-scheme-handler/ftp;")));
    }

    void applicationIdentityIsConsistentlyIoQtidm()
    {
        const QStringList paths {
            QStringLiteral(QTIDM_SOURCE_DIR) + QStringLiteral("/assets/io.qtidm.Qtidm.desktop"),
            QStringLiteral(QTIDM_SOURCE_DIR) + QStringLiteral("/assets/io.qtidm.Qtidm.appdata.xml"),
            QStringLiteral(QTIDM_SOURCE_DIR) + QStringLiteral("/packaging/flatpak/io.qtidm.Qtidm.yml"),
            QStringLiteral(QTIDM_SOURCE_DIR) + QStringLiteral("/src/app/SingleInstance.cpp"),
            QStringLiteral(QTIDM_SOURCE_DIR) + QStringLiteral("/src/main.cpp")
        };
        for (const auto& path : paths) {
            QFile file(path);
            QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(path));
            const auto data = QString::fromUtf8(file.readAll());
            QVERIFY2(data.contains(QStringLiteral("io.qtidm.Qtidm")), qPrintable(path));
            QVERIFY2(!data.contains(QStringLiteral("io.github")), qPrintable(path));
        }
        QVERIFY(QFileInfo::exists(
            QStringLiteral(QTIDM_SOURCE_DIR) + QStringLiteral("/assets/io.qtidm.Qtidm.svg")));
        for (const int size : { 16, 32, 48, 64, 128, 256, 512 }) {
            QVERIFY(QFileInfo::exists(
                QStringLiteral(QTIDM_SOURCE_DIR)
                + QStringLiteral("/assets/icons/hicolor/%1x%1/apps/io.qtidm.Qtidm.png").arg(size)));
        }
    }

    void bundledActionIconsAreComplete()
    {
        QFile resourceFile(QStringLiteral(QTIDM_SOURCE_DIR) + QStringLiteral("/resources.qrc"));
        QVERIFY(resourceFile.open(QIODevice::ReadOnly));
        const auto resources = QString::fromUtf8(resourceFile.readAll());
        QVERIFY(resources.contains(QStringLiteral("alias=\"icons/application.png\"")));
        QVERIFY(!resources.contains(QStringLiteral("alias=\"icons/application.svg\"")));
        const QStringList names {
            QStringLiteral("add"), QStringLiteral("start"), QStringLiteral("pause"),
            QStringLiteral("stop"), QStringLiteral("delete"), QStringLiteral("edit"),
            QStringLiteral("queue"), QStringLiteral("import"), QStringLiteral("export"),
            QStringLiteral("links"), QStringLiteral("grabber"), QStringLiteral("social"),
            QStringLiteral("zip"), QStringLiteral("options"), QStringLiteral("about"),
            QStringLiteral("speed-limit")
        };
        for (const auto& name : names) {
            const auto sourcePath = QStringLiteral("assets/icons/actions/%1.svg").arg(name);
            const auto runtimePath = QStringLiteral("assets/icons/actions/png/%1.png").arg(name);
            QFile iconFile(QStringLiteral(QTIDM_SOURCE_DIR) + QLatin1Char('/') + sourcePath);
            QVERIFY2(iconFile.open(QIODevice::ReadOnly), qPrintable(sourcePath));
            const auto iconData = QString::fromUtf8(iconFile.readAll());
            QVERIFY2(!iconData.contains(QStringLiteral("#2457a6"), Qt::CaseInsensitive),
                     qPrintable(QStringLiteral("%1 reuses the retired royal-blue brand").arg(sourcePath)));
            QVERIFY2(!iconData.contains(QStringLiteral("#6d2f45"), Qt::CaseInsensitive),
                     qPrintable(QStringLiteral("%1 reuses the application brand color").arg(sourcePath)));
            QVERIFY2(QFileInfo::exists(
                         QStringLiteral(QTIDM_SOURCE_DIR) + QLatin1Char('/') + runtimePath),
                     qPrintable(runtimePath));
            QVERIFY2(resources.contains(runtimePath), qPrintable(runtimePath));
        }
    }

    void browserNativeManifestsAreValidJson()
    {
        const QStringList paths {
            QStringLiteral(QTIDM_BINARY_DIR) + QStringLiteral("/generated/browser/native/io.qtidm.native.chrome.json"),
            QStringLiteral(QTIDM_BINARY_DIR) + QStringLiteral("/generated/browser/native/io.qtidm.native.firefox.json")
        };
        for (const auto& path : paths) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::ReadOnly));
            const auto doc = QJsonDocument::fromJson(file.readAll());
            QVERIFY(doc.isObject());
            QCOMPARE(doc.object().value(QStringLiteral("name")).toString(), QStringLiteral("io.qtidm.native"));
            QCOMPARE(doc.object().value(QStringLiteral("type")).toString(), QStringLiteral("stdio"));
            QVERIFY(doc.object().value(QStringLiteral("path")).toString().endsWith(QStringLiteral("qtIDM-native-host")));
        }
    }

    void browserNativeManifestsUseDiscoverableInstallNames()
    {
        QFile file(QStringLiteral(QTIDM_BINARY_DIR) + QStringLiteral("/cmake_install.cmake"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto data = QString::fromUtf8(file.readAll());
        QVERIFY(data.contains(QStringLiteral("/lib/mozilla/native-messaging-hosts")));
        QVERIFY(data.contains(QStringLiteral("/etc/opt/chrome/native-messaging-hosts")));
        QVERIFY(data.contains(QStringLiteral("/etc/chromium/native-messaging-hosts")));
        QCOMPARE(data.count(QStringLiteral("RENAME \"io.qtidm.native.json\"")), 3);
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
            const auto root = QFileInfo(path).absolutePath();
            const auto action = doc.object().value(
                doc.object().value(QStringLiteral("manifest_version")).toInt() == 3
                    ? QStringLiteral("action") : QStringLiteral("browser_action")).toObject();
            QCOMPARE(action.value(QStringLiteral("default_popup")).toString(), QStringLiteral("popup.html"));
            const auto icons = doc.object().value(QStringLiteral("icons")).toObject();
            const auto actionIcons = action.value(QStringLiteral("default_icon")).toObject();
            for (const auto& size : { QStringLiteral("16"), QStringLiteral("32"),
                                     QStringLiteral("48"), QStringLiteral("128") }) {
                QCOMPARE(icons.value(size).toString(), QStringLiteral("icons/icon-%1.png").arg(size));
                QCOMPARE(actionIcons.value(size).toString(), icons.value(size).toString());
                QVERIFY(QFileInfo::exists(root + QLatin1Char('/') + icons.value(size).toString()));
            }
            QVERIFY(QFileInfo::exists(root + QStringLiteral("/popup.html")));
            QVERIFY(QFileInfo::exists(root + QStringLiteral("/popup.css")));
            QVERIFY(QFileInfo::exists(root + QStringLiteral("/popup.js")));
            QFile popupStyles(root + QStringLiteral("/popup.css"));
            QVERIFY(popupStyles.open(QIODevice::ReadOnly));
            const auto css = QString::fromUtf8(popupStyles.readAll());
            QVERIFY(css.contains(QStringLiteral("color-scheme: dark")));
            QVERIFY(css.contains(QStringLiteral("--brand: #6d2f45")));
            QVERIFY(!css.contains(QStringLiteral("#2457a6"), Qt::CaseInsensitive));
            QVERIFY(!css.contains(QStringLiteral("#f5f2e9")));
            QVERIFY(!css.contains(QStringLiteral("#087f5b")));
        }
    }
};

QTEST_MAIN(PackagingMetadataTest)
#include "PackagingMetadataTest.moc"
