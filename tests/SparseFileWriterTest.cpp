#include "core/SparseFileWriter.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>

class SparseFileWriterTest final : public QObject {
    Q_OBJECT
private slots:
    void writesAtOffsetsAndPreservesSparseSize()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const auto path = dir.path() + QStringLiteral("/sparse.bin");
        qtidm::SparseFileWriter writer;
        QVERIFY(writer.open(path, 1024));
        const QByteArray first("abc");
        const QByteArray second("xyz");
        QVERIFY(writer.writeAt(10, first.constData(), first.size()));
        QVERIFY(writer.writeAt(900, second.constData(), second.size()));
        writer.close();

        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.size(), qint64(1024));
        QVERIFY(file.seek(10));
        QCOMPARE(file.read(3), first);
        QVERIFY(file.seek(900));
        QCOMPARE(file.read(3), second);
    }

    void rejectsOutOfRangeWrites()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        qtidm::SparseFileWriter writer;
        QVERIFY(writer.open(dir.path() + QStringLiteral("/limited.bin"), 8));
        const QByteArray data("too-large");
        QVERIFY(!writer.writeAt(4, data.constData(), data.size()));
        QCOMPARE(writer.lastError(), QStringLiteral("write exceeds target size"));
    }

    void rejectsWritesBeforeOpen()
    {
        qtidm::SparseFileWriter writer;
        const QByteArray data("x");
        QVERIFY(!writer.writeAt(0, data.constData(), data.size()));
        QCOMPARE(writer.lastError(), QStringLiteral("writer is not open"));
    }

    void truncatesExistingFileForUnknownLengthDownload()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const auto path = dir.path() + QStringLiteral("/unknown-length.bin");
        QFile existing(path);
        QVERIFY(existing.open(QIODevice::WriteOnly));
        QVERIFY(existing.write(QByteArray(1024, 'x')) == 1024);
        existing.close();

        qtidm::SparseFileWriter writer;
        QVERIFY(writer.open(path, -1));
        const QByteArray data("new");
        QVERIFY(writer.writeAt(0, data.constData(), data.size()));
        writer.close();

        QVERIFY(existing.open(QIODevice::ReadOnly));
        QCOMPARE(existing.readAll(), data);
    }

    void preservesExistingFileForUnknownLengthResume()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const auto path = dir.path() + QStringLiteral("/unknown-resume.bin");
        QFile existing(path);
        QVERIFY(existing.open(QIODevice::WriteOnly));
        QCOMPARE(existing.write("prefix"), qint64(6));
        existing.close();

        qtidm::SparseFileWriter writer;
        QVERIFY(writer.open(path, -1, true));
        const QByteArray suffix("-suffix");
        QVERIFY(writer.writeAt(6, suffix.constData(), suffix.size()));
        QVERIFY(writer.setExpectedSize(6 + suffix.size()));
        writer.close();

        QVERIFY(existing.open(QIODevice::ReadOnly));
        QCOMPARE(existing.readAll(), QByteArray("prefix-suffix"));
    }
};

QTEST_MAIN(SparseFileWriterTest)
#include "SparseFileWriterTest.moc"
