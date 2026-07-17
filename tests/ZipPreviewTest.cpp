#include "core/ZipPreview.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>

namespace {
void appendU16(QByteArray* data, quint16 value)
{
    data->append(char(value & 0xff));
    data->append(char((value >> 8) & 0xff));
}

void appendU32(QByteArray* data, quint32 value)
{
    appendU16(data, quint16(value & 0xffff));
    appendU16(data, quint16((value >> 16) & 0xffff));
}

void appendU64(QByteArray* data, quint64 value)
{
    appendU32(data, quint32(value & 0xffffffffU));
    appendU32(data, quint32((value >> 32) & 0xffffffffU));
}

QByteArray centralHeader(const QByteArray& name, quint32 compressed, quint32 uncompressed, const QByteArray& extra = {})
{
    QByteArray data;
    appendU32(&data, 0x02014b50);
    appendU16(&data, 45);
    appendU16(&data, 20);
    appendU16(&data, 0);
    appendU16(&data, 0);
    appendU16(&data, 0);
    appendU16(&data, 0);
    appendU32(&data, 0);
    appendU32(&data, compressed);
    appendU32(&data, uncompressed);
    appendU16(&data, quint16(name.size()));
    appendU16(&data, quint16(extra.size()));
    appendU16(&data, 0);
    appendU16(&data, 0);
    appendU16(&data, 0);
    appendU32(&data, 0);
    appendU32(&data, 0);
    data.append(name);
    data.append(extra);
    return data;
}

QByteArray eocd(quint16 count, quint32 centralSize, quint32 centralOffset)
{
    QByteArray data;
    appendU32(&data, 0x06054b50);
    appendU16(&data, 0);
    appendU16(&data, 0);
    appendU16(&data, count);
    appendU16(&data, count);
    appendU32(&data, centralSize);
    appendU32(&data, centralOffset);
    appendU16(&data, 0);
    return data;
}

QByteArray zip64Eocd(quint64 count, quint64 centralSize, quint64 centralOffset)
{
    QByteArray data;
    appendU32(&data, 0x06064b50);
    appendU64(&data, 44);
    appendU16(&data, 45);
    appendU16(&data, 45);
    appendU32(&data, 0);
    appendU32(&data, 0);
    appendU64(&data, count);
    appendU64(&data, count);
    appendU64(&data, centralSize);
    appendU64(&data, centralOffset);
    return data;
}

QByteArray zip64Locator(quint64 zip64EocdOffset)
{
    QByteArray data;
    appendU32(&data, 0x07064b50);
    appendU32(&data, 0);
    appendU64(&data, zip64EocdOffset);
    appendU32(&data, 1);
    return data;
}

QByteArray zip64SizeExtra(quint64 uncompressed, quint64 compressed)
{
    QByteArray data;
    appendU16(&data, 0x0001);
    appendU16(&data, 16);
    appendU64(&data, uncompressed);
    appendU64(&data, compressed);
    return data;
}

QString writeArchive(QTemporaryDir& dir, const QByteArray& data)
{
    const auto path = dir.path() + QStringLiteral("/fixture.zip");
    QFile file(path);
    Q_ASSERT(file.open(QIODevice::WriteOnly));
    file.write(data);
    file.close();
    return path;
}
}

class ZipPreviewTest final : public QObject {
    Q_OBJECT
private slots:
    void readsClassicCentralDirectory()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        QByteArray archive("local-data");
        const auto centralOffset = archive.size();
        const auto central = centralHeader("file.txt", 12, 34);
        archive.append(central);
        archive.append(eocd(1, central.size(), centralOffset));

        QString error;
        const auto entries = qtidm::ZipPreview().read(writeArchive(dir, archive), &error);

        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(entries.size(), 1);
        QCOMPARE(entries[0].name, QStringLiteral("file.txt"));
        QCOMPARE(entries[0].compressedSize, quint64(12));
        QCOMPARE(entries[0].uncompressedSize, quint64(34));
    }

    void readsZip64EntrySizesFromExtraField()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        constexpr quint64 uncompressed = 5ULL * 1024ULL * 1024ULL * 1024ULL;
        constexpr quint64 compressed = 123456789012ULL;
        QByteArray archive("local-data");
        const auto centralOffset = archive.size();
        const auto central = centralHeader("big.bin", 0xffffffffU, 0xffffffffU, zip64SizeExtra(uncompressed, compressed));
        archive.append(central);
        archive.append(eocd(1, central.size(), centralOffset));

        QString error;
        const auto entries = qtidm::ZipPreview().read(writeArchive(dir, archive), &error);

        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(entries.size(), 1);
        QCOMPARE(entries[0].name, QStringLiteral("big.bin"));
        QCOMPARE(entries[0].compressedSize, compressed);
        QCOMPARE(entries[0].uncompressedSize, uncompressed);
    }

    void readsZip64DirectoryOffsetAndCount()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        QByteArray archive("prefix");
        const auto centralOffset = archive.size();
        const auto central = centralHeader("zip64.txt", 3, 9);
        archive.append(central);
        const auto zip64Offset = archive.size();
        archive.append(zip64Eocd(1, central.size(), centralOffset));
        archive.append(zip64Locator(zip64Offset));
        archive.append(eocd(0xffffU, 0xffffffffU, 0xffffffffU));

        QString error;
        const auto entries = qtidm::ZipPreview().read(writeArchive(dir, archive), &error);

        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(entries.size(), 1);
        QCOMPARE(entries[0].name, QStringLiteral("zip64.txt"));
        QCOMPARE(entries[0].compressedSize, quint64(3));
        QCOMPARE(entries[0].uncompressedSize, quint64(9));
    }

    void readsZip64LargeEntryCount()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        constexpr int entryCount = 70000;
        QByteArray archive("prefix");
        const auto centralOffset = archive.size();
        QByteArray central;
        central.reserve(entryCount * 56);
        for (int i = 0; i < entryCount; ++i) {
            central.append(centralHeader(QByteArray("f") + QByteArray::number(i), 0, 0));
        }
        archive.append(central);
        const auto zip64Offset = archive.size();
        archive.append(zip64Eocd(entryCount, central.size(), centralOffset));
        archive.append(zip64Locator(zip64Offset));
        archive.append(eocd(0xffffU, central.size(), centralOffset));

        QString error;
        const auto entries = qtidm::ZipPreview().read(writeArchive(dir, archive), &error);

        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(entries.size(), entryCount);
        QCOMPARE(entries.first().name, QStringLiteral("f0"));
        QCOMPARE(entries.last().name, QStringLiteral("f69999"));
    }

    void rejectsMissingEndOfCentralDirectory()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        QString error;
        const auto entries = qtidm::ZipPreview().read(writeArchive(dir, QByteArray("not-a-zip")), &error);

        QVERIFY(entries.isEmpty());
        QCOMPARE(error, QStringLiteral("ZIP central directory not found"));
    }

    void rejectsMissingZip64Locator()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        QByteArray archive("prefix");
        const auto centralOffset = archive.size();
        const auto central = centralHeader("broken.txt", 1, 1);
        archive.append(central);
        archive.append(eocd(0xffffU, central.size(), centralOffset));

        QString error;
        const auto entries = qtidm::ZipPreview().read(writeArchive(dir, archive), &error);

        QVERIFY(entries.isEmpty());
        QCOMPARE(error, QStringLiteral("ZIP64 locator not found"));
    }

    void rejectsInvalidCentralDirectoryOffset()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        QByteArray archive("prefix");
        archive.append(eocd(1, 0, 999999));

        QString error;
        const auto entries = qtidm::ZipPreview().read(writeArchive(dir, archive), &error);

        QVERIFY(entries.isEmpty());
        QCOMPARE(error, QStringLiteral("invalid ZIP central directory offset"));
    }
};

QTEST_MAIN(ZipPreviewTest)
#include "ZipPreviewTest.moc"
