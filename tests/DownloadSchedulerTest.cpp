#include "core/DownloadScheduler.h"

#include "app/Paths.h"

#include <QDir>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest/QtTest>

class DownloadSchedulerTest final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
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
        request.scheduledAt = QDateTime::currentDateTime().addDays(1);
        request.segments = 4;

        scheduler.schedule(request);
        QCOMPARE(scheduler.queued().size(), 1);
        QVERIFY(scheduler.save());

        qtidm::DownloadScheduler loaded(downloader);
        QVERIFY(loaded.load());
        QCOMPARE(loaded.queued().size(), 1);
        QCOMPARE(loaded.queued().first().url, request.url);
        QCOMPARE(loaded.queued().first().segments, 4);
    }
};

QTEST_MAIN(DownloadSchedulerTest)
#include "DownloadSchedulerTest.moc"
