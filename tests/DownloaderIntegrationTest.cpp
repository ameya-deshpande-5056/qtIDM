#include "core/CurlEpollDownloader.h"

#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>

namespace {
constexpr qint64 fixtureSize = 256 * 1024;

QByteArray expectedPayload(qint64 size)
{
    QByteArray data;
    data.resize(size);
    for (qint64 i = 0; i < size; ++i) {
        data[qsizetype(i)] = char(i % 251);
    }
    return data;
}

bool waitForStatus(QSignalSpy& spy, const QString& id, qtidm::DownloadStatus status, int timeoutMs, QString* message = nullptr)
{
    QElapsedTimer timer;
    timer.start();
    qsizetype seen = 0;
    while (timer.elapsed() < timeoutMs) {
        while (seen < spy.size()) {
            const auto args = spy.at(seen++);
            if (args.at(0).toString() == id && args.at(1).value<qtidm::DownloadStatus>() == status) {
                if (message) {
                    *message = args.at(2).toString();
                }
                return true;
            }
        }
        spy.wait(100);
    }
    return false;
}
}

class DownloaderIntegrationTest final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase()
    {
        qRegisterMetaType<qtidm::DownloadRecord>("qtidm::DownloadRecord");
        qRegisterMetaType<qtidm::DownloadStatus>("qtidm::DownloadStatus");
        qRegisterMetaType<QVector<qtidm::SegmentInfo>>("QVector<qtidm::SegmentInfo>");

        server_.setProgram(QStringLiteral("python3"));
        server_.setArguments({ QStringLiteral(QTIDM_SOURCE_DIR) + QStringLiteral("/tests/fixtures/http_test_server.py") });
        server_.setProcessChannelMode(QProcess::MergedChannels);
        server_.start();
        QVERIFY(server_.waitForStarted());
        QVERIFY(server_.waitForReadyRead(5000));
        port_ = QString::fromUtf8(server_.readLine()).trimmed().toInt();
        QVERIFY(port_ > 0);
    }

    void cleanupTestCase()
    {
        server_.terminate();
        server_.waitForFinished(3000);
    }

    void downloadsSegmentedHttpRange()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        qtidm::CurlEpollDownloader downloader;
        QSignalSpy statusSpy(&downloader, &qtidm::CurlEpollDownloader::statusChanged);
        downloader.start();

        qtidm::DownloadRequest request;
        request.url = QUrl(QStringLiteral("http://127.0.0.1:%1/range.bin").arg(port_));
        request.targetPath = dir.path() + QStringLiteral("/range.bin");
        request.category = QStringLiteral("General");
        request.segments = 4;
        const auto id = downloader.enqueue(request);

        QVERIFY(waitForStatus(statusSpy, id, qtidm::DownloadStatus::Completed, 15000));
        downloader.stop();

        QFile file(request.targetPath);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.size(), fixtureSize);
        QCOMPARE(file.readAll(), expectedPayload(fixtureSize));
    }

    void fallsBackWhenServerDoesNotSupportRanges()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        qtidm::CurlEpollDownloader downloader;
        QSignalSpy statusSpy(&downloader, &qtidm::CurlEpollDownloader::statusChanged);
        downloader.start();

        qtidm::DownloadRequest request;
        request.url = QUrl(QStringLiteral("http://127.0.0.1:%1/norange.bin").arg(port_));
        request.targetPath = dir.path() + QStringLiteral("/norange.bin");
        request.segments = 8;
        const auto id = downloader.enqueue(request);

        QVERIFY(waitForStatus(statusSpy, id, qtidm::DownloadStatus::Completed, 15000));
        downloader.stop();

        QFile file(request.targetPath);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.size(), fixtureSize);
        QCOMPARE(file.readAll(), expectedPayload(fixtureSize));
    }

    void supportsBasicAuthentication()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        qtidm::CurlEpollDownloader downloader;
        QSignalSpy statusSpy(&downloader, &qtidm::CurlEpollDownloader::statusChanged);
        downloader.start();

        qtidm::DownloadRequest request;
        request.url = QUrl(QStringLiteral("http://127.0.0.1:%1/auth.bin").arg(port_));
        request.targetPath = dir.path() + QStringLiteral("/auth.bin");
        request.username = QStringLiteral("user");
        request.password = QStringLiteral("pass");
        request.segments = 4;
        const auto id = downloader.enqueue(request);

        QVERIFY(waitForStatus(statusSpy, id, qtidm::DownloadStatus::Completed, 15000));
        downloader.stop();
    }

    void rejectsResumeWhenRemoteSizeChanged()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        qtidm::CurlEpollDownloader downloader;
        QSignalSpy statusSpy(&downloader, &qtidm::CurlEpollDownloader::statusChanged);
        downloader.start();

        qtidm::DownloadRequest request;
        request.existingId = QStringLiteral("resume-id");
        request.url = QUrl(QStringLiteral("http://127.0.0.1:%1/range.bin").arg(port_));
        request.targetPath = dir.path() + QStringLiteral("/resume.bin");
        request.expectedTotalBytes = 123;
        request.resumeSegments = { { 0, 0, 122, 12, qtidm::DownloadStatus::Paused } };
        const auto id = downloader.enqueue(request);

        QString message;
        QVERIFY(waitForStatus(statusSpy, id, qtidm::DownloadStatus::Failed, 5000, &message));
        QCOMPARE(message, QStringLiteral("remote size changed; resume refused"));
        downloader.stop();
    }

    void pausesAndCancelsActiveTransfer()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        qtidm::CurlEpollDownloader downloader;
        QSignalSpy statusSpy(&downloader, &qtidm::CurlEpollDownloader::statusChanged);
        downloader.start();

        qtidm::DownloadRequest pauseRequest;
        pauseRequest.url = QUrl(QStringLiteral("http://127.0.0.1:%1/slow.bin").arg(port_));
        pauseRequest.targetPath = dir.path() + QStringLiteral("/pause.bin");
        pauseRequest.segments = 4;
        const auto pauseId = downloader.enqueue(pauseRequest);
        QVERIFY(waitForStatus(statusSpy, pauseId, qtidm::DownloadStatus::Downloading, 5000));
        QTest::qWait(100);
        downloader.pause(pauseId);
        QVERIFY(waitForStatus(statusSpy, pauseId, qtidm::DownloadStatus::Paused, 5000));

        qtidm::DownloadRequest cancelRequest;
        cancelRequest.url = QUrl(QStringLiteral("http://127.0.0.1:%1/slow.bin").arg(port_));
        cancelRequest.targetPath = dir.path() + QStringLiteral("/cancel.bin");
        cancelRequest.segments = 4;
        const auto cancelId = downloader.enqueue(cancelRequest);
        QVERIFY(waitForStatus(statusSpy, cancelId, qtidm::DownloadStatus::Downloading, 5000));
        QTest::qWait(100);
        downloader.cancel(cancelId);
        QVERIFY(waitForStatus(statusSpy, cancelId, qtidm::DownloadStatus::Canceled, 5000));
        downloader.stop();
    }

private:
    QProcess server_;
    int port_ = 0;
};

QTEST_MAIN(DownloaderIntegrationTest)
#include "DownloaderIntegrationTest.moc"
