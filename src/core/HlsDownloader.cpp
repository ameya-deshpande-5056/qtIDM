#include "core/HlsDownloader.h"
#include "app/Logger.h"

#include <curl/curl.h>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTextStream>
#include <chrono>

namespace qtidm {

namespace {

size_t writeToStringCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    const size_t total = size * nmemb;
    auto* str = static_cast<std::string*>(userdata);
    str->append(ptr, total);
    return total;
}

size_t writeToByteArrayCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    const size_t total = size * nmemb;
    auto* byteArr = static_cast<QByteArray*>(userdata);
    byteArr->append(ptr, static_cast<qsizetype>(total));
    return total;
}

void applyCurlHeadersAndDoh(CURL* easy, const QVariantMap& headers, const QString& dohUrl, struct curl_slist** outHeaders, QByteArray* outDoh)
{
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(easy, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);

    struct curl_slist* headerList = nullptr;
    for (auto it = headers.cbegin(); it != headers.cend(); ++it) {
        if (it.key().startsWith(QLatin1Char('_'))) continue;
        const auto line = it.key() + QStringLiteral(": ") + it.value().toString();
        headerList = curl_slist_append(headerList, line.toUtf8().constData());
    }
    if (headerList) {
        curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headerList);
        *outHeaders = headerList;
    }

    const QString resolvedDoh = resolveDohUrl(dohUrl.trimmed());
    if (!resolvedDoh.isEmpty()) {
        *outDoh = resolvedDoh.toUtf8();
        curl_easy_setopt(easy, CURLOPT_DOH_URL, outDoh->constData());
    }
}

} // namespace

HlsDownloader::HlsDownloader(QObject* parent)
    : QObject(parent)
{
}

HlsDownloader::~HlsDownloader()
{
    cancel();
    if (engineThread_.joinable()) {
        engineThread_.join();
    }
}

bool HlsDownloader::start(const QString& id, const QUrl& playlistUrl, const QString& targetPath,
                         const QVariantMap& headers, const QString& dohUrl, int concurrency)
{
    if (running_.load()) {
        return false;
    }

    id_ = id;
    initialUrl_ = playlistUrl;
    targetPath_ = targetPath;
    headers_ = headers;
    dohUrl_ = dohUrl;
    concurrency_ = qBound(1, concurrency, 16);

    running_ = true;
    paused_ = false;
    canceled_ = false;
    failed_ = false;
    failureReason_.clear();

    if (engineThread_.joinable()) {
        engineThread_.join();
    }

    engineThread_ = std::jthread([this]() { runEngine(); });
    return true;
}

void HlsDownloader::pause()
{
    if (running_.load() && !paused_.load()) {
        paused_ = true;
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            queueCv_.notify_all();
        }
        emit statusChanged(id_, DownloadStatus::Paused, QStringLiteral("Paused"));
    }
}

void HlsDownloader::resume()
{
    if (running_.load() && paused_.load()) {
        paused_ = false;
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            queueCv_.notify_all();
        }
        emit statusChanged(id_, DownloadStatus::Downloading, QStringLiteral("Resuming..."));
    }
}

void HlsDownloader::cancel()
{
    canceled_ = true;
    running_ = false;
    paused_ = false;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        queueCv_.notify_all();
    }
}

void HlsDownloader::emitSegmentsSnapshot(bool force)
{
    if (!force) {
        if (segmentsTimer_.elapsed() - lastSegmentsEmitMs_ < 100) {
            return;
        }
    }
    lastSegmentsEmitMs_ = segmentsTimer_.elapsed();

    QVector<qtidm::SegmentInfo> segments;
    {
        QMutexLocker lock(&stateMutex_);
        segments.reserve(tasks_.size());
        for (const auto& task : tasks_) {
            qtidm::DownloadStatus st = qtidm::DownloadStatus::Queued;
            if (task.completed) {
                st = qtidm::DownloadStatus::Completed;
            } else if (task.inProgress) {
                st = qtidm::DownloadStatus::Downloading;
            } else if (task.retries > 0) {
                st = qtidm::DownloadStatus::Connecting;
            }
            const qint64 segLen = (task.size > 0) ? task.size : (task.byteRangeLength > 0 ? task.byteRangeLength : 1024 * 1024);
            const qint64 written = task.completed ? segLen : (task.inProgress ? segLen / 2 : 0);
            segments.append(qtidm::SegmentInfo(task.index, task.index * 1024 * 1024, (task.index + 1) * 1024 * 1024 - 1, written, st));
        }
    }
    emit segmentsChanged(id_, segments);
}

bool HlsDownloader::fetchPlaylist(const QUrl& url, QString* content, QUrl* effectiveUrl)
{
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    std::string responseBody;
    struct curl_slist* headers = nullptr;
    QByteArray dohBytes;

    const std::string urlStr = url.toString(QUrl::FullyEncoded).toStdString();
    curl_easy_setopt(curl, CURLOPT_URL, urlStr.c_str());
    applyCurlHeadersAndDoh(curl, headers_, dohUrl_, &headers, &dohBytes);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeToStringCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);

    const CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

    char* effUrlStr = nullptr;
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effUrlStr);
    if (effUrlStr && effUrlStr[0] != '\0') {
        *effectiveUrl = QUrl(QString::fromUtf8(effUrlStr));
    } else {
        *effectiveUrl = url;
    }

    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || httpCode < 200 || httpCode >= 300) {
        return false;
    }

    *content = QString::fromUtf8(responseBody.c_str(), static_cast<qsizetype>(responseBody.size()));
    return true;
}

bool HlsDownloader::fetchKey(const QUrl& keyUrl, QByteArray* keyData)
{
    {
        QMutexLocker lock(&keyMutex_);
        auto it = keyCache_.find(keyUrl);
        if (it != keyCache_.end() && it.value().size() == 16) {
            *keyData = it.value();
            return true;
        }
    }

    CURL* curl = curl_easy_init();
    if (!curl) return false;

    QByteArray body;
    struct curl_slist* headers = nullptr;
    QByteArray dohBytes;

    const std::string urlStr = keyUrl.toString(QUrl::FullyEncoded).toStdString();
    curl_easy_setopt(curl, CURLOPT_URL, urlStr.c_str());
    applyCurlHeadersAndDoh(curl, headers_, dohUrl_, &headers, &dohBytes);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeToByteArrayCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);

    const CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || httpCode < 200 || httpCode >= 300 || body.size() != 16) {
        return false;
    }

    {
        QMutexLocker lock(&keyMutex_);
        keyCache_.insert(keyUrl, body);
    }
    *keyData = body;
    return true;
}

bool HlsDownloader::downloadChunk(const ChunkTask& task, const QString& stagingDir, qint64* downloadedSize)
{
    if (downloadedSize) *downloadedSize = 0;
    const QString targetFile = stagingDir + QLatin1Char('/') + task.outputFilename;
    const QString partFile = targetFile + QStringLiteral(".part");

    if (QFile::exists(targetFile)) {
        const qint64 sz = QFileInfo(targetFile).size();
        if (sz > 0) {
            if (downloadedSize) *downloadedSize = sz;
            return true;
        }
    }

    CURL* curl = curl_easy_init();
    if (!curl) return false;

    QByteArray downloadedData;
    struct curl_slist* headers = nullptr;
    QByteArray dohBytes;

    const std::string urlStr = task.uri.toString(QUrl::FullyEncoded).toStdString();
    curl_easy_setopt(curl, CURLOPT_URL, urlStr.c_str());
    applyCurlHeadersAndDoh(curl, headers_, dohUrl_, &headers, &dohBytes);

    std::string rangeStr;
    if (task.byteRangeLength > 0) {
        const qint64 offset = qMax<qint64>(0, task.byteRangeOffset);
        const qint64 end = offset + task.byteRangeLength - 1;
        rangeStr = std::to_string(offset) + "-" + std::to_string(end);
        curl_easy_setopt(curl, CURLOPT_RANGE, rangeStr.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeToByteArrayCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &downloadedData);

    const CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || httpCode < 200 || httpCode >= 300 || downloadedData.isEmpty()) {
        return false;
    }

    // Decrypt if AES-128
    QByteArray outputData = downloadedData;
    if (task.key.method == QStringLiteral("AES-128")) {
        QByteArray keyData;
        if (!fetchKey(task.key.uri, &keyData)) {
            return false;
        }
        outputData = HlsPlaylistParser::decryptAes128Cbc(downloadedData, keyData, task.key.iv);
        if (outputData.isEmpty()) {
            return false;
        }
    }

    // Write to part file then atomically rename
    QFile file(partFile);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(outputData);
    file.close();

    QFile::remove(targetFile);
    if (!file.rename(targetFile)) {
        return false;
    }

    const qint64 finalSize = outputData.size();
    if (downloadedSize) *downloadedSize = finalSize;
    downloadedBytes_ += finalSize;
    return true;
}

void HlsDownloader::workerLoop(const QString& stagingDir)
{
    while (running_.load() && !canceled_.load() && !failed_.load()) {
        int taskIndex = -1;
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            queueCv_.wait(lock, [this]() {
                return !running_.load() || canceled_.load() || failed_.load()
                    || (!paused_.load() && !pendingTaskIndices_.empty())
                    || (!paused_.load() && pendingTaskIndices_.empty() && activeWorkers_ == 0);
            });

            if (!running_.load() || canceled_.load() || failed_.load()) {
                break;
            }

            if (pendingTaskIndices_.empty() && activeWorkers_ == 0) {
                break;
            }

            if (paused_.load() || pendingTaskIndices_.empty()) {
                continue;
            }

            taskIndex = pendingTaskIndices_.front();
            pendingTaskIndices_.pop_front();
            activeWorkers_++;

            {
                QMutexLocker stateLock(&stateMutex_);
                tasks_[taskIndex].inProgress = true;
            }
        }

        emitSegmentsSnapshot();

        ChunkTask task;
        {
            QMutexLocker stateLock(&stateMutex_);
            task = tasks_[taskIndex];
        }

        qint64 chunkSize = 0;
        const bool success = downloadChunk(task, stagingDir, &chunkSize);

        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            activeWorkers_--;

            {
                QMutexLocker stateLock(&stateMutex_);
                if (success) {
                    tasks_[taskIndex].completed = true;
                    tasks_[taskIndex].inProgress = false;
                    tasks_[taskIndex].size = chunkSize;
                } else {
                    tasks_[taskIndex].inProgress = false;
                    tasks_[taskIndex].retries++;
                    if (tasks_[taskIndex].retries <= 4 && !canceled_.load()) {
                        pendingTaskIndices_.push_back(taskIndex);
                    } else if (!failed_.load()) {
                        failed_ = true;
                        failureReason_ = QStringLiteral("Segment download failed after retries (%1)").arg(task.uri.toString());
                    }
                }
            }
            queueCv_.notify_all();
        }

        emitSegmentsSnapshot();
    }
}

bool HlsDownloader::assembleOutput(const QString& stagingDir, const QString& targetPath, bool hasSeparateAudio)
{
    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    const QFileInfo targetInfo(targetPath);
    QDir().mkpath(targetInfo.dir().path());

    QStringList videoFilenames;
    QStringList audioFilenames;
    {
        QMutexLocker lock(&stateMutex_);
        for (const auto& task : tasks_) {
            if (task.isAudio) {
                audioFilenames.append(task.outputFilename);
            } else {
                videoFilenames.append(task.outputFilename);
            }
        }
    }

    if (videoFilenames.isEmpty()) return false;

    if (!ffmpeg.isEmpty()) {
        if (hasSeparateAudio && !audioFilenames.isEmpty()) {
            const QString videoConcatPath = stagingDir + QStringLiteral("/concat_video.txt");
            const QString audioConcatPath = stagingDir + QStringLiteral("/concat_audio.txt");

            QFile vFile(videoConcatPath);
            if (vFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
                QTextStream out(&vFile);
                out << "ffconcat version 1.0\n";
                for (const auto& fn : videoFilenames) {
                    out << "file '" << fn << "'\n";
                }
                vFile.close();
            }

            QFile aFile(audioConcatPath);
            if (aFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
                QTextStream out(&aFile);
                out << "ffconcat version 1.0\n";
                for (const auto& fn : audioFilenames) {
                    out << "file '" << fn << "'\n";
                }
                aFile.close();
            }

            QProcess process;
            QStringList args = {
                QStringLiteral("-y"),
                QStringLiteral("-f"), QStringLiteral("concat"),
                QStringLiteral("-safe"), QStringLiteral("0"),
                QStringLiteral("-i"), videoConcatPath,
                QStringLiteral("-f"), QStringLiteral("concat"),
                QStringLiteral("-safe"), QStringLiteral("0"),
                QStringLiteral("-i"), audioConcatPath,
                QStringLiteral("-c"), QStringLiteral("copy")
            };
            if (targetPath.endsWith(QStringLiteral(".mp4"), Qt::CaseInsensitive)
                || targetPath.endsWith(QStringLiteral(".mov"), Qt::CaseInsensitive)) {
                args << QStringLiteral("-movflags") << QStringLiteral("+faststart");
            }
            args << targetPath;

            process.start(ffmpeg, args);
            if (process.waitForFinished(180000) && process.exitCode() == 0
                && QFile::exists(targetPath) && QFileInfo(targetPath).size() > 0) {
                return true;
            }
        } else {
            const QString concatFilePath = stagingDir + QStringLiteral("/concat.txt");
            QFile concatFile(concatFilePath);
            if (concatFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
                QTextStream out(&concatFile);
                out << "ffconcat version 1.0\n";
                for (const auto& filename : videoFilenames) {
                    out << "file '" << filename << "'\n";
                }
                concatFile.close();

                QProcess process;
                QStringList args = {
                    QStringLiteral("-y"),
                    QStringLiteral("-f"), QStringLiteral("concat"),
                    QStringLiteral("-safe"), QStringLiteral("0"),
                    QStringLiteral("-i"), concatFilePath,
                    QStringLiteral("-c"), QStringLiteral("copy")
                };
                if (targetPath.endsWith(QStringLiteral(".mp4"), Qt::CaseInsensitive)
                    || targetPath.endsWith(QStringLiteral(".mov"), Qt::CaseInsensitive)) {
                    args << QStringLiteral("-movflags") << QStringLiteral("+faststart");
                }
                args << targetPath;

                process.start(ffmpeg, args);
                if (process.waitForFinished(180000) && process.exitCode() == 0
                    && QFile::exists(targetPath) && QFileInfo(targetPath).size() > 0) {
                    return true;
                }
            }
        }
    }

    // Direct binary concatenation fallback (standard for MPEG-TS streams or when ffmpeg is not available)
    QFile outFile(targetPath);
    if (!outFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    for (const auto& filename : videoFilenames) {
        QFile chunk(stagingDir + QLatin1Char('/') + filename);
        if (chunk.open(QIODevice::ReadOnly)) {
            while (!chunk.atEnd()) {
                const QByteArray buffer = chunk.read(64 * 1024);
                outFile.write(buffer);
            }
        }
    }
    outFile.close();
    return QFile::exists(targetPath) && QFileInfo(targetPath).size() > 0;
}

void HlsDownloader::runEngine()
{
    segmentsTimer_.start();
    emit statusChanged(id_, DownloadStatus::Connecting, QStringLiteral("Fetching master playlist..."));

    QString manifestText;
    QUrl effectiveUrl;
    if (!fetchPlaylist(initialUrl_, &manifestText, &effectiveUrl)) {
        emit statusChanged(id_, DownloadStatus::Failed, QStringLiteral("Failed to fetch HLS playlist manifest."));
        running_ = false;
        return;
    }

    auto playlist = HlsPlaylistParser::parse(manifestText, effectiveUrl);
    QUrl audioPlaylistUrl;
    bool hasSeparateAudio = false;

    if (playlist.isMaster) {
        const int bestIndex = HlsPlaylistParser::selectBestVariant(playlist.variants);
        if (bestIndex < 0 || bestIndex >= playlist.variants.size()) {
            emit statusChanged(id_, DownloadStatus::Failed, QStringLiteral("No valid variant stream in master playlist."));
            running_ = false;
            return;
        }

        const auto& selectedVariant = playlist.variants[bestIndex];
        const QUrl variantUrl = selectedVariant.uri;

        if (!selectedVariant.audioGroup.isEmpty()) {
            audioPlaylistUrl = HlsPlaylistParser::selectAudioRenditionUrl(playlist, selectedVariant.audioGroup);
            if (audioPlaylistUrl.isValid()) {
                hasSeparateAudio = true;
            }
        }

        emit statusChanged(id_, DownloadStatus::Connecting, QStringLiteral("Fetching media playlist..."));
        if (!fetchPlaylist(variantUrl, &manifestText, &effectiveUrl)) {
            emit statusChanged(id_, DownloadStatus::Failed, QStringLiteral("Failed to fetch media playlist."));
            running_ = false;
            return;
        }
        playlist = HlsPlaylistParser::parse(manifestText, effectiveUrl);
    }

    if (playlist.segments.isEmpty()) {
        emit statusChanged(id_, DownloadStatus::Failed, QStringLiteral("Media playlist contains no downloadable segments."));
        running_ = false;
        return;
    }

    if (playlist.isLive) {
        emit statusChanged(id_, DownloadStatus::Failed, QStringLiteral("Live HLS streams require continuous stream recorder."));
        running_ = false;
        return;
    }

    HlsPlaylist audioPlaylist;
    if (hasSeparateAudio) {
        QString audioManifestText;
        QUrl audioEffectiveUrl;
        if (fetchPlaylist(audioPlaylistUrl, &audioManifestText, &audioEffectiveUrl)) {
            audioPlaylist = HlsPlaylistParser::parse(audioManifestText, audioEffectiveUrl);
            if (audioPlaylist.segments.isEmpty()) {
                hasSeparateAudio = false;
            }
        } else {
            hasSeparateAudio = false;
        }
    }

    const QFileInfo targetInfo(targetPath_);
    const QString stagingDir = targetInfo.dir().filePath(QStringLiteral(".%1.hls_parts").arg(id_));
    QDir().mkpath(stagingDir);

    // Build chunk tasks
    {
        QMutexLocker lock(&stateMutex_);
        tasks_.clear();
        downloadedBytes_ = 0;

        int taskCounter = 0;
        // Video init segment if present
        if (playlist.segments.first().initSegment.uri.isValid()) {
            ChunkTask initTask;
            initTask.index = taskCounter++;
            initTask.uri = playlist.segments.first().initSegment.uri;
            initTask.outputFilename = hasSeparateAudio ? QStringLiteral("video_init.mp4") : QStringLiteral("init.mp4");
            initTask.byteRangeOffset = playlist.segments.first().initSegment.byteRangeOffset;
            initTask.byteRangeLength = playlist.segments.first().initSegment.byteRangeLength;
            initTask.isInitSegment = true;
            initTask.isAudio = false;
            tasks_.append(initTask);
        }

        // Video chunks
        for (int i = 0; i < playlist.segments.size(); ++i) {
            const auto& seg = playlist.segments[i];
            ChunkTask task;
            task.index = taskCounter++;
            task.uri = seg.uri;
            const QString ext = seg.uri.path().endsWith(QStringLiteral(".m4s"), Qt::CaseInsensitive)
                ? QStringLiteral(".m4s") : QStringLiteral(".ts");
            task.outputFilename = hasSeparateAudio
                ? QStringLiteral("video_chunk_%1%2").arg(i, 5, 10, QLatin1Char('0')).arg(ext)
                : QStringLiteral("chunk_%1%2").arg(i, 5, 10, QLatin1Char('0')).arg(ext);
            task.byteRangeOffset = seg.byteRangeOffset;
            task.byteRangeLength = seg.byteRangeLength;
            task.key = seg.key;
            task.isAudio = false;
            tasks_.append(task);
        }

        // Audio chunks if separate
        if (hasSeparateAudio) {
            if (audioPlaylist.segments.first().initSegment.uri.isValid()) {
                ChunkTask audioInitTask;
                audioInitTask.index = taskCounter++;
                audioInitTask.uri = audioPlaylist.segments.first().initSegment.uri;
                audioInitTask.outputFilename = QStringLiteral("audio_init.mp4");
                audioInitTask.byteRangeOffset = audioPlaylist.segments.first().initSegment.byteRangeOffset;
                audioInitTask.byteRangeLength = audioPlaylist.segments.first().initSegment.byteRangeLength;
                audioInitTask.isInitSegment = true;
                audioInitTask.isAudio = true;
                tasks_.append(audioInitTask);
            }

            for (int i = 0; i < audioPlaylist.segments.size(); ++i) {
                const auto& seg = audioPlaylist.segments[i];
                ChunkTask task;
                task.index = taskCounter++;
                task.uri = seg.uri;
                const QString ext = seg.uri.path().endsWith(QStringLiteral(".m4s"), Qt::CaseInsensitive)
                    ? QStringLiteral(".m4s") : (seg.uri.path().endsWith(QStringLiteral(".aac"), Qt::CaseInsensitive) ? QStringLiteral(".aac") : QStringLiteral(".ts"));
                task.outputFilename = QStringLiteral("audio_chunk_%1%2").arg(i, 5, 10, QLatin1Char('0')).arg(ext);
                task.byteRangeOffset = seg.byteRangeOffset;
                task.byteRangeLength = seg.byteRangeLength;
                task.key = seg.key;
                task.isAudio = true;
                tasks_.append(task);
            }
        }

        // Check already completed tasks for resumption
        for (auto& task : tasks_) {
            const QString chunkPath = stagingDir + QLatin1Char('/') + task.outputFilename;
            if (QFile::exists(chunkPath)) {
                const qint64 size = QFileInfo(chunkPath).size();
                if (size > 0) {
                    task.completed = true;
                    task.size = size;
                    downloadedBytes_ += size;
                }
            }
        }
    }

    emit statusChanged(id_, DownloadStatus::Downloading, QString());
    emitSegmentsSnapshot(true);

    speedTimer_.start();
    lastSpeedBytes_ = downloadedBytes_.load();

    // Populate queue and spawn persistent worker pool
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        pendingTaskIndices_.clear();
        for (int i = 0; i < tasks_.size(); ++i) {
            if (!tasks_[i].completed) {
                pendingTaskIndices_.push_back(i);
            }
        }
        activeWorkers_ = 0;
        failed_ = false;
        failureReason_.clear();
    }

    const int totalTasks = tasks_.size();
    const int effectiveConcurrency = qMin(concurrency_, qMax(1, static_cast<int>(pendingTaskIndices_.size())));

    workerPool_.clear();
    workerPool_.reserve(effectiveConcurrency);
    for (int i = 0; i < effectiveConcurrency; ++i) {
        workerPool_.emplace_back([this, stagingDir]() {
            workerLoop(stagingDir);
        });
    }

    // Monitor loop
    while (running_.load() && !canceled_.load() && !failed_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));

        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            if (pendingTaskIndices_.empty() && activeWorkers_ == 0) {
                break;
            }
        }

        if (speedTimer_.elapsed() >= 500) {
            const qint64 currentBytes = downloadedBytes_.load();
            const qint64 elapsedMs = speedTimer_.elapsed();
            currentSpeed_ = static_cast<double>(currentBytes - lastSpeedBytes_) / (static_cast<double>(elapsedMs) / 1000.0);
            lastSpeedBytes_ = currentBytes;
            speedTimer_.restart();

            int completedCount = 0;
            {
                QMutexLocker lock(&stateMutex_);
                for (const auto& t : tasks_) {
                    if (t.completed) completedCount++;
                }
            }
            const qint64 totalEst = (completedCount > 0)
                ? (currentBytes * totalTasks / completedCount)
                : 0;

            emit progressChanged(id_, currentBytes, totalEst, currentSpeed_);
        }
    }

    // Wake and join worker threads
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        queueCv_.notify_all();
    }
    for (auto& w : workerPool_) {
        if (w.joinable()) {
            w.join();
        }
    }
    workerPool_.clear();

    if (canceled_.load()) {
        QDir(stagingDir).removeRecursively();
        emit statusChanged(id_, DownloadStatus::Canceled, QStringLiteral("Canceled"));
        running_ = false;
        return;
    }

    if (failed_.load()) {
        emit statusChanged(id_, DownloadStatus::Failed, failureReason_);
        running_ = false;
        return;
    }

    if (paused_.load()) {
        running_ = false;
        return;
    }

    // Assembly Phase
    emit statusChanged(id_, DownloadStatus::Connecting, QStringLiteral("Losslessly assembling media container..."));
    if (assembleOutput(stagingDir, targetPath_, hasSeparateAudio)) {
        QDir(stagingDir).removeRecursively();
        emitSegmentsSnapshot(true);
        emit progressChanged(id_, downloadedBytes_.load(), downloadedBytes_.load(), 0.0);
        emit statusChanged(id_, DownloadStatus::Completed, QStringLiteral("Completed"));
    } else {
        emit statusChanged(id_, DownloadStatus::Failed, QStringLiteral("Failed to assemble media segments into target file."));
    }

    running_ = false;
}

} // namespace qtidm
