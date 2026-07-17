#include "integration/ImportExportService.h"
#include "storage/DownloadRepository.h"

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest/QtTest>

namespace {
qtidm::DownloadRecord makeRecord(const QString& id, const QString& target)
{
    qtidm::DownloadRecord record;
    record.id = id;
    record.url = QUrl(QStringLiteral("https://example.com/") + id);
    record.targetPath = target;
    record.category = QStringLiteral("General");
    record.totalBytes = 20;
    record.completedBytes = 10;
    record.status = qtidm::DownloadStatus::Paused;
    record.createdAt = QDateTime::currentDateTimeUtc();
    record.updatedAt = record.createdAt;
    record.segments = { { 0, 0, 19, 10, qtidm::DownloadStatus::Paused } };
    return record;
}
}

class ImportExportServiceTest final : public QObject {
    Q_OBJECT
private slots:
    void exportsAndImportsHistoryWithSegments()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        qtidm::DownloadRepository source;
        QVERIFY(source.open(dir.path() + QStringLiteral("/source.sqlite3")));
        QVERIFY(source.upsertDownload(makeRecord(QStringLiteral("one"), dir.path() + QStringLiteral("/one.bin"))));

        const auto exportPath = dir.path() + QStringLiteral("/history.json");
        qtidm::ImportExportService exporter(source);
        QVERIFY(exporter.exportHistory(exportPath));
        QVERIFY(QFileInfo::exists(exportPath));

        qtidm::DownloadRepository target;
        QVERIFY(target.open(dir.path() + QStringLiteral("/target.sqlite3")));
        qtidm::ImportExportService importer(target);
        QVERIFY(importer.importHistory(exportPath));

        const auto records = target.listDownloads();
        QCOMPARE(records.size(), 1);
        QCOMPARE(records.first().id, QStringLiteral("one"));
        QCOMPARE(records.first().segments.size(), 1);
        QCOMPARE(records.first().segments.first().written, qint64(10));
    }

    void rejectsInvalidImportFile()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const auto path = dir.path() + QStringLiteral("/bad.json");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("{}");
        file.close();

        qtidm::DownloadRepository repository;
        QVERIFY(repository.open(dir.path() + QStringLiteral("/db.sqlite3")));
        qtidm::ImportExportService service(repository);
        QVERIFY(!service.importHistory(path));
        QCOMPARE(service.lastError(), QStringLiteral("invalid import file"));
    }
};

QTEST_MAIN(ImportExportServiceTest)
#include "ImportExportServiceTest.moc"
