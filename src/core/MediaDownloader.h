#pragma once

#include "core/DownloadTypes.h"

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QStringList>
#include <memory>

class QProcess;
class QUrl;

namespace qtidm {

class MediaStreamProxy;

class MediaDownloader final : public QObject {
    Q_OBJECT
public:
    explicit MediaDownloader(QObject* parent = nullptr);
    ~MediaDownloader() override;

    static bool supports(const QUrl& url, const QString& mediaTypeHint = {});
    static bool declaresUnsupportedDrm(const QByteArray& manifest);
    static QString protocolWhitelist(const QUrl& url);
    static QStringList httpInputArguments(const QVariantMap& headers);
    static QString suggestedFilename(const QString& name);
    static QString mediaStagingPath(const QString& targetPath);
    QString enqueue(const QUrl& url, const QString& targetPath, const QVariantMap& headers, const QString& existingId = {},
        const QUrl& audioUrl = {}, const QVariantMap& audioHeaders = {});
    bool contains(const QString& id) const;
    void pause(const QString& id);
    void resume(const QString& id);
    void cancel(const QString& id);

signals:
    void downloadAdded(qtidm::DownloadRecord record);
    void progressChanged(QString id, qint64 received, qint64 total, double bytesPerSecond);
    void statusChanged(QString id, qtidm::DownloadStatus status, QString message);

private:
    struct Job {
        QProcess* process = nullptr;
        std::shared_ptr<MediaStreamProxy> proxy;
        std::shared_ptr<MediaStreamProxy> audioProxy;
        QString targetPath;
        QString stagingPath;
        QByteArray diagnostics;
        qint64 lastBytes = 0;
        qint64 lastTick = 0;
        bool canceled = false;
        bool paused = false;
    };

    void collectOutput(const QString& id);
    void finish(const QString& id, bool succeeded, const QString& error = {});

    QHash<QString, Job> jobs_;
};

}
