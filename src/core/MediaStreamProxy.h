#pragma once

#include <QString>
#include <QUrl>
#include <QVariantMap>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace qtidm {

class MediaStreamProxy final {
public:
    MediaStreamProxy();
    ~MediaStreamProxy();

    MediaStreamProxy(const MediaStreamProxy&) = delete;
    MediaStreamProxy& operator=(const MediaStreamProxy&) = delete;

    bool start(const QUrl& rootUrl, const QVariantMap& headers);
    void stop();

    bool isRunning() const;
    quint16 port() const;
    QUrl proxyUrl() const;

private:
    void acceptLoop();
    void handleClient(int clientFd);
    std::string rewriteM3u8(const std::string& manifestData, const QUrl& effectiveUrl);

    QUrl rootUrl_;
    QVariantMap headers_;
    int listenFd_ = -1;
    quint16 port_ = 0;
    std::atomic_bool running_ { false };
    std::jthread acceptThread_;

    std::mutex mapMutex_;
    std::unordered_map<std::string, std::string> urlMap_;
    int nextId_ = 0;
};

}
