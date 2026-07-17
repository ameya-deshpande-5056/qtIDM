#pragma once

#include "core/DownloadTypes.h"

#include <QHash>
#include <QObject>

class QProcess;
class QUrl;

namespace qtidm {

class MediaDownloader final : public QObject {
    Q_OBJECT
public:
    explicit MediaDownloader(QObject* parent = nullptr);
    ~MediaDownloader() override;

    static bool supports(const QUrl& url);
    QString enqueue(const QUrl& url, const QString& targetPath, const QVariantMap& headers, const QString& existingId = {});
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
