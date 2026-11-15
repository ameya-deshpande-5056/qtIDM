#include "integration/UpdateChecker.h"

#include <QtTest/QtTest>

class UpdateCheckerTest final : public QObject {
    Q_OBJECT

private slots:
    void cleansVersionTags()
    {
        QCOMPARE(qtidm::UpdateChecker::cleanVersionTag(QStringLiteral("v0.3.3")), QStringLiteral("0.3.3"));
        QCOMPARE(qtidm::UpdateChecker::cleanVersionTag(QStringLiteral("V2.9.8")), QStringLiteral("2.9.8"));
        QCOMPARE(qtidm::UpdateChecker::cleanVersionTag(QStringLiteral("2026.10.26")), QStringLiteral("2026.10.26"));
        QCOMPARE(qtidm::UpdateChecker::cleanVersionTag(QStringLiteral("  v1.0.4  ")), QStringLiteral("1.0.4"));
    }

    void parsesVersionNumbers()
    {
        QCOMPARE(qtidm::UpdateChecker::parseVersionNumbers(QStringLiteral("0.3.3")),
                 QList<int>({0, 3, 3}));
        QCOMPARE(qtidm::UpdateChecker::parseVersionNumbers(QStringLiteral("v0.3.4")),
                 QList<int>({0, 3, 4}));
        QCOMPARE(qtidm::UpdateChecker::parseVersionNumbers(QStringLiteral("2026.10.26")),
                 QList<int>({2026, 10, 26}));
        QCOMPARE(qtidm::UpdateChecker::parseVersionNumbers(QStringLiteral("6.1.1-3ubuntu5")),
                 QList<int>({6, 1, 1, 3, 5}));
        QVERIFY(qtidm::UpdateChecker::parseVersionNumbers(QStringLiteral("")).isEmpty());
    }

    void comparesVersionsCorrectly()
    {
        // SemVer
        QVERIFY(qtidm::UpdateChecker::isVersionNewer(QStringLiteral("0.3.4"), QStringLiteral("0.3.3")));
        QVERIFY(!qtidm::UpdateChecker::isVersionNewer(QStringLiteral("0.3.3"), QStringLiteral("0.3.3")));
        QVERIFY(!qtidm::UpdateChecker::isVersionNewer(QStringLiteral("0.3.2"), QStringLiteral("0.3.3")));
        QVERIFY(qtidm::UpdateChecker::isVersionNewer(QStringLiteral("1.0.0"), QStringLiteral("0.9.9")));
        QVERIFY(qtidm::UpdateChecker::isVersionNewer(QStringLiteral("0.4.0"), QStringLiteral("0.3.9")));
        QVERIFY(qtidm::UpdateChecker::isVersionNewer(QStringLiteral("v0.3.4"), QStringLiteral("v0.3.3")));

        // CalVer (e.g. yt-dlp)
        QVERIFY(qtidm::UpdateChecker::isVersionNewer(QStringLiteral("2026.11.01"), QStringLiteral("2026.10.26")));
        QVERIFY(!qtidm::UpdateChecker::isVersionNewer(QStringLiteral("2026.10.26"), QStringLiteral("2026.11.01")));
        QVERIFY(!qtidm::UpdateChecker::isVersionNewer(QStringLiteral("2026.10.26"), QStringLiteral("2026.10.26")));

        // Minor / Patch jumps (e.g. Deno)
        QVERIFY(qtidm::UpdateChecker::isVersionNewer(QStringLiteral("2.9.8"), QStringLiteral("2.1.2")));
        QVERIFY(!qtidm::UpdateChecker::isVersionNewer(QStringLiteral("2.1.2"), QStringLiteral("2.9.8")));
    }

    void handlesUpdateResultSummaryAndFlags()
    {
        qtidm::UpdateCheckResult result;
        QVERIFY(!result.hasAnyUpdates());
        QVERIFY(result.summary().isEmpty());

        // App update available
        result.app.currentVersion = QStringLiteral("0.3.3");
        result.app.latestVersion = QStringLiteral("0.3.4");
        result.app.updateAvailable = true;
        QVERIFY(result.hasAnyUpdates());
        QVERIFY(result.summary().contains(QStringLiteral("qtIDM 0.3.4 is available")));

        // Dependency update available
        result.app.updateAvailable = false;
        qtidm::DependencyUpdateInfo dep;
        dep.name = QStringLiteral("yt-dlp");
        dep.installedVersion = QStringLiteral("2026.10.26");
        dep.latestVersion = QStringLiteral("2026.11.01");
        dep.installed = true;
        dep.updateAvailable = true;
        result.dependencies.append(dep);

        QVERIFY(result.hasAnyUpdates());
        QVERIFY(result.summary().contains(QStringLiteral("yt-dlp 2026.11.01 is available")));
    }

    void inspectsLocalDependencies()
    {
        qtidm::UpdateChecker checker;
        const auto result = checker.checkSync();
        // Check that dependencies list is populated with all expected components
        QStringList names;
        for (const auto& dep : result.dependencies) {
            names.append(dep.name);
        }
        QVERIFY(names.contains(QStringLiteral("yt-dlp")));
        QVERIFY(names.contains(QStringLiteral("Deno")));
        QVERIFY(names.contains(QStringLiteral("FFmpeg")));
        QVERIFY(names.contains(QStringLiteral("7-Zip")));

        // On this Linux environment, FFmpeg is installed
        const auto ffmpegIt = std::find_if(result.dependencies.begin(), result.dependencies.end(),
            [](const qtidm::DependencyUpdateInfo& d) { return d.name == QStringLiteral("FFmpeg"); });
        QVERIFY(ffmpegIt != result.dependencies.end());
        QVERIFY(ffmpegIt->installed);
        QVERIFY(!ffmpegIt->installedVersion.isEmpty());
    }
};

QTEST_MAIN(UpdateCheckerTest)
#include "UpdateCheckerTest.moc"
