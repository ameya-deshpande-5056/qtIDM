#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
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
            // The D-Bus service name lives in one place now that qtidm-cli and
            // the discovery file must agree with it.
            QStringLiteral(QTIDM_SOURCE_DIR) + QStringLiteral("/src/app/AutomationNames.h"),
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
            QStringLiteral("zip"), QStringLiteral("options"), QStringLiteral("updates"),
            QStringLiteral("about"), QStringLiteral("speed-limit"),
            QStringLiteral("speed-limit-active")
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

    void distroPackagingTracksTheProjectVersion()
    {
        QFile cmakeFile(QStringLiteral(QTIDM_SOURCE_DIR) + QStringLiteral("/CMakeLists.txt"));
        QVERIFY(cmakeFile.open(QIODevice::ReadOnly));
        const auto cmakeData = QString::fromUtf8(cmakeFile.readAll());
        const QRegularExpression projectExpression(
            QStringLiteral("project\\(qtIDM VERSION ([0-9]+\\.[0-9]+\\.[0-9]+)"));
        const auto match = projectExpression.match(cmakeData);
        QVERIFY(match.hasMatch());
        const auto version = match.captured(1);

        // The RPM and Arch scripts rewrite these at build time; the checked-in
        // values must still follow the project version for anyone building
        // directly with rpmbuild or makepkg.
        QFile spec(QStringLiteral(QTIDM_SOURCE_DIR) + QStringLiteral("/packaging/rpm/qtidm.spec"));
        QVERIFY(spec.open(QIODevice::ReadOnly));
        const auto specData = QString::fromUtf8(spec.readAll());
        QVERIFY2(specData.contains(QStringLiteral("Version:        %1").arg(version)),
                 qPrintable(version));

        QFile pkgbuild(QStringLiteral(QTIDM_SOURCE_DIR) + QStringLiteral("/packaging/arch/PKGBUILD"));
        QVERIFY(pkgbuild.open(QIODevice::ReadOnly));
        const auto pkgbuildData = QString::fromUtf8(pkgbuild.readAll());
        QVERIFY2(pkgbuildData.contains(QStringLiteral("pkgver=%1").arg(version)),
                 qPrintable(version));
    }

    void distroPackagingSupportsBothArchitectures()
    {
        // Debian, Fedora, Arch, Flatpak and AppImage package amd64 and arm64;
        // the scripts pick the host architecture by default and take --arch.
        const auto read = [](const QString& path) {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) {
                return QString();
            }
            return QString::fromUtf8(file.readAll());
        };
        const auto source = QStringLiteral(QTIDM_SOURCE_DIR) + QLatin1Char('/');

        const auto spec = read(source + QStringLiteral("packaging/rpm/qtidm.spec"));
        QVERIFY(spec.contains(QStringLiteral("ExclusiveArch:  x86_64 aarch64")));
        QVERIFY(spec.contains(QStringLiteral("qtidm_toolchain")));

        const auto pkgbuild = read(source + QStringLiteral("packaging/arch/PKGBUILD"));
        QVERIFY(pkgbuild.contains(QStringLiteral("arch=('x86_64' 'aarch64')")));
        QVERIFY(pkgbuild.contains(QStringLiteral("QTIDM_CMAKE_TOOLCHAIN_FILE")));

        const auto debianControl = read(source + QStringLiteral("packaging/debian/control"));
        QVERIFY(debianControl.contains(QStringLiteral("Architecture: any")));

        const auto flatpak = read(source + QStringLiteral("packaging/flatpak/io.qtidm.Qtidm.yml"));
        QVERIFY(flatpak.contains(QStringLiteral("x86_64")));
        QVERIFY(flatpak.contains(QStringLiteral("aarch64")));
        QVERIFY(flatpak.contains(QStringLiteral("name: deno")));

        const auto appimageScript = read(source + QStringLiteral("packaging/build-appimage.sh"));
        QVERIFY(appimageScript.contains(QStringLiteral("cp -L \"$DENO\" \"$APPDIR/usr/bin/deno\"")));

        for (const auto& script : { QStringLiteral("packaging/build-rpm.sh"),
                                    QStringLiteral("packaging/build-arch.sh"),
                                    QStringLiteral("packaging/build-appimage.sh") }) {
            const auto data = read(source + script);
            QVERIFY2(data.contains(QStringLiteral("--arch")), qPrintable(script));
            QVERIFY2(data.contains(QStringLiteral("--toolchain")), qPrintable(script));
            QVERIFY2(data.contains(QStringLiteral("packaging_normalize_arch")), qPrintable(script));
        }

        for (const auto& script : { QStringLiteral("packaging/build-deb.sh"),
                                    QStringLiteral("packaging/build-flatpak.sh"),
                                    QStringLiteral("packaging/release/build-flatpak-bundle.sh") }) {
            const auto data = read(source + script);
            QVERIFY2(data.contains(QStringLiteral("--arch")), qPrintable(script));
            QVERIFY2(data.contains(QStringLiteral("packaging_normalize_arch")), qPrintable(script));
        }
    }

    void rpmPackagingShipsEveryBinary()
    {
        // %files is an explicit list: a binary missing from it fails the build or
        // ships a package without the tool.
        QFile file(QStringLiteral(QTIDM_SOURCE_DIR) + QStringLiteral("/packaging/rpm/qtidm.spec"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto data = QString::fromUtf8(file.readAll());
        for (const auto& binary : { "/usr/bin/qtIDM", "/usr/bin/qtIDM-native-host", "/usr/bin/qtidm-cli" }) {
            QVERIFY2(data.contains(QLatin1String(binary)), binary);
        }
    }

    void debianPackagingInstallsEveryBinary()
    {
        // packaging/debian/install is an explicit list: a binary missing from it
        // is simply dropped from the package.
        QFile file(QStringLiteral(QTIDM_SOURCE_DIR) + QStringLiteral("/packaging/debian/install"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto data = QString::fromUtf8(file.readAll());
        for (const auto& binary : { "usr/bin/qtIDM", "usr/bin/qtIDM-native-host", "usr/bin/qtidm-cli" }) {
            QVERIFY2(data.contains(QLatin1String(binary)), binary);
        }
    }

    void appImageLauncherExposesTheCli()
    {
        QFile file(QStringLiteral(QTIDM_SOURCE_DIR) + QStringLiteral("/packaging/appimage/AppRun"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto data = QString::fromUtf8(file.readAll());
        QVERIFY(data.contains(QStringLiteral("qtidm-cli")));
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
    void releaseSmokeLinksStayOutOfTheSourceTree()
    {
        QFile file(QStringLiteral(QTIDM_SOURCE_DIR)
                   + QStringLiteral("/.github/workflows/build-release.yml"));
        QVERIFY(file.exists());
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto workflow = QString::fromUtf8(file.readAll());
        // The Flatpak bundle is built from the checkout, so a smoke link left
        // there becomes a dead source-tree entry named after a CMake target and
        // makes the bundle's linker fail with "cannot open output file".
        QVERIFY2(!workflow.contains(QStringLiteral("ln -sf \"$APPIMAGE\" qtidm-cli")),
                 "the AppImage smoke link must not be created inside the checkout");
        QVERIFY(workflow.contains(QStringLiteral("SMOKE_DIR=\"$RUNNER_TEMP")));
        QVERIFY(workflow.contains(QStringLiteral("\"$SMOKE_DIR/qtidm-cli\" --version")));
    }

    void releaseWorkflowPackagesBothArchitectures()
    {
        QFile file(QStringLiteral(QTIDM_SOURCE_DIR)
                   + QStringLiteral("/.github/workflows/build-release.yml"));
        QVERIFY(file.exists());
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto workflow = QString::fromUtf8(file.readAll());
        // All 5 packaging targets (deb, rpm, arch, flatpak, appimage) must build
        // for both amd64 and arm64 architectures.
        QVERIFY(workflow.contains(QStringLiteral("ubuntu-24.04")));
        QVERIFY(workflow.contains(QStringLiteral("ubuntu-24.04-arm")));
        QVERIFY(workflow.contains(QStringLiteral("package-deb-appimage-flatpak")));
        QVERIFY(workflow.contains(QStringLiteral("package-rpm")));
        QVERIFY(workflow.contains(QStringLiteral("package-arch")));
        QVERIFY(workflow.contains(QStringLiteral("publish-release")));
    }
};

QTEST_MAIN(PackagingMetadataTest)
#include "PackagingMetadataTest.moc"
