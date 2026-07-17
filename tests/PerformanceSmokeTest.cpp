#include "storage/DownloadRepository.h"

#include <QElapsedTimer>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QtTest/QtTest>

class PerformanceSmokeTest final : public QObject {
    Q_OBJECT
private slots:
    void loadsTenThousandRowsWithinBudget()
    {
        if (QProcessEnvironment::systemEnvironment().value(QStringLiteral("QTIDM_RUN_PERF_TESTS")) != QStringLiteral("1")) {
            QSKIP("set QTIDM_RUN_PERF_TESTS=1 to run performance smoke tests");
        }

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        qtidm::DownloadRepository repository;
        QVERIFY(repository.open(dir.path() + QStringLiteral("/perf.sqlite3")));

        for (int i = 0; i < 10000; ++i) {
            qtidm::DownloadRecord record;
            record.id = QStringLiteral("id-%1").arg(i);
            record.url = QUrl(QStringLiteral("https://example.com/%1.bin").arg(i));
            record.targetPath = dir.path() + QStringLiteral("/%1.bin").arg(i);
            record.category = QStringLiteral("General");
            record.totalBytes = 1024;
            record.completedBytes = i % 1024;
            record.status = qtidm::DownloadStatus::Queued;
            record.createdAt = QDateTime::currentDateTimeUtc();
            record.updatedAt = record.createdAt;
            QVERIFY(repository.upsertDownload(record));
        }

        QElapsedTimer timer;
        timer.start();
        const auto records = repository.listDownloads();
        const auto elapsed = timer.elapsed();

        QCOMPARE(records.size(), 10000);
        QVERIFY2(elapsed < 5000, qPrintable(QStringLiteral("listDownloads took %1 ms").arg(elapsed)));
    }
};

QTEST_MAIN(PerformanceSmokeTest)
#include "PerformanceSmokeTest.moc"
