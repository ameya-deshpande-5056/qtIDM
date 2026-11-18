#pragma once

#include "core/SparseFileWriter.h"

#include <QJsonObject>
#include <QString>
#include <cstdint>
#include <map>
#include <memory>

namespace qtidm {

class SwStreamSessionManager final {
public:
    explicit SwStreamSessionManager(QString outputDirectory = {});
    ~SwStreamSessionManager();

    SwStreamSessionManager(const SwStreamSessionManager&) = delete;
    SwStreamSessionManager& operator=(const SwStreamSessionManager&) = delete;

    QJsonObject handleBegin(const QJsonObject& request);
    QJsonObject handleChunk(const QJsonObject& request);
    QJsonObject handleFinish(const QJsonObject& request);
    QJsonObject handleAbort(const QJsonObject& request);

    int activeSessionCount() const;
    void abortAll();

    static QString sanitizeFilename(const QString& rawFilename);
    static bool isValidStreamId(const QString& streamId);

private:
    struct Session {
        QString streamId;
        QString url;
        QString finalPath;
        QString partPath;
        std::int64_t expectedSize = -1;
        std::int64_t bytesWritten = 0;
        std::int64_t expectedSequence = 0;
        std::unique_ptr<SparseFileWriter> writer;
    };

    QString resolveOutputDirectory() const;
    QString allocateUniquePath(const QString& directory, const QString& filename) const;
    void cleanupSession(Session& session, bool removePartFile);

    QString outputDirectory_;
    std::map<QString, std::unique_ptr<Session>> sessions_;
};

} // namespace qtidm
