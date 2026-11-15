#include "app/AutomationIpc.h"
#include "app/Paths.h"
#include "core/CurlEpollDownloader.h"
#include "core/DownloadScheduler.h"
#include "integration/AutomationApi.h"
#include "storage/DownloadRepository.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

using namespace qtidm;

namespace {

DownloadRecord makeRecord(const QString& id, DownloadStatus status, qint64 totalBytes,
                          qint64 completedBytes, const QString& targetPath,
                          const QString& queue = QStringLiteral("Main"))
{
    DownloadRecord record;
    record.id = id;
    record.url = QUrl(QStringLiteral("https://example.test/") + id + QStringLiteral(".bin"));
    record.targetPath = targetPath;
    record.category = QStringLiteral("Others");
    record.totalBytes = totalBytes;
    record.completedBytes = completedBytes;
    record.status = status;
    record.createdAt = QDateTime::currentDateTimeUtc();
    record.updatedAt = record.createdAt;
    record.request.url = record.url;
    record.request.targetPath = targetPath;
    record.request.queueName = queue;
    record.request.segments = 4;
    return record;
}

QJsonObject parseObject(const QString& json)
{
    return QJsonDocument::fromJson(json.toUtf8()).object();
}

QJsonObject downloadEntry(const QJsonObject& list, const QString& id)
{
    for (const auto& value : list.value(QStringLiteral("downloads")).toArray()) {
        const auto entry = value.toObject();
        if (entry.value(QStringLiteral("id")).toString() == id) {
            return entry;
        }
    }
    return {};
}

QJsonObject skippedFor(const QJsonObject& answer, const QString& id)
{
    for (const auto& value : answer.value(QStringLiteral("skipped")).toArray()) {
        const auto entry = value.toObject();
        if (entry.value(QStringLiteral("id")).toString() == id) {
            return entry;
        }
    }
    return {};
}

bool contains(const QJsonArray& array, const QString& id)
{
    for (const auto& value : array) {
        if (value.toString() == id) {
            return true;
        }
    }
    return false;
}

}

class AutomationApiTest final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        // Keep the scheduler's persisted queue out of the real user data dir.
        QStandardPaths::setTestModeEnabled(true);
    }

    void init()
    {
        directory_ = std::make_unique<QTemporaryDir>();
        QVERIFY(directory_->isValid());
        repository_ = std::make_unique<DownloadRepository>();
        QVERIFY(repository_->open(directory_->filePath(QStringLiteral("downloads.sqlite3"))));
        downloader_ = std::make_unique<CurlEpollDownloader>();
        scheduler_ = std::make_unique<DownloadScheduler>(*downloader_);
        // Hold the Main queue so a resume never starts a real probe from a test.
        scheduler_->setQueueEnabled(QStringLiteral("Main"), false);
        api_ = std::make_unique<AutomationApi>(*downloader_, *scheduler_, *repository_);
    }

    void cleanup()
    {
        api_.reset();
        scheduler_.reset();
        downloader_.reset();
        repository_.reset();
        directory_.reset();
        Paths::setDataDirectory({});
    }

    void listReportsDownloadsAndQueues()
    {
        QVERIFY(repository_->upsertDownload(makeRecord(QStringLiteral("a"), DownloadStatus::Downloading,
                                                       1000, 250, directory_->filePath(QStringLiteral("a.bin")))));
        QVERIFY(repository_->upsertDownload(makeRecord(QStringLiteral("b"), DownloadStatus::Completed,
                                                       4096, 4096, directory_->filePath(QStringLiteral("b.bin")))));

        const auto answer = parseObject(api_->listJson());
        QVERIFY(!answer.contains(QStringLiteral("error")));
        QCOMPARE(answer.value(QStringLiteral("downloads")).toArray().size(), 2);
        QCOMPARE(answer.value(QStringLiteral("speed")).toInteger(), 0);

        const auto first = downloadEntry(answer, QStringLiteral("a"));
        QCOMPARE(first.value(QStringLiteral("status")).toString(), QStringLiteral("downloading"));
        QCOMPARE(first.value(QStringLiteral("size")).toInteger(), 1000);
        QCOMPARE(first.value(QStringLiteral("downloaded")).toInteger(), 250);
        QCOMPARE(first.value(QStringLiteral("file")).toString(), QStringLiteral("a.bin"));
        QCOMPARE(first.value(QStringLiteral("queue")).toString(), QStringLiteral("Main"));
        QCOMPARE(first.value(QStringLiteral("category")).toString(), QStringLiteral("Others"));
        QVERIFY(first.value(QStringLiteral("added")).isString());

        const auto queues = answer.value(QStringLiteral("queues")).toArray();
        QVERIFY(!queues.isEmpty());
        QCOMPARE(queues.first().toObject().value(QStringLiteral("name")).toString(), QStringLiteral("Main"));
        QCOMPARE(queues.first().toObject().value(QStringLiteral("running")).toBool(), false);
        QVERIFY(answer.contains(QStringLiteral("free")));
    }

    void listReportsUnknownSizeAsNull()
    {
        QVERIFY(repository_->upsertDownload(makeRecord(QStringLiteral("a"), DownloadStatus::Paused,
                                                       -1, 12, directory_->filePath(QStringLiteral("a.bin")))));
        const auto entry = downloadEntry(parseObject(api_->listJson()), QStringLiteral("a"));
        QVERIFY(entry.value(QStringLiteral("size")).isNull());
    }

    void listAlwaysShowsTheMainQueue()
    {
        // A scheduler that has never seen a queue (no Options visit, nothing
        // queued) must still report Main so scripts can address it.
        api_.reset();
        scheduler_ = std::make_unique<DownloadScheduler>(*downloader_);
        api_ = std::make_unique<AutomationApi>(*downloader_, *scheduler_, *repository_);

        const auto queues = parseObject(api_->listJson()).value(QStringLiteral("queues")).toArray();
        QCOMPARE(queues.size(), 1);
        const auto main = queues.first().toObject();
        QCOMPARE(main.value(QStringLiteral("name")).toString(), QStringLiteral("Main"));
        QCOMPARE(main.value(QStringLiteral("running")).toBool(), true);
        QCOMPARE(main.value(QStringLiteral("active")).toInteger(), 0);
        QCOMPARE(main.value(QStringLiteral("pending")).toInteger(), 0);
    }

    void pauseReportsActiveAndSkipsInactive()
    {
        QVERIFY(repository_->upsertDownload(makeRecord(QStringLiteral("active"), DownloadStatus::Downloading,
                                                       100, 10, directory_->filePath(QStringLiteral("active.bin")))));
        QVERIFY(repository_->upsertDownload(makeRecord(QStringLiteral("done"), DownloadStatus::Completed,
                                                       100, 100, directory_->filePath(QStringLiteral("done.bin")))));

        const auto answer = parseObject(api_->pauseJson({ QStringLiteral("active"), QStringLiteral("done"),
                                                          QStringLiteral("missing") }));
        QCOMPARE(answer.value(QStringLiteral("action")).toString(), QStringLiteral("pause"));
        QVERIFY(contains(answer.value(QStringLiteral("done")).toArray(), QStringLiteral("active")));
        QCOMPARE(skippedFor(answer, QStringLiteral("done")).value(QStringLiteral("why")).toString(),
                 QStringLiteral("not active"));
        QCOMPARE(skippedFor(answer, QStringLiteral("missing")).value(QStringLiteral("why")).toString(),
                 QStringLiteral("unknown id"));
    }

    void pauseRoutesMediaThroughHandler()
    {
        QVERIFY(repository_->upsertDownload(makeRecord(QStringLiteral("video"), DownloadStatus::Downloading,
                                                       100, 10, directory_->filePath(QStringLiteral("video.mp4")))));
        QStringList seen;
        api_->setMediaHandler([&seen](const QString& id, AutomationApi::MediaAction action) -> std::optional<bool> {
            if (action != AutomationApi::MediaAction::Pause) {
                return std::nullopt;
            }
            seen.append(id);
            return true;
        });
        auto answer = parseObject(api_->pauseJson({ QStringLiteral("video") }));
        QCOMPARE(seen, QStringList { QStringLiteral("video") });
        QVERIFY(contains(answer.value(QStringLiteral("done")).toArray(), QStringLiteral("video")));

        api_->setMediaHandler([](const QString&, AutomationApi::MediaAction) -> std::optional<bool> {
            return false;
        });
        answer = parseObject(api_->pauseJson({ QStringLiteral("video") }));
        QCOMPARE(skippedFor(answer, QStringLiteral("video")).value(QStringLiteral("why")).toString(),
                 QStringLiteral("media control failed"));
    }

    void pauseFallsBackToTheEngineWithoutAMediaHandler()
    {
        QVERIFY(repository_->upsertDownload(makeRecord(QStringLiteral("plain"), DownloadStatus::Downloading,
                                                       100, 10, directory_->filePath(QStringLiteral("plain.bin")))));
        const auto answer = parseObject(api_->pauseJson({ QStringLiteral("plain") }));
        QVERIFY(contains(answer.value(QStringLiteral("done")).toArray(), QStringLiteral("plain")));
    }

    void startRoutesMediaThroughHandler()
    {
        QVERIFY(repository_->upsertDownload(makeRecord(QStringLiteral("episode"), DownloadStatus::Paused,
                                                       100, 40, directory_->filePath(QStringLiteral("episode.mp4")))));
        QStringList started;
        api_->setMediaHandler([&started](const QString& id, AutomationApi::MediaAction action) -> std::optional<bool> {
            if (action != AutomationApi::MediaAction::Start) {
                return std::nullopt;
            }
            started.append(id);
            return true;
        });

        const auto answer = parseObject(api_->startJson({ QStringLiteral("episode") }));
        QCOMPARE(started, QStringList { QStringLiteral("episode") });
        QVERIFY(contains(answer.value(QStringLiteral("done")).toArray(), QStringLiteral("episode")));
        // A media start must not also queue the file in the standard engine.
        QVERIFY(scheduler_->queued().isEmpty());
    }

    void startQueuesResumableDownload()
    {
        auto record = makeRecord(QStringLiteral("paused"), DownloadStatus::Paused, 1024, 300,
                                 directory_->filePath(QStringLiteral("paused.bin")));
        record.request.scheduledAt = QDateTime::currentDateTime().addDays(3);
        QVERIFY(repository_->upsertDownload(record));

        const auto answer = parseObject(api_->startJson({ QStringLiteral("paused") }));
        QVERIFY(contains(answer.value(QStringLiteral("done")).toArray(), QStringLiteral("paused")));

        const auto queued = scheduler_->queued();
        QCOMPARE(queued.size(), 1);
        QCOMPARE(queued.first().existingId, QStringLiteral("paused"));
        QCOMPARE(queued.first().segments, 4);
        // An explicit start ignores a future "Start at" from the saved request.
        QVERIFY(queued.first().scheduledAt <= QDateTime::currentDateTime().addSecs(1));

        const auto again = parseObject(api_->startJson({ QStringLiteral("paused") }));
        QCOMPARE(skippedFor(again, QStringLiteral("paused")).value(QStringLiteral("why")).toString(),
                 QStringLiteral("already queued"));
    }

    void startSkipsCompletedDownload()
    {
        QVERIFY(repository_->upsertDownload(makeRecord(QStringLiteral("done"), DownloadStatus::Completed,
                                                       10, 10, directory_->filePath(QStringLiteral("done.bin")))));
        const auto answer = parseObject(api_->startJson({ QStringLiteral("done") }));
        QCOMPARE(skippedFor(answer, QStringLiteral("done")).value(QStringLiteral("why")).toString(),
                 QStringLiteral("already complete"));
        QVERIFY(scheduler_->queued().isEmpty());
    }

    void removeKeepsFilesAndForgetsRecord()
    {
        const auto path = directory_->filePath(QStringLiteral("keep.bin"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("payload");
        file.close();
        QVERIFY(repository_->upsertDownload(makeRecord(QStringLiteral("keep"), DownloadStatus::Paused,
                                                       7, 7, path)));

        const auto answer = parseObject(api_->removeJson({ QStringLiteral("keep") }));
        QCOMPARE(answer.value(QStringLiteral("action")).toString(), QStringLiteral("remove"));
        QVERIFY(contains(answer.value(QStringLiteral("done")).toArray(), QStringLiteral("keep")));
        QVERIFY(QFile::exists(path));
        QVERIFY(repository_->listDownloads().isEmpty());
    }

    void deleteRemovesFileAndPartialFile()
    {
        const auto path = directory_->filePath(QStringLiteral("gone.bin"));
        for (const auto& target : { path, path + QStringLiteral(".part") }) {
            QFile file(target);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("payload");
            file.close();
        }
        QVERIFY(repository_->upsertDownload(makeRecord(QStringLiteral("gone"), DownloadStatus::Failed,
                                                       7, 7, path)));

        const auto answer = parseObject(api_->deleteJson({ QStringLiteral("gone") }));
        QCOMPARE(answer.value(QStringLiteral("action")).toString(), QStringLiteral("delete"));
        QVERIFY(contains(answer.value(QStringLiteral("done")).toArray(), QStringLiteral("gone")));
        QVERIFY(!QFile::exists(path));
        QVERIFY(!QFile::exists(path + QStringLiteral(".part")));
        QVERIFY(repository_->listDownloads().isEmpty());
    }

    void queueVerbsToggleDispatch()
    {
        QVERIFY(scheduler_->isQueueEnabled(QStringLiteral("Main")) == false);

        auto answer = parseObject(api_->startQueueJson(QStringLiteral("Main")));
        QCOMPARE(answer.value(QStringLiteral("queue")).toString(), QStringLiteral("Main"));
        QCOMPARE(answer.value(QStringLiteral("running")).toBool(), true);
        QCOMPARE(scheduler_->isQueueEnabled(QStringLiteral("Main")), true);

        answer = parseObject(api_->stopQueueJson(QStringLiteral("main")));
        QCOMPARE(answer.value(QStringLiteral("queue")).toString(), QStringLiteral("main"));
        QCOMPARE(answer.value(QStringLiteral("running")).toBool(), false);
        QCOMPARE(scheduler_->isQueueEnabled(QStringLiteral("Main")), false);
    }

    void queueVerbsRejectUnknownQueue()
    {
        const auto answer = parseObject(api_->startQueueJson(QStringLiteral("Night")));
        QCOMPARE(answer.value(QStringLiteral("code")).toString(), QStringLiteral("unknown-queue"));
        QCOMPARE(parseObject(api_->stopQueueJson(QString())).value(QStringLiteral("code")).toString(),
                 QStringLiteral("invalid-queue"));
    }

    void handleVerbDispatchesNamedVerbs()
    {
        QVERIFY(repository_->upsertDownload(makeRecord(QStringLiteral("a"), DownloadStatus::Downloading,
                                                       10, 5, directory_->filePath(QStringLiteral("a.bin")))));

        const auto list = parseObject(api_->handleVerb(QStringLiteral("list"), {}));
        QCOMPARE(list.value(QStringLiteral("downloads")).toArray().size(), 1);

        const auto paused = parseObject(api_->handleVerb(
            QStringLiteral("pause"), { QStringLiteral("a"), QStringLiteral("zzz") }));
        QVERIFY(contains(paused.value(QStringLiteral("done")).toArray(), QStringLiteral("a")));
        QCOMPARE(skippedFor(paused, QStringLiteral("zzz")).value(QStringLiteral("why")).toString(),
                 QStringLiteral("unknown id"));

        const auto queue = parseObject(api_->handleVerb(
            QStringLiteral("start-queue"), { QStringLiteral("Main") }));
        QCOMPARE(queue.value(QStringLiteral("running")).toBool(), true);
    }

    void handleVerbRejectsUnknownVerb()
    {
        const auto answer = parseObject(api_->handleVerb(QStringLiteral("explode"), {}));
        QCOMPARE(answer.value(QStringLiteral("code")).toString(), QStringLiteral("unknown-verb"));
    }

    void ipcPublishesAnOwnerOnlyToken()
    {
        AutomationIpc ipc(directory_->filePath(QStringLiteral("runtime")));
        QVERIFY2(ipc.start(), qPrintable(ipc.lastError()));
        QVERIFY(QFile::exists(ipc.path()));

        QFile file(ipc.path());
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto document = QJsonDocument::fromJson(file.readAll()).object();
        file.close();

        QCOMPARE(document.value(QStringLiteral("token")).toString().size(), 32);
        QCOMPARE(document.value(QStringLiteral("token")).toString(), ipc.token());
        QCOMPARE(document.value(QStringLiteral("service")).toString(),
                 QString::fromLatin1(AutomationIpc::serviceName));
        QCOMPARE(document.value(QStringLiteral("interface")).toString(),
                 QString::fromLatin1(AutomationIpc::interfaceName));
        QVERIFY(document.value(QStringLiteral("port")).isNull());
        QVERIFY(document.value(QStringLiteral("pid")).toInteger() > 0);

        const auto permissions = QFile::permissions(ipc.path());
        QVERIFY(permissions.testFlag(QFile::ReadOwner));
        QVERIFY(permissions.testFlag(QFile::WriteOwner));
        QVERIFY(!permissions.testFlag(QFile::ReadGroup));
        QVERIFY(!permissions.testFlag(QFile::ReadOther));
    }

    void ipcPublishesTheInstanceServiceName()
    {
        // A default instance keeps the canonical bus name.
        QCOMPARE(AutomationIpc::channelServiceName(),
                 QString::fromLatin1(AutomationIpc::serviceName));

        QTemporaryDir portable;
        QVERIFY(portable.isValid());
        Paths::setDataDirectory(portable.path());
        const auto expected = QString::fromLatin1(AutomationIpc::serviceName)
            + QLatin1Char('.') + Paths::instanceSuffix();
        QCOMPARE(AutomationIpc::channelServiceName(), expected);

        AutomationIpc ipc;
        QVERIFY2(ipc.start(), qPrintable(ipc.lastError()));
        QFile file(ipc.path());
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto published = QJsonDocument::fromJson(file.readAll())
                                   .object().value(QStringLiteral("service")).toString();
        file.close();
        ipc.stop();

        QCOMPARE(published, expected);
    }

    void ipcRotatesTheTokenAndRemovesTheFile()
    {
        AutomationIpc ipc(directory_->filePath(QStringLiteral("runtime")));
        QVERIFY(ipc.start());
        const auto first = ipc.token();
        QVERIFY(ipc.start());
        QVERIFY(ipc.token() != first);
        QCOMPARE(ipc.token().size(), 32);

        ipc.stop();
        QVERIFY(!QFile::exists(ipc.path()));
    }

private:
    std::unique_ptr<QTemporaryDir> directory_;
    std::unique_ptr<DownloadRepository> repository_;
    std::unique_ptr<CurlEpollDownloader> downloader_;
    std::unique_ptr<DownloadScheduler> scheduler_;
    std::unique_ptr<AutomationApi> api_;
};

QTEST_GUILESS_MAIN(AutomationApiTest)
#include "AutomationApiTest.moc"
