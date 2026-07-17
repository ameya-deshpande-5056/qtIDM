#include "integration/ImportExportService.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>

namespace qtidm {

ImportExportService::ImportExportService(DownloadRepository& repository)
    : repository_(repository)
{
}

bool ImportExportService::exportHistory(const QString& path) const
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        lastError_ = file.errorString();
        return false;
    }
    QJsonArray records;
    for (const auto& record : repository_.listDownloads()) {
        records.append(recordToJson(record));
    }
    file.write(QJsonDocument(records).toJson(QJsonDocument::Indented));
    return true;
}

bool ImportExportService::importHistory(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        lastError_ = file.errorString();
        return false;
    }
    const auto doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isArray()) {
        lastError_ = QStringLiteral("invalid import file");
        return false;
    }
    for (const auto& value : doc.array()) {
        if (!repository_.upsertDownload(recordFromJson(value.toObject()))) {
            lastError_ = repository_.lastError();
            return false;
        }
    }
    return true;
}

QString ImportExportService::lastError() const
{
    return lastError_;
}

}
