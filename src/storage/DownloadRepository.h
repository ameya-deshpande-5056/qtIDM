#pragma once

#include "core/DownloadTypes.h"

#include <QList>
#include <QString>
#include <sqlite3.h>

namespace qtidm {

class DownloadRepository final {
public:
    DownloadRepository() = default;
    ~DownloadRepository();

    bool open(const QString& databasePath);
    bool migrate();
    bool upsertDownload(const DownloadRecord& record);
    bool updateStatus(const QString& id, DownloadStatus status);
    bool updateProgress(const QString& id, qint64 completedBytes, qint64 totalBytes, DownloadStatus status);
    bool updateSegments(const QString& id, const QVector<SegmentInfo>& segments);
    bool removeDownload(const QString& id);
    QList<DownloadRecord> listDownloads() const;
    QString lastError() const;

private:
    sqlite3* db_ = nullptr;
    mutable QString lastError_;
};

}
