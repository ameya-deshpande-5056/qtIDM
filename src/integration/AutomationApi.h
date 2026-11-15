#pragma once

#include "core/DownloadTypes.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <functional>
#include <optional>

namespace qtidm {

class CurlEpollDownloader;
class DownloadRepository;
class DownloadScheduler;

// The command surface behind qtIDM's automation channel: the D-Bus service and
// qtidm-cli drive these verbs, and every answer is a JSON object.
class AutomationApi final : public QObject {
    Q_OBJECT
public:
    enum class MediaAction {
        Pause,
        Start,
        Cancel
    };

    // std::nullopt means "not a media download, use the engine"; true and false
    // mean the media job applied or refused the action.
    using MediaHandler = std::function<std::optional<bool>(const QString& id, MediaAction action)>;

    AutomationApi(CurlEpollDownloader& downloader, DownloadScheduler& scheduler,
                  DownloadRepository& repository, QObject* parent = nullptr);

    void setMediaHandler(MediaHandler handler);

    QString listJson() const;
    QString pauseJson(const QStringList& ids);
    QString startJson(const QStringList& ids);
    QString removeJson(const QStringList& ids);
    QString deleteJson(const QStringList& ids);
    QString startQueueJson(const QString& queueName);
    QString stopQueueJson(const QString& queueName);

    // Named-verb dispatch for the D-Bus surface.
    QString handleVerb(const QString& verb, const QStringList& arguments);

private:
    QString cancelAndForget(const QStringList& ids, bool deleteFiles);
    static const DownloadRecord* findRecord(const QList<DownloadRecord>& records, const QString& id);
    static QString errorJson(const QString& message, const QString& code = {});
    static bool isActive(DownloadStatus status);
    static bool isResumable(DownloadStatus status);
    static bool isStoppable(DownloadStatus status);

    CurlEpollDownloader& downloader_;
    DownloadScheduler& scheduler_;
    DownloadRepository& repository_;
    MediaHandler mediaHandler_;
    QHash<QString, double> speeds_;
};

}
