#include "integration/SwStreamSessionManager.h"

#include "app/Logger.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrl>
#include <limits>

namespace qtidm {

namespace {
constexpr int kMaxConcurrentSessions = 4;
constexpr qsizetype kMaxBase64ChunkChars = 512 * 1024;
constexpr qsizetype kMaxDecodedChunkBytes = 256 * 1024;

QJsonObject errorResponse(const QString& code, const QString& message)
{
    Logger::error(QStringLiteral("SwStreamSessionManager"), code + QStringLiteral(": ") + message);
    return {
        { QStringLiteral("ok"), false },
        { QStringLiteral("code"), code },
        { QStringLiteral("message"), message }
    };
}

bool validHttpUrl(const QString& value)
{
    const QUrl url(value);
    return url.isValid()
        && (url.scheme() == QStringLiteral("http") || url.scheme() == QStringLiteral("https"))
        && url.userInfo().isEmpty()
        && url.host() != QStringLiteral("169.254.169.254");
}
} // namespace

SwStreamSessionManager::SwStreamSessionManager(QString outputDirectory)
    : outputDirectory_(std::move(outputDirectory))
{
}

SwStreamSessionManager::~SwStreamSessionManager()
{
    abortAll();
}

bool SwStreamSessionManager::isValidStreamId(const QString& streamId)
{
    if (streamId.isEmpty() || streamId.size() > 128) {
        return false;
    }
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9._-]+$"));
    return re.match(streamId).hasMatch();
}

QString SwStreamSessionManager::sanitizeFilename(const QString& rawFilename)
{
    QString name = rawFilename;
    name.replace(QLatin1Char('\0'), QString());
    // Take only the leaf segment if a path was supplied
    const auto slashIdx = qMax(name.lastIndexOf(QLatin1Char('/')), name.lastIndexOf(QLatin1Char('\\')));
    if (slashIdx >= 0) {
        name = name.mid(slashIdx + 1);
    }
    name = name.trimmed();

    QString cleaned;
    cleaned.reserve(name.size());
    for (const QChar ch : name) {
        const auto code = ch.unicode();
        if (code < 0x20 || code == 0x7F) {
            continue;
        }
        if (ch == QLatin1Char('/') || ch == QLatin1Char('\\') || ch == QLatin1Char(':')
            || ch == QLatin1Char('*') || ch == QLatin1Char('?') || ch == QLatin1Char('"')
            || ch == QLatin1Char('<') || ch == QLatin1Char('>') || ch == QLatin1Char('|')) {
            cleaned.append(QLatin1Char('_'));
        } else {
            cleaned.append(ch);
        }
    }
    cleaned = cleaned.trimmed();
    while (cleaned.startsWith(QLatin1Char('.'))) {
        cleaned.remove(0, 1);
    }
    cleaned.replace(QStringLiteral(".."), QStringLiteral("_"));
    if (cleaned.size() > 200) {
        const QFileInfo fi(cleaned);
        const QString ext = fi.suffix();
        if (!ext.isEmpty() && ext.size() <= 16) {
            cleaned = cleaned.left(200 - ext.size() - 1) + QLatin1Char('.') + ext;
        } else {
            cleaned = cleaned.left(200);
        }
    }
    if (cleaned.isEmpty() || cleaned == QStringLiteral(".") || cleaned == QStringLiteral("..")) {
        return QStringLiteral("sw_download.bin");
    }
    return cleaned;
}

QString SwStreamSessionManager::resolveOutputDirectory() const
{
    if (!outputDirectory_.isEmpty()) {
        return outputDirectory_;
    }
    QString dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (dir.isEmpty()) {
        dir = QDir::homePath() + QStringLiteral("/Downloads");
    }
    return dir;
}

QString SwStreamSessionManager::allocateUniquePath(const QString& directory, const QString& filename) const
{
    QDir dir(directory);
    QString candidate = dir.filePath(filename);
    auto isReserved = [&](const QString& path) {
        if (QFileInfo::exists(path) || QFileInfo::exists(path + QStringLiteral(".qtidm-part"))) {
            return true;
        }
        for (const auto& [id, sess] : sessions_) {
            Q_UNUSED(id);
            if (sess && sess->finalPath == path) {
                return true;
            }
        }
        return false;
    };
    if (!isReserved(candidate)) {
        return candidate;
    }

    const int dotIdx = filename.lastIndexOf(QLatin1Char('.'));
    const QString stem = dotIdx > 0 ? filename.left(dotIdx) : filename;
    const QString ext = dotIdx > 0 ? filename.mid(dotIdx) : QString();
    for (int counter = 1; counter <= 10000; ++counter) {
        const QString numbered = QStringLiteral("%1 (%2)%3").arg(stem).arg(counter).arg(ext);
        candidate = dir.filePath(numbered);
        if (!isReserved(candidate)) {
            return candidate;
        }
    }
    return dir.filePath(QStringLiteral("%1_%2%3").arg(stem).arg(QDateTime::currentMSecsSinceEpoch()).arg(ext));
}

void SwStreamSessionManager::cleanupSession(Session& session, bool removePartFile)
{
    if (session.writer) {
        session.writer->close();
        session.writer.reset();
    }
    if (removePartFile && !session.partPath.isEmpty()) {
        QFile::remove(session.partPath);
    }
}

QJsonObject SwStreamSessionManager::handleBegin(const QJsonObject& request)
{
    const QString streamId = request.value(QStringLiteral("streamId")).toString().trimmed();
    if (!isValidStreamId(streamId)) {
        return errorResponse(QStringLiteral("invalid-stream-id"), QStringLiteral("Invalid ServiceWorker stream identifier."));
    }
    if (sessions_.find(streamId) != sessions_.end()) {
        return errorResponse(QStringLiteral("duplicate-stream-id"), QStringLiteral("Stream session already exists."));
    }
    if (static_cast<int>(sessions_.size()) >= kMaxConcurrentSessions) {
        return errorResponse(QStringLiteral("too-many-streams"), QStringLiteral("Maximum concurrent ServiceWorker streams reached."));
    }

    const QString url = request.value(QStringLiteral("url")).toString().trimmed();
    if (!validHttpUrl(url)) {
        return errorResponse(QStringLiteral("invalid-url"), QStringLiteral("ServiceWorker stream URL must be a valid HTTP or HTTPS URL."));
    }

    const QString filename = sanitizeFilename(request.value(QStringLiteral("filename")).toString());
    const auto expectedSizeVal = request.value(QStringLiteral("expectedSize"));
    std::int64_t expectedSize = -1;
    if (expectedSizeVal.isDouble()) {
        const double raw = expectedSizeVal.toDouble(-1.0);
        if (raw >= 0.0 && raw <= static_cast<double>(std::numeric_limits<std::int64_t>::max() / 2)) {
            expectedSize = static_cast<std::int64_t>(raw);
        }
    }

    const QString outDir = resolveOutputDirectory();
    if (!QDir().mkpath(outDir)) {
        return errorResponse(QStringLiteral("mkdir-failed"), QStringLiteral("Could not create destination directory."));
    }

    const QString finalPath = allocateUniquePath(outDir, filename);
    const QString partPath = finalPath + QStringLiteral(".qtidm-part");

    auto writer = std::make_unique<SparseFileWriter>();
    if (!writer->open(partPath, expectedSize, false)) {
        return errorResponse(QStringLiteral("open-failed"),
            QStringLiteral("Could not open target file for writing: %1").arg(writer->lastError()));
    }

    auto session = std::make_unique<Session>();
    session->streamId = streamId;
    session->url = url;
    session->finalPath = finalPath;
    session->partPath = partPath;
    session->expectedSize = expectedSize;
    session->bytesWritten = 0;
    session->expectedSequence = 0;
    session->writer = std::move(writer);

    sessions_.emplace(streamId, std::move(session));

    return {
        { QStringLiteral("ok"), true },
        { QStringLiteral("streamId"), streamId },
        { QStringLiteral("path"), finalPath }
    };
}

QJsonObject SwStreamSessionManager::handleChunk(const QJsonObject& request)
{
    const QString streamId = request.value(QStringLiteral("streamId")).toString().trimmed();
    auto it = sessions_.find(streamId);
    if (it == sessions_.end() || !it->second) {
        return errorResponse(QStringLiteral("unknown-stream"), QStringLiteral("ServiceWorker stream session not found."));
    }
    Session& session = *it->second;

    const auto sequence = static_cast<std::int64_t>(request.value(QStringLiteral("sequence")).toDouble(-1));
    const auto offset = static_cast<std::int64_t>(request.value(QStringLiteral("offset")).toDouble(-1));
    if (sequence != session.expectedSequence || offset != session.bytesWritten) {
        cleanupSession(session, true);
        sessions_.erase(it);
        return errorResponse(QStringLiteral("out-of-order-chunk"),
            QStringLiteral("Stream chunk sequence or offset mismatch."));
    }

    const QByteArray base64Data = request.value(QStringLiteral("data")).toString().toLatin1();
    if (base64Data.size() > kMaxBase64ChunkChars) {
        cleanupSession(session, true);
        sessions_.erase(it);
        return errorResponse(QStringLiteral("chunk-too-large"), QStringLiteral("Stream chunk exceeds maximum size."));
    }

    const auto decodedResult = QByteArray::fromBase64Encoding(base64Data, QByteArray::AbortOnBase64DecodingErrors);
    if (decodedResult.decodingStatus != QByteArray::Base64DecodingStatus::Ok
        || decodedResult.decoded.size() > kMaxDecodedChunkBytes) {
        cleanupSession(session, true);
        sessions_.erase(it);
        return errorResponse(QStringLiteral("invalid-chunk-data"), QStringLiteral("Invalid base64 chunk payload."));
    }

    const QByteArray& bytes = decodedResult.decoded;
    if (!bytes.isEmpty()) {
        if (!session.writer->writeAt(offset, bytes.constData(), bytes.size())) {
            const QString err = session.writer->lastError();
            cleanupSession(session, true);
            sessions_.erase(it);
            return errorResponse(QStringLiteral("write-failed"),
                QStringLiteral("Failed writing stream chunk: %1").arg(err));
        }
        session.bytesWritten += bytes.size();
    }
    session.expectedSequence += 1;

    return {
        { QStringLiteral("ok"), true },
        { QStringLiteral("streamId"), streamId },
        { QStringLiteral("sequence"), static_cast<double>(sequence) },
        { QStringLiteral("bytesWritten"), static_cast<double>(session.bytesWritten) }
    };
}

QJsonObject SwStreamSessionManager::handleFinish(const QJsonObject& request)
{
    const QString streamId = request.value(QStringLiteral("streamId")).toString().trimmed();
    auto it = sessions_.find(streamId);
    if (it == sessions_.end() || !it->second) {
        return errorResponse(QStringLiteral("unknown-stream"), QStringLiteral("ServiceWorker stream session not found."));
    }

    std::unique_ptr<Session> session = std::move(it->second);
    sessions_.erase(it);

    const auto totalBytesVal = request.value(QStringLiteral("totalBytes"));
    if (totalBytesVal.isDouble()) {
        const auto reportedTotal = static_cast<std::int64_t>(totalBytesVal.toDouble(-1));
        if (reportedTotal >= 0 && reportedTotal != session->bytesWritten) {
            cleanupSession(*session, true);
            return errorResponse(QStringLiteral("size-mismatch"),
                QStringLiteral("Reported stream byte total does not match bytes written."));
        }
    }

    if (session->expectedSize >= 0 && session->bytesWritten != session->expectedSize) {
        // Truncate/resize to actual written size if stream finished early, or verify consistency
        if (!session->writer->setExpectedSize(session->bytesWritten)) {
            cleanupSession(*session, true);
            return errorResponse(QStringLiteral("truncate-failed"),
                QStringLiteral("Could not finalize stream file size."));
        }
    }

    if (!session->writer->sync()) {
        const QString err = session->writer->lastError();
        cleanupSession(*session, true);
        return errorResponse(QStringLiteral("sync-failed"),
            QStringLiteral("Failed syncing stream file to disk: %1").arg(err));
    }
    session->writer->close();
    session->writer.reset();

    if (QFile::exists(session->finalPath)) {
        QFile::remove(session->finalPath);
    }
    if (!QFile::rename(session->partPath, session->finalPath)) {
        QFile::remove(session->partPath);
        return errorResponse(QStringLiteral("rename-failed"),
            QStringLiteral("Could not finalize downloaded stream file."));
    }

    return {
        { QStringLiteral("ok"), true },
        { QStringLiteral("streamId"), streamId },
        { QStringLiteral("path"), session->finalPath },
        { QStringLiteral("bytesWritten"), static_cast<double>(session->bytesWritten) }
    };
}

QJsonObject SwStreamSessionManager::handleAbort(const QJsonObject& request)
{
    const QString streamId = request.value(QStringLiteral("streamId")).toString().trimmed();
    auto it = sessions_.find(streamId);
    if (it != sessions_.end() && it->second) {
        cleanupSession(*it->second, true);
        sessions_.erase(it);
    }
    return {
        { QStringLiteral("ok"), true },
        { QStringLiteral("streamId"), streamId },
        { QStringLiteral("aborted"), true }
    };
}

int SwStreamSessionManager::activeSessionCount() const
{
    return static_cast<int>(sessions_.size());
}

void SwStreamSessionManager::abortAll()
{
    for (auto& [id, sess] : sessions_) {
        Q_UNUSED(id);
        if (sess) {
            cleanupSession(*sess, true);
        }
    }
    sessions_.clear();
}

} // namespace qtidm
