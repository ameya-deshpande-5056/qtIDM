#include "core/DownloadScheduler.h"

#include "app/Paths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest/QtTest>

class DownloadSchedulerTest final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QVERIFY(dataDir_.isValid());
        qputenv("XDG_DATA_HOME", dataDir_.path().toUtf8());
        QDir(qtidm::Paths::dataDir()).removeRecursively();
    }

    void persistsFutureQueue()
    {
        qtidm::CurlEpollDownloader downloader;
        qtidm::DownloadScheduler scheduler(downloader);

        qtidm::DownloadRequest request;
        request.url = QUrl(QStringLiteral("https://example.com/future.bin"));
        request.targetPath = QStringLiteral("/tmp/future.bin");
        request.category = QStringLiteral("General");
        request.queueName = QStringLiteral("Night");
        request.scheduledAt = QDateTime::currentDateTime().addDays(1);
        request.segments = 4;

        scheduler.schedule(request);
        QCOMPARE(scheduler.queued().size(), 1);
        QVERIFY2(scheduler.save(), qPrintable(scheduler.lastError()));

        qtidm::DownloadScheduler loaded(downloader);
        QVERIFY(loaded.load());
        QCOMPARE(loaded.queued().size(), 1);
        QCOMPARE(loaded.queued().first().url, request.url);
        QCOMPARE(loaded.queued().first().segments, 4);
        QCOMPARE(loaded.queueNames(), QStringList { QStringLiteral("Night") });
        QCOMPARE(loaded.queued(QStringLiteral("night")).size(), 1);
    }

    void configuresNamedQueueConcurrency()
    {
        qtidm::CurlEpollDownloader downloader;
        qtidm::DownloadScheduler scheduler(downloader);
        QCOMPARE(scheduler.queueConcurrency(QStringLiteral("Main")), 3);
        scheduler.setQueueConcurrency(QStringLiteral("Night"), 1);
        QCOMPARE(scheduler.queueConcurrency(QStringLiteral("night")), 1);
        QCOMPARE(scheduler.activeCount(QStringLiteral("Night")), 0);
    }

    void persistsQueueControlsAndPausesDispatch()
    {
        qtidm::CurlEpollDownloader downloader;
        qtidm::DownloadScheduler scheduler(downloader);
        QSignalSpy dispatched(&scheduler, &qtidm::DownloadScheduler::downloadDispatched);

        scheduler.setQueueConcurrency(QStringLiteral("Night"), 1);
        scheduler.setQueueEnabled(QStringLiteral("Night"), false);
        qtidm::DownloadRequest request;
        request.url = QUrl(QStringLiteral("https://example.com/paused.bin"));
        request.targetPath = QStringLiteral("/tmp/paused.bin");
        request.queueName = QStringLiteral("Night");
        scheduler.schedule(request);
        QCOMPARE(dispatched.size(), 0);
        QCOMPARE(scheduler.queued(QStringLiteral("Night")).size(), 1);

        qtidm::DownloadScheduler loaded(downloader);
        QVERIFY2(loaded.load(), qPrintable(loaded.lastError()));
        QCOMPARE(loaded.queueConcurrency(QStringLiteral("night")), 1);
        QVERIFY(!loaded.isQueueEnabled(QStringLiteral("NIGHT")));
        QCOMPARE(loaded.queued(QStringLiteral("Night")).size(), 1);

        QSignalSpy loadedDispatched(&loaded, &qtidm::DownloadScheduler::downloadDispatched);
        loaded.setQueueEnabled(QStringLiteral("Night"), true);
        QCOMPARE(loadedDispatched.size(), 1);
        QVERIFY(loaded.isQueueEnabled(QStringLiteral("night")));
    }

    void holdsQueuedDownloadsOnMeteredNetworks()
    {
        qtidm::CurlEpollDownloader downloader;
        qtidm::DownloadScheduler scheduler(downloader);
        QSignalSpy dispatched(&scheduler, &qtidm::DownloadScheduler::downloadDispatched);
        scheduler.setMeteredNetworkPolicy(qtidm::MeteredNetworkPolicy::HoldNew);
        scheduler.setNetworkMetered(true);

        qtidm::DownloadRequest request;
        request.url = QUrl(QStringLiteral("https://example.com/metered.bin"));
        request.targetPath = QStringLiteral("/tmp/metered.bin");
        scheduler.schedule(request);
        QCOMPARE(dispatched.size(), 0);
        QCOMPARE(scheduler.queued().size(), 1);

        scheduler.setNetworkMetered(false);
        QCOMPARE(dispatched.size(), 1);
        QVERIFY(scheduler.queued().isEmpty());
    }

    void evaluatesWeekdaysAndOvernightWindows()
    {
        qtidm::DownloadRequest request;
        request.allowedWeekdays = 1 << 0; // Monday
        request.windowStart = QTime(22, 0);
        request.windowEnd = QTime(6, 0);

        QVERIFY(qtidm::DownloadScheduler::isAllowedAt(
            request, QDateTime(QDate(2026, 7, 20), QTime(23, 0))));
        QVERIFY(qtidm::DownloadScheduler::isAllowedAt(
            request, QDateTime(QDate(2026, 7, 20), QTime(2, 0))));
        QVERIFY(!qtidm::DownloadScheduler::isAllowedAt(
            request, QDateTime(QDate(2026, 7, 20), QTime(12, 0))));
        QVERIFY(!qtidm::DownloadScheduler::isAllowedAt(
            request, QDateTime(QDate(2026, 7, 21), QTime(23, 0))));
    }

    void editsReordersAndRemovesPendingEntries()
    {
        qtidm::CurlEpollDownloader downloader;
        qtidm::DownloadScheduler scheduler(downloader);
        scheduler.setQueueEnabled(QStringLiteral("Editing"), false);

        qtidm::DownloadRequest first;
        first.url = QUrl(QStringLiteral("https://example.com/first.bin"));
        first.targetPath = QStringLiteral("/tmp/first.bin");
        first.queueName = QStringLiteral("Editing");
        scheduler.schedule(first);

        qtidm::DownloadRequest second = first;
        second.url = QUrl(QStringLiteral("https://example.com/second.bin"));
        second.targetPath = QStringLiteral("/tmp/second.bin");
        scheduler.schedule(second);

        auto queued = scheduler.queued(QStringLiteral("Editing"));
        QCOMPARE(queued.size(), 2);
        QVERIFY(!queued[0].scheduleId.isEmpty());
        QVERIFY(queued[0].scheduleId != queued[1].scheduleId);
        QVERIFY(scheduler.moveScheduled(queued[1].scheduleId, -1));
        QCOMPARE(scheduler.queued(QStringLiteral("Editing"))[0].url, second.url);

        const auto future = QDateTime::currentDateTime().addDays(2);
        QVERIFY(scheduler.updateScheduled(queued[0].scheduleId, QStringLiteral("Later"), 77, future));
        const auto updated = scheduler.queued(QStringLiteral("Later"));
        QCOMPARE(updated.size(), 1);
        QCOMPARE(updated[0].priority, 77);
        QCOMPARE(updated[0].scheduledAt, future);

        QVERIFY(scheduler.removeScheduled(queued[1].scheduleId));
        QCOMPARE(scheduler.queued().size(), 1);
        QVERIFY(!scheduler.removeScheduled(QStringLiteral("missing")));
    }

    void emitsSafeCompletionCommandOnlyOnSuccess()
    {
        qRegisterMetaType<qtidm::DownloadRequest>("qtidm::DownloadRequest");
        qtidm::CurlEpollDownloader downloader;
        qtidm::DownloadScheduler scheduler(downloader);
        QSignalSpy dispatched(&scheduler, &qtidm::DownloadScheduler::downloadDispatched);
        QSignalSpy commands(&scheduler, &qtidm::DownloadScheduler::completionCommandRequested);

        qtidm::DownloadRequest request;
        request.url = QUrl(QStringLiteral("https://example.com/file.bin"));
        request.targetPath = QStringLiteral("/tmp/file.bin");
        request.completionCommand = QStringLiteral("/usr/bin/notify-send \"Download complete\"");
        scheduler.schedule(request);
        QCOMPARE(dispatched.size(), 1);
        const auto id = dispatched.first().at(0).toString();

        emit downloader.statusChanged(id, qtidm::DownloadStatus::Completed, {});
        QTRY_COMPARE(commands.size(), 1);
        QCOMPARE(commands.first().at(0).toString(), QStringLiteral("/usr/bin/notify-send"));
        QCOMPARE(commands.first().at(1).toStringList(), QStringList { QStringLiteral("Download complete") });
    }

    void appliesCompletionMovePlaceholdersAndHistoryRemoval()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        qtidm::CurlEpollDownloader downloader;
        qtidm::DownloadScheduler scheduler(downloader);
        QSignalSpy dispatched(&scheduler, &qtidm::DownloadScheduler::downloadDispatched);
        QSignalSpy moved(&scheduler, &qtidm::DownloadScheduler::completionFileMoved);
        QSignalSpy commands(&scheduler, &qtidm::DownloadScheduler::completionCommandRequested);
        QSignalSpy removals(&scheduler, &qtidm::DownloadScheduler::historyRemovalRequested);

        const auto source = dir.path() + QStringLiteral("/source.bin");
        QFile file(source);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("data"), qint64(4));
        file.close();

        qtidm::DownloadRequest request;
        request.url = QUrl(QStringLiteral("https://example.com/source.bin"));
        request.targetPath = source;
        request.completionMoveDirectory = dir.path() + QStringLiteral("/finished");
        request.completionCommand = QStringLiteral("/usr/bin/tool \"{file}\" \"{dir}\" \"{url}\"");
        request.removeRecordOnCompletion = true;
        scheduler.schedule(request);
        QCOMPARE(dispatched.size(), 1);
        const auto id = dispatched.first().at(0).toString();

        emit downloader.statusChanged(id, qtidm::DownloadStatus::Completed, {});
        QTRY_COMPARE(moved.size(), 1);
        const auto target = moved.first().at(1).toString();
        QVERIFY(QFileInfo::exists(target));
        QVERIFY(!QFileInfo::exists(source));
        QCOMPARE(commands.size(), 1);
        QCOMPARE(commands.first().at(1).toStringList(),
            QStringList({ target, QFileInfo(target).absolutePath(), request.url.toString() }));
        QCOMPARE(removals.size(), 1);
    }

private:
    QTemporaryDir dataDir_;
};

QTEST_MAIN(DownloadSchedulerTest)
#include "DownloadSchedulerTest.moc"
