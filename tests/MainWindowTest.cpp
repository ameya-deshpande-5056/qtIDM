#include <QApplication>
#include <QClipboard>
#include <QHeaderView>
#include <QHelpEvent>
#include <QLineEdit>
#include <QMetaObject>
#include <QSystemTrayIcon>
#include <QTableWidget>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QtTest/QtTest>
#include <QToolTip>
#include <QTreeWidget>

#include "core/CurlEpollDownloader.h"
#include "core/DownloadScheduler.h"
#include "core/DownloadTypes.h"
#include "storage/DownloadRepository.h"
#include "theme/ThemeManager.h"
#include "ui/MainWindow.h"
#include "ui/SegmentGrid.h"

namespace {

QTableWidget* findDownloadsTable(qtidm::MainWindow& window)
{
    for (auto* table : window.findChildren<QTableWidget*>()) {
        if (table->columnCount() >= 4 && table->horizontalHeaderItem(3)
            && table->horizontalHeaderItem(3)->text() == QStringLiteral("Status")) {
            return table;
        }
    }
    return nullptr;
}

QTreeWidget* findGeneralDetailsTree(qtidm::MainWindow& window)
{
    for (auto* tabs : window.findChildren<QTabWidget*>()) {
        if (tabs->count() > 0 && tabs->tabText(0) == QStringLiteral("General")) {
            return qobject_cast<QTreeWidget*>(tabs->widget(0));
        }
    }
    return nullptr;
}

QString detailValue(QTreeWidget* tree, const QString& key)
{
    if (!tree) {
        return {};
    }
    for (int index = 0; index < tree->topLevelItemCount(); ++index) {
        const auto* item = tree->topLevelItem(index);
        if (item && item->text(0) == key) {
            return item->text(1);
        }
    }
    return {};
}

} // namespace

class MainWindowTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        qRegisterMetaType<qtidm::DownloadRecord>("DownloadRecord");
        qRegisterMetaType<qtidm::DownloadStatus>("DownloadStatus");
        qRegisterMetaType<QVector<qtidm::SegmentInfo>>("QVector<SegmentInfo>");
    }

    void tableAndDetailsStatusStaySynchronized()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        qtidm::DownloadRepository repository;
        QVERIFY(repository.open(dir.filePath(QStringLiteral("downloads.sqlite3"))));
        QVERIFY(repository.migrate());
        qtidm::CurlEpollDownloader downloader;
        qtidm::DownloadScheduler scheduler(downloader);
        qtidm::ThemeManager themeManager;

        qtidm::MainWindow window(downloader, scheduler, repository, themeManager);
        auto* table = findDownloadsTable(window);
        auto* details = findGeneralDetailsTree(window);
        QVERIFY(table != nullptr);
        QVERIFY(details != nullptr);

        qtidm::DownloadRecord record;
        record.id = QStringLiteral("dl-sync-test");
        record.url = QUrl(QStringLiteral("https://example.com/archive.tar.gz"));
        record.targetPath = dir.filePath(QStringLiteral("archive.tar.gz"));
        record.category = QStringLiteral("Compressed");
        record.totalBytes = 1000;
        record.status = qtidm::DownloadStatus::Connecting;
        record.createdAt = QDateTime::currentDateTimeUtc();
        record.updatedAt = record.createdAt;
        record.request.url = record.url;
        record.request.targetPath = record.targetPath;
        record.segments = {
            qtidm::SegmentInfo(0, 0, 999, 0, qtidm::DownloadStatus::Queued)
        };

        QVERIFY(QMetaObject::invokeMethod(&window, "onDownloadAdded", Qt::DirectConnection,
                                          Q_ARG(qtidm::DownloadRecord, record)));
        QCOMPARE(table->rowCount(), 1);
        table->selectRow(0);
        QApplication::processEvents();

        QCOMPARE(table->item(0, 3)->text(), QStringLiteral("Connecting"));
        QCOMPARE(detailValue(details, QStringLiteral("Status")), QStringLiteral("Connecting"));

        // Zero-delta progress tick while connecting must keep both table and details on Connecting.
        QVERIFY(QMetaObject::invokeMethod(&window, "onProgressChanged", Qt::DirectConnection,
                                          Q_ARG(QString, record.id),
                                          Q_ARG(qint64, 0),
                                          Q_ARG(qint64, 1000),
                                          Q_ARG(double, 0.0)));
        QCOMPARE(table->item(0, 3)->text(), QStringLiteral("Connecting"));
        QCOMPARE(detailValue(details, QStringLiteral("Status")), QStringLiteral("Connecting"));

        // Non-zero byte progress within the 750ms metric throttle must update both table and details immediately.
        QVERIFY(QMetaObject::invokeMethod(&window, "onProgressChanged", Qt::DirectConnection,
                                          Q_ARG(QString, record.id),
                                          Q_ARG(qint64, 250),
                                          Q_ARG(qint64, 1000),
                                          Q_ARG(double, 250.0)));
        QCOMPARE(table->item(0, 3)->text(), QStringLiteral("Downloading"));
        QCOMPARE(detailValue(details, QStringLiteral("Status")), QStringLiteral("Downloading"));

        // Transient retry switches both to Connecting, and a segment refresh must not resurrect Downloading.
        QVERIFY(QMetaObject::invokeMethod(&window, "onStatusChanged", Qt::DirectConnection,
                                          Q_ARG(QString, record.id),
                                          Q_ARG(qtidm::DownloadStatus, qtidm::DownloadStatus::Connecting),
                                          Q_ARG(QString, QStringLiteral("Segment 1 retry 1/5 in 500 ms"))));
        QVERIFY(QMetaObject::invokeMethod(&window, "onProgressChanged", Qt::DirectConnection,
                                          Q_ARG(QString, record.id),
                                          Q_ARG(qint64, 250),
                                          Q_ARG(qint64, 1000),
                                          Q_ARG(double, 0.0)));
        QVERIFY(QMetaObject::invokeMethod(&window, "onSegmentsChanged", Qt::DirectConnection,
                                          Q_ARG(QString, record.id),
                                          Q_ARG(QVector<qtidm::SegmentInfo>, record.segments)));
        QCOMPARE(table->item(0, 3)->text(), QStringLiteral("Connecting"));
        QCOMPARE(detailValue(details, QStringLiteral("Status")), QStringLiteral("Connecting"));

        // Subsequent progress moves both back to Downloading and clears the stale retry message.
        QVERIFY(QMetaObject::invokeMethod(&window, "onProgressChanged", Qt::DirectConnection,
                                          Q_ARG(QString, record.id),
                                          Q_ARG(qint64, 600),
                                          Q_ARG(qint64, 1000),
                                          Q_ARG(double, 350.0)));
        QCOMPARE(table->item(0, 3)->text(), QStringLiteral("Downloading"));
        QCOMPARE(detailValue(details, QStringLiteral("Status")), QStringLiteral("Downloading"));
        QVERIFY(detailValue(details, QStringLiteral("Last message")).isEmpty());

        // Reaching 100% progress before statusChanged(Completed) must keep both at Downloading until verified.
        QVERIFY(QMetaObject::invokeMethod(&window, "onProgressChanged", Qt::DirectConnection,
                                          Q_ARG(QString, record.id),
                                          Q_ARG(qint64, 1000),
                                          Q_ARG(qint64, 1000),
                                          Q_ARG(double, 400.0)));
        QCOMPARE(table->item(0, 3)->text(), QStringLiteral("Downloading"));
        QCOMPARE(detailValue(details, QStringLiteral("Status")), QStringLiteral("Downloading"));

        QVERIFY(QMetaObject::invokeMethod(&window, "onStatusChanged", Qt::DirectConnection,
                                          Q_ARG(QString, record.id),
                                          Q_ARG(qtidm::DownloadStatus, qtidm::DownloadStatus::Completed),
                                          Q_ARG(QString, QString())));
        QCOMPARE(table->item(0, 3)->text(), QStringLiteral("Complete"));
        QCOMPARE(detailValue(details, QStringLiteral("Status")), QStringLiteral("Complete"));
    }

    void trayLeftClickOpensHiddenAndMinimizedWindow()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        qtidm::DownloadRepository repository;
        QVERIFY(repository.open(dir.filePath(QStringLiteral("downloads.sqlite3"))));
        QVERIFY(repository.migrate());
        qtidm::CurlEpollDownloader downloader;
        qtidm::DownloadScheduler scheduler(downloader);
        qtidm::ThemeManager themeManager;

        qtidm::MainWindow window(downloader, scheduler, repository, themeManager);
        auto* tray = window.findChild<QSystemTrayIcon*>();
        QVERIFY(tray != nullptr);

        // Context-menu / right-click must not prematurely unhide the window.
        window.hide();
        QVERIFY(!window.isVisible());
        emit tray->activated(QSystemTrayIcon::Context);
        QVERIFY(!window.isVisible());

        // Single left-click must open a hidden-to-tray window.
        emit tray->activated(QSystemTrayIcon::Trigger);
        QVERIFY(window.isVisible());
        QVERIFY(!window.isMinimized());

        // Single left-click must restore a minimized window while preserving maximized state.
        window.setWindowState(Qt::WindowMaximized | Qt::WindowMinimized);
        QVERIFY(window.isMinimized());
        emit tray->activated(QSystemTrayIcon::Trigger);
        QVERIFY(window.isVisible());
        QVERIFY(!window.isMinimized());
        QVERIFY(window.windowState().testFlag(Qt::WindowMaximized));
    }

    void tableOmitsSourceUrlAndSegmentsAndResizesColumnsDynamically()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        qtidm::DownloadRepository repository;
        QVERIFY(repository.open(dir.filePath(QStringLiteral("downloads.sqlite3"))));
        QVERIFY(repository.migrate());
        qtidm::CurlEpollDownloader downloader;
        qtidm::DownloadScheduler scheduler(downloader);
        qtidm::ThemeManager themeManager;

        qtidm::MainWindow window(downloader, scheduler, repository, themeManager);
        auto* table = findDownloadsTable(window);
        QVERIFY(table != nullptr);

        QCOMPARE(table->columnCount(), 9);
        QVERIFY(table->isColumnHidden(0));
        QCOMPARE(table->horizontalHeaderItem(1)->text(), QStringLiteral("File Name"));
        QCOMPARE(table->horizontalHeaderItem(2)->text(), QStringLiteral("Size"));
        QCOMPARE(table->horizontalHeaderItem(3)->text(), QStringLiteral("Status"));
        QCOMPARE(table->horizontalHeaderItem(4)->text(), QStringLiteral("Progress"));
        QCOMPARE(table->horizontalHeaderItem(5)->text(), QStringLiteral("Download Speed"));
        QCOMPARE(table->horizontalHeaderItem(6)->text(), QStringLiteral("Date Added"));
        QCOMPARE(table->horizontalHeaderItem(7)->text(), QStringLiteral("ETA"));
        QCOMPARE(table->horizontalHeaderItem(8)->text(), QStringLiteral("Save To"));
        for (int col = 0; col < table->columnCount(); ++col) {
            const auto label = table->horizontalHeaderItem(col)->text();
            QVERIFY(label != QStringLiteral("Source URL"));
            QVERIFY(label != QStringLiteral("Segments"));
        }
        QVERIFY(!table->wordWrap());
        QCOMPARE(table->textElideMode(), Qt::ElideRight);
        QVERIFY(!table->horizontalHeader()->stretchLastSection());
        QCOMPARE(table->horizontalHeader()->sectionResizeMode(1), QHeaderView::Interactive);
        QCOMPARE(table->horizontalHeader()->sectionResizeMode(8), QHeaderView::Interactive);

        qtidm::DownloadRecord record;
        record.id = QStringLiteral("dl-table-layout");
        record.url = QUrl(QStringLiteral("https://cdn.example.net/releases/very_long_filename_episode_01_1080p.mkv"));
        record.targetPath = dir.filePath(QStringLiteral("Downloads/Anime/very_long_filename_episode_01_1080p.mkv"));
        record.category = QStringLiteral("Video");
        record.totalBytes = 250 * 1024 * 1024;
        record.status = qtidm::DownloadStatus::Downloading;
        record.createdAt = QDateTime::currentDateTimeUtc();
        record.updatedAt = record.createdAt;
        record.request.url = record.url;
        record.request.targetPath = record.targetPath;
        record.segments = {
            qtidm::SegmentInfo(0, 0, 999, 100, qtidm::DownloadStatus::Downloading)
        };

        QVERIFY(QMetaObject::invokeMethod(&window, "onDownloadAdded", Qt::DirectConnection,
                                          Q_ARG(qtidm::DownloadRecord, record)));
        QCOMPARE(table->rowCount(), 1);
        QCOMPARE(table->item(0, 1)->text(), QStringLiteral("very_long_filename_episode_01_1080p.mkv"));
        QCOMPARE(table->item(0, 8)->text(), record.targetPath);
        QVERIFY(table->findChildren<qtidm::SegmentGrid*>().isEmpty());

        // Copy URL and search filter by hidden URL must continue working.
        table->selectRow(0);
        QVERIFY(QMetaObject::invokeMethod(&window, "copySelectedUrl", Qt::DirectConnection));
        QCOMPARE(QApplication::clipboard()->text(), record.url.toString());
        if (auto* search = window.findChild<QLineEdit*>()) {
            search->setText(QStringLiteral("cdn.example.net"));
            QApplication::processEvents();
            QVERIFY(!table->isRowHidden(0));
            search->clear();
            QApplication::processEvents();
        }

        // Expanding and contracting the window dynamically adjusts File Name & Save To widths.
        window.resize(1100, 720);
        window.show();
        QApplication::processEvents();
        const int baseFileW = table->columnWidth(1);
        const int baseSaveW = table->columnWidth(8);
        QVERIFY(baseFileW > 110);
        QVERIFY(baseSaveW > 110);

        window.resize(1500, 720);
        QApplication::processEvents();
        const int expandedFileW = table->columnWidth(1);
        const int expandedSaveW = table->columnWidth(8);
        QVERIFY(expandedFileW > baseFileW);
        QVERIFY(expandedSaveW > baseSaveW);

        window.resize(960, 720);
        QApplication::processEvents();
        QVERIFY(table->columnWidth(1) < expandedFileW);
        QVERIFY(table->columnWidth(8) < expandedSaveW);

        // Hovering a non-elided cell shows no tooltip; contracting to elide shows full text; expanding hides it.
        const auto sendTooltipEventAt = [&](int col) {
            const auto rect = table->visualItemRect(table->item(0, col));
            const QPoint localPos = rect.center();
            const QPoint globalPos = table->viewport()->mapToGlobal(localPos);
            QHelpEvent helpEvent(QEvent::ToolTip, localPos, globalPos);
            QApplication::sendEvent(table->viewport(), &helpEvent);
        };
        table->horizontalHeader()->resizeSection(1, 700);
        QApplication::processEvents();
        sendTooltipEventAt(1);
        QVERIFY(!QToolTip::isVisible());

        table->horizontalHeader()->resizeSection(1, 90);
        QApplication::processEvents();
        sendTooltipEventAt(1);
        QVERIFY(QToolTip::isVisible());
        QCOMPARE(QToolTip::text(), QStringLiteral("very_long_filename_episode_01_1080p.mkv"));

        table->horizontalHeader()->resizeSection(1, 700);
        QApplication::processEvents();
        sendTooltipEventAt(1);
        QTRY_VERIFY(!QToolTip::isVisible());
    }
};

int main(int argc, char** argv)
{
    QTemporaryDir homeDir;
    if (!homeDir.isValid()) {
        return 1;
    }
    qputenv("XDG_DATA_HOME", homeDir.filePath(QStringLiteral("data")).toUtf8());
    qputenv("XDG_CONFIG_HOME", homeDir.filePath(QStringLiteral("config")).toUtf8());
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("qtIDM-test"));
    app.setApplicationName(QStringLiteral("qtIDM-main-window-test"));
    MainWindowTest tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "MainWindowTest.moc"
