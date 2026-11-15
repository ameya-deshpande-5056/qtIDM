#include "app/Paths.h"

#include <QDir>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

using namespace qtidm;

class PathsTest final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
    }

    void cleanup()
    {
        Paths::setDataDirectory({});
    }

    void defaultLocationsUseTheFixedProjectName()
    {
        QVERIFY(Paths::dataDir().endsWith(QStringLiteral("/qtIDM")));
        QVERIFY(Paths::configDir().endsWith(QStringLiteral("/qtIDM")));
        QVERIFY(Paths::databasePath().endsWith(QStringLiteral("/downloads.sqlite3")));
        QCOMPARE(Paths::dataDirOverride(), QString());
        QCOMPARE(Paths::instanceSuffix(), QString());
    }

    void overrideRedirectsEveryPath()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Paths::setDataDirectory(directory.path());

        const auto absolute = QDir(directory.path()).absolutePath();
        QCOMPARE(Paths::dataDir(), absolute);
        QCOMPARE(Paths::configDir(), absolute + QStringLiteral("/config"));
        QCOMPARE(Paths::databasePath(), absolute + QStringLiteral("/downloads.sqlite3"));
        QCOMPARE(Paths::logPath(), absolute + QStringLiteral("/qtidm.log"));
        QCOMPARE(Paths::dataDirOverride(), directory.path());

        Paths::ensureRuntimeDirs();
        QVERIFY(QDir(absolute).exists());
        QVERIFY(QDir(absolute + QStringLiteral("/config")).exists());
    }

    void suffixIsStableAndSeparatesInstances()
    {
        QTemporaryDir first;
        QTemporaryDir second;
        QVERIFY(first.isValid() && second.isValid());

        Paths::setDataDirectory(first.path());
        const auto firstSuffix = Paths::instanceSuffix();
        QCOMPARE(firstSuffix.size(), 8);
        QVERIFY(QRegularExpression(QStringLiteral("^[0-9a-f]{8}$")).match(firstSuffix).hasMatch());

        // Must be identical across processes, so not qHash.
        Paths::setDataDirectory(first.path());
        QCOMPARE(Paths::instanceSuffix(), firstSuffix);

        Paths::setDataDirectory(second.path());
        QVERIFY(Paths::instanceSuffix() != firstSuffix);
    }

    void clearingTheOverrideRestoresTheDefault()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Paths::setDataDirectory(directory.path());
        QVERIFY(!Paths::instanceSuffix().isEmpty());

        Paths::setDataDirectory({});
        QVERIFY(Paths::dataDir().endsWith(QStringLiteral("/qtIDM")));
        QCOMPARE(Paths::dataDirOverride(), QString());
        QCOMPARE(Paths::instanceSuffix(), QString());
    }
};

QTEST_GUILESS_MAIN(PathsTest)
#include "PathsTest.moc"
