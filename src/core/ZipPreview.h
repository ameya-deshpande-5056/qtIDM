#pragma once

#include <QList>
#include <QString>

namespace qtidm {

struct ZipEntry {
    QString name;
    quint64 compressedSize = 0;
    quint64 uncompressedSize = 0;
};

class ZipPreview final {
public:
    QList<ZipEntry> read(const QString& path, QString* error = nullptr) const;
};

}
