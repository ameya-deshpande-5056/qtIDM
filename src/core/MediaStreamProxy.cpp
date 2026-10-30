#include "core/MediaStreamProxy.h"

#include <arpa/inet.h>
#include <curl/curl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>

namespace qtidm {

MediaStreamProxy::MediaStreamProxy()
{
}

MediaStreamProxy::~MediaStreamProxy()
{
    stop();
}

bool MediaStreamProxy::isRunning() const
{
    return running_.load();
}

quint16 MediaStreamProxy::port() const
{
    return port_;
}

QUrl MediaStreamProxy::proxyUrl() const
{
    const QString ext = rootUrl_.path().endsWith(QStringLiteral(".mpd"), Qt::CaseInsensitive)
        ? QStringLiteral("/start.mpd")
        : QStringLiteral("/start.m3u8");
    return QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(port_).arg(ext));
}

bool MediaStreamProxy::start(const QUrl& rootUrl, const QVariantMap& headers)
{
    stop();
    rootUrl_ = rootUrl;
    headers_ = headers;

    listenFd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listenFd_ < 0) {
        return false;
    }

    int opt = 1;
    ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    if (::bind(listenFd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }

    if (::listen(listenFd_, 64) != 0) {
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }

    socklen_t addrLen = sizeof(addr);
    if (::getsockname(listenFd_, reinterpret_cast<struct sockaddr*>(&addr), &addrLen) == 0) {
        port_ = ntohs(addr.sin_port);
    } else {
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }

    running_ = true;
    acceptThread_ = std::jthread([this] { acceptLoop(); });
    return true;
}

void MediaStreamProxy::stop()
{
    if (!running_.exchange(false)) {
        return;
    }
    if (listenFd_ >= 0) {
        ::shutdown(listenFd_, SHUT_RDWR);
        ::close(listenFd_);
        listenFd_ = -1;
    }
    if (acceptThread_.joinable()) {
        acceptThread_.join();
    }
    std::lock_guard<std::mutex> lock(mapMutex_);
    urlMap_.clear();
    port_ = 0;
}

void MediaStreamProxy::acceptLoop()
{
    while (running_) {
        int clientFd = ::accept4(listenFd_, nullptr, nullptr, SOCK_CLOEXEC);
        if (clientFd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        std::thread([this, clientFd] {
            handleClient(clientFd);
        }).detach();
    }
}

void MediaStreamProxy::handleClient(int clientFd)
{
    struct SocketCloser {
        int fd;
        ~SocketCloser() { if (fd >= 0) ::close(fd); }
    } closer { clientFd };

    std::string request;
    char buf[4096];
    while (request.find("\r\n\r\n") == std::string::npos && request.size() < 65536) {
        ssize_t n = ::recv(clientFd, buf, sizeof(buf), 0);
        if (n <= 0) return;
        request.append(buf, static_cast<size_t>(n));
    }

    const auto firstLineEnd = request.find("\r\n");
    if (firstLineEnd == std::string::npos) return;
    const std::string requestLine = request.substr(0, firstLineEnd);

    std::string method, path, proto;
    {
        std::istringstream iss(requestLine);
        iss >> method >> path >> proto;
    }
    if (method != "GET" && method != "HEAD") {
        const std::string resp = "HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        ::send(clientFd, resp.data(), resp.size(), MSG_NOSIGNAL);
        return;
    }

    std::string rangeHeader;
    {
        const std::string rangePrefix = "\r\nrange:";
        std::string lowerReq = request;
        std::transform(lowerReq.begin(), lowerReq.end(), lowerReq.begin(), [](unsigned char c) { return std::tolower(c); });
        auto pos = lowerReq.find(rangePrefix);
        if (pos != std::string::npos) {
            pos += rangePrefix.size();
            auto endPos = lowerReq.find("\r\n", pos);
            if (endPos != std::string::npos) {
                rangeHeader = request.substr(pos, endPos - pos);
                rangeHeader.erase(0, rangeHeader.find_first_not_of(" \t"));
                rangeHeader.erase(rangeHeader.find_last_not_of(" \t") + 1);
                if (rangeHeader.rfind("bytes=", 0) == 0) {
                    rangeHeader = rangeHeader.substr(6);
                }
            }
        }
    }

    std::string targetUrlStr;
    bool isStart = false;
    {
        std::lock_guard<std::mutex> lock(mapMutex_);
        if (path == "/start.m3u8" || path == "/start.mpd") {
            targetUrlStr = rootUrl_.toString().toStdString();
            isStart = true;
        } else {
            auto it = urlMap_.find(path);
            if (it != urlMap_.end()) {
                targetUrlStr = it->second;
            } else {
                QUrl resolved = rootUrl_.resolved(QUrl(QString::fromStdString(path)));
                targetUrlStr = resolved.toString().toStdString();
            }
        }
    }

    if (targetUrlStr.empty()) {
        const std::string resp = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        ::send(clientFd, resp.data(), resp.size(), MSG_NOSIGNAL);
        return;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        const std::string resp = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        ::send(clientFd, resp.data(), resp.size(), MSG_NOSIGNAL);
        return;
    }

    struct CurlGuard {
        CURL* c;
        struct curl_slist* headers = nullptr;
        ~CurlGuard() {
            if (headers) curl_slist_free_all(headers);
            if (c) curl_easy_cleanup(c);
        }
    } curlGuard { curl };

    curl_easy_setopt(curl, CURLOPT_URL, targetUrlStr.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_AUTOREFERER, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);

    struct curl_slist* reqHeaders = nullptr;
    for (auto it = headers_.cbegin(); it != headers_.cend(); ++it) {
        if (it.key().startsWith(QLatin1Char('_'))) continue;
        const auto line = it.key() + QStringLiteral(": ") + it.value().toString();
        reqHeaders = curl_slist_append(reqHeaders, line.toUtf8().constData());
    }
    if (reqHeaders) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, reqHeaders);
        curlGuard.headers = reqHeaders;
    }

    if (!rangeHeader.empty()) {
        curl_easy_setopt(curl, CURLOPT_RANGE, rangeHeader.c_str());
    }

    struct StreamState {
        int fd = -1;
        bool isM3u8 = false;
        std::string buffer;
        bool headerSent = false;
        long responseCode = 0;
        curl_off_t contentLength = -1;
    };
    StreamState state;
    state.fd = clientFd;
    state.isM3u8 = isStart || path.find(".m3u8") != std::string::npos;

    auto writeCb = [](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
        auto* s = static_cast<StreamState*>(userdata);
        size_t total = size * nmemb;
        if (s->isM3u8) {
            s->buffer.append(ptr, total);
            return total;
        }
        if (!s->headerSent) {
            s->headerSent = true;
            std::string statusLine = (s->responseCode == 206)
                ? "HTTP/1.1 206 Partial Content\r\n"
                : "HTTP/1.1 200 OK\r\n";
            std::string resp = statusLine;
            if (s->contentLength > 0) {
                resp += "Content-Length: " + std::to_string(s->contentLength) + "\r\n";
            }
            resp += "Connection: close\r\n\r\n";
            if (::send(s->fd, resp.data(), resp.size(), MSG_NOSIGNAL) < 0) {
                return 0;
            }
        }
        ssize_t sent = ::send(s->fd, ptr, total, MSG_NOSIGNAL);
        return (sent > 0) ? static_cast<size_t>(sent) : 0;
    };

    auto headerCb = [](char* buffer, size_t size, size_t nitems, void* userdata) -> size_t {
        auto* s = static_cast<StreamState*>(userdata);
        size_t total = size * nitems;
        std::string line(buffer, total);
        std::string lowerLine = line;
        std::transform(lowerLine.begin(), lowerLine.end(), lowerLine.begin(), [](unsigned char c) { return std::tolower(c); });
        if (lowerLine.rfind("content-length:", 0) == 0) {
            try {
                s->contentLength = std::stoll(line.substr(15));
            } catch (...) {}
        }
        if (lowerLine.rfind("content-type:", 0) == 0) {
            if (lowerLine.find("mpegurl") != std::string::npos) {
                s->isM3u8 = true;
            }
        }
        return total;
    };

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +writeCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, +headerCb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &state);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &state.responseCode);

    if (state.isM3u8) {
        if (res == CURLE_OK && state.responseCode >= 200 && state.responseCode < 300) {
            char* effUrl = nullptr;
            curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effUrl);
            QUrl effectiveUrl = effUrl ? QUrl(QString::fromUtf8(effUrl)) : QUrl(QString::fromStdString(targetUrlStr));
            std::string rewritten = rewriteM3u8(state.buffer, effectiveUrl);
            std::string resp = "HTTP/1.1 200 OK\r\n"
                               "Content-Type: application/vnd.apple.mpegurl\r\n"
                               "Content-Length: " + std::to_string(rewritten.size()) + "\r\n"
                               "Connection: close\r\n\r\n";
            ::send(clientFd, resp.data(), resp.size(), MSG_NOSIGNAL);
            ::send(clientFd, rewritten.data(), rewritten.size(), MSG_NOSIGNAL);
        } else {
            std::string resp = "HTTP/1.1 " + std::to_string(state.responseCode > 0 ? state.responseCode : 502) + " Error\r\n"
                               "Content-Length: 0\r\nConnection: close\r\n\r\n";
            ::send(clientFd, resp.data(), resp.size(), MSG_NOSIGNAL);
        }
    } else if (!state.headerSent) {
        std::string resp = "HTTP/1.1 " + std::to_string(state.responseCode > 0 ? state.responseCode : 502) + " Error\r\n"
                           "Content-Length: 0\r\nConnection: close\r\n\r\n";
        ::send(clientFd, resp.data(), resp.size(), MSG_NOSIGNAL);
    }
}

std::string MediaStreamProxy::rewriteM3u8(const std::string& manifestData, const QUrl& effectiveUrl)
{
    std::istringstream stream(manifestData);
    std::string line;
    std::ostringstream out;

    std::lock_guard<std::mutex> lock(mapMutex_);
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            out << "\n";
            continue;
        }

        if (line.rfind("#EXT-X-KEY:", 0) == 0 && line.find("URI=\"") != std::string::npos) {
            auto uriPos = line.find("URI=\"") + 5;
            auto uriEnd = line.find("\"", uriPos);
            if (uriEnd != std::string::npos) {
                std::string rawUri = line.substr(uriPos, uriEnd - uriPos);
                QUrl absUri = effectiveUrl.resolved(QUrl(QString::fromUtf8(rawUri.c_str())));
                std::string path = "/key_" + std::to_string(nextId_++) + ".bin";
                urlMap_[path] = absUri.toString().toStdString();
                std::string proxiedUri = "http://127.0.0.1:" + std::to_string(port_) + path;
                line = line.substr(0, uriPos) + proxiedUri + line.substr(uriEnd);
            }
            out << line << "\n";
            continue;
        }

        if (line.rfind("#EXT-X-MAP:", 0) == 0 && line.find("URI=\"") != std::string::npos) {
            auto uriPos = line.find("URI=\"") + 5;
            auto uriEnd = line.find("\"", uriPos);
            if (uriEnd != std::string::npos) {
                std::string rawUri = line.substr(uriPos, uriEnd - uriPos);
                QUrl absUri = effectiveUrl.resolved(QUrl(QString::fromUtf8(rawUri.c_str())));
                std::string path = "/init_" + std::to_string(nextId_++) + ".mp4";
                urlMap_[path] = absUri.toString().toStdString();
                std::string proxiedUri = "http://127.0.0.1:" + std::to_string(port_) + path;
                line = line.substr(0, uriPos) + proxiedUri + line.substr(uriEnd);
            }
            out << line << "\n";
            continue;
        }

        if (line[0] != '#') {
            QUrl absUri = effectiveUrl.resolved(QUrl(QString::fromUtf8(line.c_str())));
            std::string lowerLine = line;
            std::transform(lowerLine.begin(), lowerLine.end(), lowerLine.begin(), [](unsigned char c) { return std::tolower(c); });
            std::string ext = (lowerLine.find(".m3u8") != std::string::npos) ? ".m3u8" : ".ts";
            std::string path = (ext == ".m3u8" ? "/variant_" : "/seg_") + std::to_string(nextId_++) + ext;
            urlMap_[path] = absUri.toString().toStdString();
            std::string proxiedUri = "http://127.0.0.1:" + std::to_string(port_) + path;
            out << proxiedUri << "\n";
            continue;
        }

        out << line << "\n";
    }

    return out.str();
}

}
