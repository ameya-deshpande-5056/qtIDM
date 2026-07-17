#pragma once

#include "storage/DownloadRepository.h"

#include <QString>

namespace qtidm {

class ImportExportService final {
public:
    explicit ImportExportService(DownloadRepository& repository);

    bool exportHistory(const QString& path) const;
    bool importHistory(const QString& path);
    QString lastError() const;

private:
    DownloadRepository& repository_;
    mutable QString lastError_;
};

}
