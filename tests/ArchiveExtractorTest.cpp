#include "core/ArchiveExtractor.h"

#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest/QtTest>

class ArchiveExtractorTest final : public QObject {
    Q_OBJECT
private slots:
    void recognizesSupportedArchives()
    {
        QVERIFY(qtidm::ArchiveExtractor::supports(QStringLiteral("bundle.tar.gz")));
        QVERIFY(qtidm::ArchiveExtractor::supports(QStringLiteral("bundle.7z")));
        QVERIFY(!qtidm::ArchiveExtractor::supports(QStringLiteral("document.pdf")));
    }

    void rejectsTraversalAndSymbolicLinks()
    {
        QString error;
        QVERIFY(!qtidm::ArchiveExtractor::listingIsSafe(
            "----------\nPath = ../escape.txt\n", &error));
        QVERIFY(error.contains(QStringLiteral("unsafe path")));

        error.clear();
        QVERIFY(!qtidm::ArchiveExtractor::listingIsSafe(
            "----------\nPath = link\nAttributes = A_ lrwxrwxrwx\n", &error));
        QVERIFY(error.contains(QStringLiteral("symbolic link")));

        error.clear();
        QVERIFY(qtidm::ArchiveExtractor::listingIsSafe(
            "----------\nPath = folder/file.txt\nMode = -rw-r--r--\n"
            "Symbolic Link = \nHard Link = \n", &error));

        error.clear();
        QVERIFY(!qtidm::ArchiveExtractor::listingIsSafe(
            "----------\nPath = alias\nMode = -rw-r--r--\nHard Link = target\n", &error));
        QVERIFY(error.contains(QStringLiteral("hard link")));
    }

    void extractsWithSevenZip()
    {
        const auto sevenZip = QStandardPaths::findExecutable(QStringLiteral("7z"));
        if (sevenZip.isEmpty()) {
            QSKIP("7-Zip is not installed");
        }
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QFile source(dir.path() + QStringLiteral("/hello.txt"));
        QVERIFY(source.open(QIODevice::WriteOnly));
        source.write("hello archive");
        source.close();
        const auto archive = dir.path() + QStringLiteral("/sample.zip");

        QProcess create;
        create.setWorkingDirectory(dir.path());
        create.start(sevenZip, { QStringLiteral("a"), QStringLiteral("-tzip"), archive, QStringLiteral("hello.txt") });
        QVERIFY(create.waitForFinished(10000));
        QCOMPARE(create.exitCode(), 0);
        QVERIFY(QFile::remove(source.fileName()));

        qtidm::ArchiveExtractor extractor;
        QSignalSpy finished(&extractor, &qtidm::ArchiveExtractor::extractionFinished);
        QSignalSpy failed(&extractor, &qtidm::ArchiveExtractor::extractionFailed);
        const auto destination = dir.path() + QStringLiteral("/output");
        extractor.extract(QStringLiteral("job-1"), archive, destination, false);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
        QCOMPARE(failed.size(), 0);
        QFile extracted(destination + QStringLiteral("/hello.txt"));
        QVERIFY(extracted.open(QIODevice::ReadOnly));
        QCOMPARE(extracted.readAll(), QByteArray("hello archive"));
    }

    void rejectsDestinationWithExistingContent()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto archive = dir.path() + QStringLiteral("/sample.zip");
        QFile file(archive);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("not inspected because the destination is unsafe");
        file.close();
        const auto destination = dir.path() + QStringLiteral("/output");
        QVERIFY(QDir().mkpath(destination));
        QFile existing(destination + QStringLiteral("/existing.txt"));
        QVERIFY(existing.open(QIODevice::WriteOnly));
        existing.write("keep");
        existing.close();

        qtidm::ArchiveExtractor extractor;
        QSignalSpy failed(&extractor, &qtidm::ArchiveExtractor::extractionFailed);
        extractor.extract(QStringLiteral("job-2"), archive, destination, false);
        QCOMPARE(failed.size(), 1);
        QVERIFY(failed.constFirst().at(1).toString().contains(QStringLiteral("absent or an empty directory")));
        QVERIFY(existing.exists());
    }
};

QTEST_MAIN(ArchiveExtractorTest)
#include "ArchiveExtractorTest.moc"
