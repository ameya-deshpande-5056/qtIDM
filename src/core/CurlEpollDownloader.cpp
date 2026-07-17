#include "core/CurlEpollDownloader.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFileInfo>
#include <QMutexLocker>
#include <QUrl>
#include <curl/curl.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <algorithm>
#include <utility>

namespace qtidm {

struct CurlEpollDownloader::DownloadBatch {
    QString id;
    DownloadRequest request;
    std::shared_ptr<SparseFileWriter> writer;
    QVector<SegmentInfo> segments;
    qint64 total = -1;
    qint64 lastReceived = 0;
    qint64 lastTick = 0;
    int activeSegments = 0;
    bool failed = false;
};

struct CurlEpollDownloader::SegmentTransfer {
    std::shared_ptr<DownloadBatch> batch;
    int segmentIndex = 0;
    qint64 start = 0;
    qint64 end = 0;
    qint64 written = 0;
    QByteArray urlBytes;
    QByteArray rangeBytes;
    QByteArray userPwdBytes;
    QByteArray proxyBytes;
    curl_slist* headers = nullptr;

    ~SegmentTransfer()
    {
        curl_slist_free_all(headers);
    }
};

namespace {
QString makeId(const QUrl& url, const QString& target)
{
    const auto seed = url.toString() + QLatin1Char('|') + target + QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    return QString::fromLatin1(QCryptographicHash::hash(seed.toUtf8(), QCryptographicHash::Sha256).toHex().left(16));
}

int epollEventsFromCurl(int action)
{
    int events = 0;
    if (action & CURL_POLL_IN) {
        events |= EPOLLIN;
    }
    if (action & CURL_POLL_OUT) {
        events |= EPOLLOUT;
    }
    return events;
}

qint64 batchReceived(const CurlEpollDownloader::DownloadBatch& batch)
{
    qint64 received = 0;
    for (const auto& segment : batch.segments) {
        received += segment.written;
    }
    return received;
}

void applyRequestOptions(CURL* easy, const DownloadRequest& request, CurlEpollDownloader::SegmentTransfer* transfer)
{
    transfer->urlBytes = request.url.toString().toUtf8();
    curl_easy_setopt(easy, CURLOPT_URL, transfer->urlBytes.constData());
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, 15000L);
    curl_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(easy, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(easy, CURLOPT_FTP_RESPONSE_TIMEOUT, 30L);
    curl_easy_setopt(easy, CURLOPT_USE_SSL, CURLUSESSL_TRY);

    if (!request.username.isEmpty() || !request.password.isEmpty()) {
        transfer->userPwdBytes = (request.username + QLatin1Char(':') + request.password).toUtf8();
        curl_easy_setopt(easy, CURLOPT_USERPWD, transfer->userPwdBytes.constData());
        curl_easy_setopt(easy, CURLOPT_HTTPAUTH, CURLAUTH_ANY);
    }
    if (!request.proxyUrl.isEmpty()) {
        transfer->proxyBytes = request.proxyUrl.toUtf8();
        curl_easy_setopt(easy, CURLOPT_PROXY, transfer->proxyBytes.constData());
    }
    if (request.speedLimitBytesPerSecond > 0) {
        curl_easy_setopt(easy, CURLOPT_MAX_RECV_SPEED_LARGE, static_cast<curl_off_t>(request.speedLimitBytesPerSecond));
    }
    for (auto it = request.headers.cbegin(); it != request.headers.cend(); ++it) {
        const auto line = it.key().toUtf8() + ": " + it.value().toString().toUtf8();
        transfer->headers = curl_slist_append(transfer->headers, line.constData());
    }
    if (transfer->headers) {
        curl_easy_setopt(easy, CURLOPT_HTTPHEADER, transfer->headers);
    }
}
}

CurlEpollDownloader::CurlEpollDownloader(QObject* parent)
    : QObject(parent)
{
}

CurlEpollDownloader::~CurlEpollDownloader()
{
    stop();
}

void CurlEpollDownloader::start()
{
    if (running_.exchange(true)) {
        return;
    }
    thread_ = std::jthread([this] { run(); });
}

void CurlEpollDownloader::stop()
{
    if (!running_.exchange(false)) {
        return;
    }
    wake();
    if (thread_.joinable()) {
        thread_.join();
    }
}

QString CurlEpollDownloader::enqueue(DownloadRequest request)
{
    const auto id = request.existingId.isEmpty() ? makeId(request.url, request.targetPath) : request.existingId;
    {
        QMutexLocker lock(&pendingMutex_);
        pending_.enqueue(QueuedRequest { id, std::move(request) });
    }
    wake();
    return id;
}

void CurlEpollDownloader::pause(const QString& id)
{
    QMutexLocker lock(&controlMutex_);
    controls_.insert(id, DownloadStatus::Paused);
    wake();
}

void CurlEpollDownloader::cancel(const QString& id)
{
    QMutexLocker lock(&controlMutex_);
    controls_.insert(id, DownloadStatus::Canceled);
    wake();
}

int CurlEpollDownloader::socketCallback(CURL*, curl_socket_t socket, int what, void* userp, void*)
{
    auto* self = static_cast<CurlEpollDownloader*>(userp);
    if (what == CURL_POLL_REMOVE) {
        self->removeSocket(socket);
    } else {
        self->updateSocket(socket, what);
    }
    return 0;
}

int CurlEpollDownloader::timerCallback(CURLM*, long, void* userp)
{
    static_cast<CurlEpollDownloader*>(userp)->wake();
    return 0;
}

size_t CurlEpollDownloader::writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    const auto bytes = static_cast<qint64>(size * nmemb);
    auto* transfer = static_cast<SegmentTransfer*>(userdata);
    const auto offset = transfer->start + transfer->written;
    if (!transfer->batch->writer->writeAt(offset, ptr, bytes)) {
        return 0;
    }
    transfer->written += bytes;
    auto& segment = transfer->batch->segments[transfer->segmentIndex];
    segment.written = transfer->written;
    segment.status = DownloadStatus::Downloading;
    return static_cast<size_t>(bytes);
}

int CurlEpollDownloader::progressCallback(void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    return 0;
}

void CurlEpollDownloader::run()
{
    curl_global_init(CURL_GLOBAL_DEFAULT);
    multi_ = curl_multi_init();
    epollFd_ = epoll_create1(EPOLL_CLOEXEC);
    wakeFd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

    curl_multi_setopt(multi_, CURLMOPT_SOCKETFUNCTION, &CurlEpollDownloader::socketCallback);
    curl_multi_setopt(multi_, CURLMOPT_SOCKETDATA, this);
    curl_multi_setopt(multi_, CURLMOPT_TIMERFUNCTION, &CurlEpollDownloader::timerCallback);
    curl_multi_setopt(multi_, CURLMOPT_TIMERDATA, this);

    epoll_event wakeEvent {};
    wakeEvent.events = EPOLLIN;
    wakeEvent.data.fd = wakeFd_;
    epoll_ctl(epollFd_, EPOLL_CTL_ADD, wakeFd_, &wakeEvent);

    int active = 0;
    while (running_) {
        addPendingTransfers();
        applyControlRequests();
        epoll_event events[64] {};
        const int count = epoll_wait(epollFd_, events, 64, 1000);
        if (count == 0) {
            curl_multi_socket_action(multi_, CURL_SOCKET_TIMEOUT, 0, &active);
            checkCompleted();
            applyControlRequests();
            continue;
        }
        for (int i = 0; i < count; ++i) {
            if (events[i].data.fd == wakeFd_) {
                eventfd_t value = 0;
                eventfd_read(wakeFd_, &value);
                curl_multi_socket_action(multi_, CURL_SOCKET_TIMEOUT, 0, &active);
            } else {
                int action = 0;
                if (events[i].events & EPOLLIN) action |= CURL_CSELECT_IN;
                if (events[i].events & EPOLLOUT) action |= CURL_CSELECT_OUT;
                if (events[i].events & EPOLLERR) action |= CURL_CSELECT_ERR;
                curl_multi_socket_action(multi_, events[i].data.fd, action, &active);
            }
            checkCompleted();
            applyControlRequests();
        }
    }

    for (const auto& [easy, transfer] : transfers_) {
        curl_multi_remove_handle(multi_, easy);
        curl_easy_cleanup(easy);
    }
    transfers_.clear();
    batches_.clear();
    curl_multi_cleanup(multi_);
    multi_ = nullptr;
    close(wakeFd_);
    close(epollFd_);
    curl_global_cleanup();
}

void CurlEpollDownloader::wake()
{
    if (wakeFd_ >= 0) {
        eventfd_write(wakeFd_, 1);
    }
}

void CurlEpollDownloader::addPendingTransfers()
{
    QQueue<QueuedRequest> local;
    {
        QMutexLocker lock(&pendingMutex_);
        std::swap(local, pending_);
    }
    while (!local.isEmpty()) {
        addTransfer(local.dequeue());
    }
}

void CurlEpollDownloader::addTransfer(QueuedRequest queued)
{
    bool rangeSupported = false;
    auto total = probeSize(queued.request, &rangeSupported);
    if (queued.request.expectedTotalBytes > 0 && total > 0 && queued.request.expectedTotalBytes != total) {
        emit statusChanged(queued.id, DownloadStatus::Failed, QStringLiteral("remote size changed; resume refused"));
        return;
    }
    if (total <= 0 && queued.request.expectedTotalBytes > 0) {
        total = queued.request.expectedTotalBytes;
    }
    const int requestedSegments = std::clamp(queued.request.segments, 1, 32);
    const bool canResumeSegments = rangeSupported && total > 0 && !queued.request.resumeSegments.isEmpty();
    const int segmentCount = canResumeSegments ? queued.request.resumeSegments.size() : (rangeSupported && total > 0 ? requestedSegments : 1);

    auto batch = std::make_shared<DownloadBatch>();
    batch->id = queued.id;
    batch->request = queued.request;
    batch->writer = std::make_shared<SparseFileWriter>();
    batch->total = total;
    batch->lastTick = QDateTime::currentMSecsSinceEpoch();
    batch->activeSegments = segmentCount;
    if (!batch->writer->open(queued.request.targetPath, total)) {
        emit statusChanged(batch->id, DownloadStatus::Failed, batch->writer->lastError());
        return;
    }

    DownloadRecord record;
    record.id = batch->id;
    record.url = queued.request.url;
    record.targetPath = queued.request.targetPath;
    record.category = queued.request.category;
    record.totalBytes = total;
    record.status = DownloadStatus::Downloading;
    record.createdAt = QDateTime::currentDateTimeUtc();
    record.updatedAt = record.createdAt;

    const qint64 segmentSize = total > 0 ? ((total + segmentCount - 1) / segmentCount) : -1;
    for (int index = 0; index < segmentCount; ++index) {
        SegmentInfo segment;
        if (canResumeSegments) {
            segment = queued.request.resumeSegments[index];
        } else {
            segment.index = index;
            segment.start = total > 0 ? index * segmentSize : 0;
            segment.end = total > 0 ? qMin(total - 1, segment.start + segmentSize - 1) : -1;
            segment.status = DownloadStatus::Queued;
        }
        batch->segments.append(segment);
        record.segments.append(segment);
    }

    batches_.insert(batch->id, batch);
    emit downloadAdded(record);
    emit statusChanged(batch->id, DownloadStatus::Downloading, {});

    for (const auto& segment : std::as_const(batch->segments)) {
        if (segment.end >= segment.start && segment.written >= segment.end - segment.start + 1) {
            batch->activeSegments--;
            continue;
        }
        auto transfer = std::make_unique<SegmentTransfer>();
        transfer->batch = batch;
        transfer->segmentIndex = segment.index;
        transfer->start = segment.start;
        transfer->end = segment.end;
        transfer->written = segment.written;

        auto* easy = curl_easy_init();
        applyRequestOptions(easy, queued.request, transfer.get());
        const auto rangeStart = segment.start + segment.written;
        if (segment.end >= rangeStart && segmentCount > 1) {
            transfer->rangeBytes = QByteArray::number(rangeStart) + "-" + QByteArray::number(segment.end);
            curl_easy_setopt(easy, CURLOPT_RANGE, transfer->rangeBytes.constData());
        }
        curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, &CurlEpollDownloader::writeCallback);
        curl_easy_setopt(easy, CURLOPT_WRITEDATA, transfer.get());
        curl_easy_setopt(easy, CURLOPT_XFERINFOFUNCTION, &CurlEpollDownloader::progressCallback);
        curl_easy_setopt(easy, CURLOPT_XFERINFODATA, transfer.get());
        curl_easy_setopt(easy, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(easy, CURLOPT_PRIVATE, easy);
        batch->segments[segment.index].status = DownloadStatus::Downloading;
        transfers_.emplace(easy, std::move(transfer));
        curl_multi_add_handle(multi_, easy);
    }
    emit segmentsChanged(batch->id, batch->segments);
    if (batch->activeSegments == 0) {
        batch->writer->close();
        emit progressChanged(batch->id, batchReceived(*batch), batch->total, 0.0);
        emit statusChanged(batch->id, DownloadStatus::Completed, {});
        batches_.remove(batch->id);
    }
}

void CurlEpollDownloader::updateSocket(curl_socket_t socket, int action)
{
    epoll_event event {};
    event.events = epollEventsFromCurl(action) | EPOLLET;
    event.data.fd = socket;
    const bool known = sockets_.contains(socket);
    epoll_ctl(epollFd_, known ? EPOLL_CTL_MOD : EPOLL_CTL_ADD, socket, &event);
    sockets_[socket] = event.events;
}

void CurlEpollDownloader::removeSocket(curl_socket_t socket)
{
    epoll_ctl(epollFd_, EPOLL_CTL_DEL, socket, nullptr);
    sockets_.erase(socket);
}

qint64 CurlEpollDownloader::probeSize(const DownloadRequest& request, bool* rangeSupported)
{
    *rangeSupported = false;
    auto transfer = std::make_unique<SegmentTransfer>();
    auto* easy = curl_easy_init();
    applyRequestOptions(easy, request, transfer.get());
    curl_easy_setopt(easy, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(easy, CURLOPT_HEADER, 1L);
    const auto rc = curl_easy_perform(easy);
    curl_off_t contentLength = -1;
    curl_easy_getinfo(easy, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &contentLength);
    long response = 0;
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &response);
    curl_easy_cleanup(easy);
    if (rc != CURLE_OK || response >= 400) {
        return -1;
    }

    auto rangeProbe = std::make_unique<SegmentTransfer>();
    easy = curl_easy_init();
    applyRequestOptions(easy, request, rangeProbe.get());
    rangeProbe->rangeBytes = "0-0";
    curl_easy_setopt(easy, CURLOPT_RANGE, rangeProbe->rangeBytes.constData());
    curl_easy_setopt(easy, CURLOPT_NOBODY, 1L);
    rc = curl_easy_perform(easy);
    response = 0;
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &response);
    curl_easy_cleanup(easy);
    *rangeSupported = rc == CURLE_OK && response == 206;
    return contentLength > 0 ? static_cast<qint64>(contentLength) : -1;
}

void CurlEpollDownloader::applyControlRequests()
{
    QHash<QString, DownloadStatus> controls;
    {
        QMutexLocker lock(&controlMutex_);
        controls.swap(controls_);
    }
    if (controls.isEmpty()) {
        return;
    }

    for (auto it = transfers_.begin(); it != transfers_.end();) {
        const auto batch = it->second->batch;
        if (!controls.contains(batch->id)) {
            ++it;
            continue;
        }
        auto* easy = it->first;
        curl_multi_remove_handle(multi_, easy);
        curl_easy_cleanup(easy);
        auto& segment = batch->segments[it->second->segmentIndex];
        segment.status = controls.value(batch->id);
        batch->activeSegments--;
        it = transfers_.erase(it);
    }

    for (auto controlIt = controls.cbegin(); controlIt != controls.cend(); ++controlIt) {
        const auto batch = batches_.value(controlIt.key());
        if (!batch) {
            continue;
        }
        batch->writer->close();
        emit segmentsChanged(batch->id, batch->segments);
        emit statusChanged(batch->id, controlIt.value(), {});
        batches_.remove(batch->id);
    }
}

void CurlEpollDownloader::checkCompleted()
{
    int pending = 0;
    while (auto* message = curl_multi_info_read(multi_, &pending)) {
        if (message->msg != CURLMSG_DONE) {
            continue;
        }
        auto* easy = message->easy_handle;
        auto transferIt = transfers_.find(easy);
        if (transferIt == transfers_.end()) {
            continue;
        }
        auto transfer = std::move(transferIt->second);
        transfers_.erase(transferIt);
        curl_multi_remove_handle(multi_, easy);
        curl_easy_cleanup(easy);

        auto batch = transfer->batch;
        auto& segment = batch->segments[transfer->segmentIndex];
        batch->activeSegments--;
        if (message->data.result == CURLE_OK) {
            segment.status = DownloadStatus::Completed;
        } else {
            batch->failed = true;
            segment.status = DownloadStatus::Failed;
            emit statusChanged(batch->id, DownloadStatus::Failed, QString::fromUtf8(curl_easy_strerror(message->data.result)));
        }

        emit segmentsChanged(batch->id, batch->segments);
        if (batch->activeSegments == 0) {
            batch->writer->close();
            const auto received = batchReceived(*batch);
            emit progressChanged(batch->id, received, batch->total, 0.0);
            emit statusChanged(batch->id, batch->failed ? DownloadStatus::Failed : DownloadStatus::Completed, {});
            batches_.remove(batch->id);
        }
    }

    const auto now = QDateTime::currentMSecsSinceEpoch();
    for (const auto& batch : std::as_const(batches_)) {
        if (now - batch->lastTick < 250) {
            continue;
        }
        const auto received = batchReceived(*batch);
        const auto deltaBytes = received - batch->lastReceived;
        const auto deltaMs = now - batch->lastTick;
        const double bps = deltaMs > 0 ? (deltaBytes * 1000.0 / deltaMs) : 0.0;
        batch->lastReceived = received;
        batch->lastTick = now;
        emit progressChanged(batch->id, received, batch->total, bps);
        emit segmentsChanged(batch->id, batch->segments);
    }
}

}
