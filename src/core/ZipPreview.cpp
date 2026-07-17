#include "core/ZipPreview.h"

#include <QFile>
#include <QtGlobal>

namespace qtidm {

namespace {
constexpr quint32 eocdSignature = 0x06054b50;
constexpr quint32 zip64LocatorSignature = 0x07064b50;
constexpr quint32 zip64EocdSignature = 0x06064b50;
constexpr quint32 centralHeaderSignature = 0x02014b50;

quint16 u16(const QByteArray& data, qsizetype offset)
{
    return static_cast<quint16>(static_cast<unsigned char>(data[offset]))
        | static_cast<quint16>(static_cast<unsigned char>(data[offset + 1]) << 8);
}

quint32 u32(const QByteArray& data, qsizetype offset)
{
    return static_cast<quint32>(u16(data, offset)) | (static_cast<quint32>(u16(data, offset + 2)) << 16);
}

quint64 u64(const QByteArray& data, qsizetype offset)
{
    return static_cast<quint64>(u32(data, offset)) | (static_cast<quint64>(u32(data, offset + 4)) << 32);
}

bool readAt(QFile& file, qint64 offset, qint64 size, QByteArray* out, QString* error)
{
    if (offset < 0 || size < 0 || !file.seek(offset)) {
        if (error) {
            *error = QStringLiteral("invalid ZIP offset");
        }
        return false;
    }
    *out = file.read(size);
    if (out->size() != size) {
        if (error) {
            *error = QStringLiteral("truncated ZIP structure");
        }
        return false;
    }
    return true;
}

void applyZip64Extra(const QByteArray& extra, ZipEntry* entry, quint32 compressed32, quint32 uncompressed32)
{
    qsizetype pos = 0;
    while (pos + 4 <= extra.size()) {
        const auto headerId = u16(extra, pos);
        const auto dataSize = u16(extra, pos + 2);
        pos += 4;
        if (pos + dataSize > extra.size()) {
            return;
        }
        if (headerId == 0x0001) {
            qsizetype zip64Pos = pos;
            if (uncompressed32 == 0xffffffffU && zip64Pos + 8 <= pos + dataSize) {
                entry->uncompressedSize = u64(extra, zip64Pos);
                zip64Pos += 8;
            }
            if (compressed32 == 0xffffffffU && zip64Pos + 8 <= pos + dataSize) {
                entry->compressedSize = u64(extra, zip64Pos);
            }
            return;
        }
        pos += dataSize;
    }
}
}

QList<ZipEntry> ZipPreview::read(const QString& path, QString* error) const
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = file.errorString();
        }
        return {};
    }

    const auto size = file.size();
    const auto tailSize = qMin<qint64>(size, 66000);
    QByteArray tail;
    if (!readAt(file, size - tailSize, tailSize, &tail, error)) {
        return {};
    }

    qsizetype eocd = -1;
    for (qsizetype i = tail.size() - 22; i >= 0; --i) {
        if (u32(tail, i) == eocdSignature) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) {
        if (error) {
            *error = QStringLiteral("ZIP central directory not found");
        }
        return {};
    }

    quint64 count = u16(tail, eocd + 10);
    quint64 centralOffset = u32(tail, eocd + 16);
    const bool needsZip64 = count == 0xffffU || centralOffset == 0xffffffffU;

    if (needsZip64) {
        qsizetype locator = -1;
        const auto locatorSearchStart = qMax<qsizetype>(0, eocd - 1024);
        for (qsizetype i = eocd - 20; i >= locatorSearchStart; --i) {
            if (u32(tail, i) == zip64LocatorSignature) {
                locator = i;
                break;
            }
        }
        if (locator < 0) {
            if (error) {
                *error = QStringLiteral("ZIP64 locator not found");
            }
            return {};
        }
        const auto locatorOffset = size - tailSize + locator;
        QByteArray locatorData;
        if (!readAt(file, locatorOffset, 20, &locatorData, error)) {
            return {};
        }
        if (u32(locatorData, 0) != zip64LocatorSignature) {
            if (error) {
                *error = QStringLiteral("ZIP64 locator not found");
            }
            return {};
        }

        const auto zip64EocdOffset = static_cast<qint64>(u64(locatorData, 8));
        QByteArray zip64Header;
        if (!readAt(file, zip64EocdOffset, 56, &zip64Header, error)) {
            return {};
        }
        if (u32(zip64Header, 0) != zip64EocdSignature) {
            if (error) {
                *error = QStringLiteral("ZIP64 end record not found");
            }
            return {};
        }
        count = u64(zip64Header, 32);
        centralOffset = u64(zip64Header, 48);
    }

    if (centralOffset > static_cast<quint64>(size)) {
        if (error) {
            *error = QStringLiteral("invalid ZIP central directory offset");
        }
        return {};
    }
    file.seek(static_cast<qint64>(centralOffset));

    QList<ZipEntry> entries;
    for (quint64 index = 0; index < count; ++index) {
        const auto header = file.read(46);
        if (header.size() != 46 || u32(header, 0) != centralHeaderSignature) {
            if (error) {
                *error = QStringLiteral("invalid ZIP central directory");
            }
            return {};
        }
        const auto compressed32 = u32(header, 20);
        const auto uncompressed32 = u32(header, 24);
        const auto nameSize = u16(header, 28);
        const auto extraSize = u16(header, 30);
        const auto commentSize = u16(header, 32);

        ZipEntry entry;
        entry.compressedSize = compressed32;
        entry.uncompressedSize = uncompressed32;
        entry.name = QString::fromUtf8(file.read(nameSize));
        const auto extra = file.read(extraSize);
        if (extra.size() != extraSize) {
            if (error) {
                *error = QStringLiteral("truncated ZIP extra fields");
            }
            return {};
        }
        applyZip64Extra(extra, &entry, compressed32, uncompressed32);
        if (!file.seek(file.pos() + commentSize)) {
            if (error) {
                *error = QStringLiteral("invalid ZIP entry comment");
            }
            return {};
        }
        entries.append(entry);
    }
    return entries;
}

}
