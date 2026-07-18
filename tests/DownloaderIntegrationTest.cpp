#include "core/CurlEpollDownloader.h"

#include <QElapsedTimer>
#include <QCryptographicHash>
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
        server_.setProcessChannelMode(QProcess::SeparateChannels);
        server_.start();
        QVERIFY(server_.waitForStarted());
        QVERIFY2(server_.waitForReadyRead(5000), qPrintable(server_.errorString()));
        const auto portText = QString::fromUtf8(server_.readLine()).trimmed();
        port_ = portText.toInt();
        QVERIFY2(port_ > 0, qPrintable(QStringLiteral("HTTP fixture did not report a port: %1")
                                       .arg(QString::fromUtf8(server_.readAllStandardError()))));
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
        QVERIFY(!QFileInfo::exists(request.targetPath + QStringLiteral(".part")));
    }

    void dynamicallyPlansMoreRangesThanConnections()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        qtidm::CurlEpollDownloader downloader;
        QSignalSpy addedSpy(&downloader, &qtidm::CurlEpollDownloader::downloadAdded);
        QSignalSpy statusSpy(&downloader, &qtidm::CurlEpollDownloader::statusChanged);
        downloader.start();

        qtidm::DownloadRequest request;
        request.url = QUrl(QStringLiteral("http://127.0.0.1:%1/range.bin").arg(port_));
        request.targetPath = dir.path() + QStringLiteral("/dynamic.bin");
        request.segments = 2;
        request.dynamicSegmentation = true;
        const auto id = downloader.enqueue(request);

        QVERIFY(waitForStatus(statusSpy, id, qtidm::DownloadStatus::Completed, 15000));
        downloader.stop();
        QCOMPARE(addedSpy.size(), 1);
        const auto record = addedSpy.first().first().value<qtidm::DownloadRecord>();
        QVERIFY(record.segments.size() > request.segments);

        QFile file(request.targetPath);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), expectedPayload(fixtureSize));
    }

    void enforcesSharedPerHostConnectionLimit()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        qtidm::CurlEpollDownloader downloader;
        QSignalSpy statusSpy(&downloader, &qtidm::CurlEpollDownloader::statusChanged);
        QSignalSpy connectionSpy(&downloader, &qtidm::CurlEpollDownloader::hostConnectionCountChanged);
        downloader.start();

        qtidm::DownloadRequest request;
        request.url = QUrl(QStringLiteral("http://127.0.0.1:%1/slow.bin").arg(port_));
        request.targetPath = dir.path() + QStringLiteral("/host-limited.bin");
        request.segments = 8;
        request.perHostConnectionLimit = 2;
        const auto id = downloader.enqueue(request);
        QVERIFY(waitForStatus(statusSpy, id, qtidm::DownloadStatus::Completed, 15000));
        downloader.stop();

        int maximumConnections = 0;
        for (const auto& event : connectionSpy) {
            maximumConnections = qMax(maximumConnections, event.at(1).toInt());
        }
        QCOMPARE(maximumConnections, 2);
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

    void retriesTemporaryServerFailures()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        qtidm::CurlEpollDownloader downloader;
        QSignalSpy statusSpy(&downloader, &qtidm::CurlEpollDownloader::statusChanged);
        downloader.start();

        qtidm::DownloadRequest request;
        request.url = QUrl(QStringLiteral("http://127.0.0.1:%1/flaky.bin").arg(port_));
        request.targetPath = dir.path() + QStringLiteral("/flaky.bin");
        request.segments = 1;
        request.maxRetries = 3;
        request.retryBaseDelayMs = 10;
        const auto id = downloader.enqueue(request);

        QVERIFY(waitForStatus(statusSpy, id, qtidm::DownloadStatus::Completed, 15000));
        downloader.stop();

        QFile file(request.targetPath);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), expectedPayload(fixtureSize));
    }

    void reportsPermanentHttpFailureOnce()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        qtidm::CurlEpollDownloader downloader;
        QSignalSpy statusSpy(&downloader, &qtidm::CurlEpollDownloader::statusChanged);
        downloader.start();

        qtidm::DownloadRequest request;
        request.url = QUrl(QStringLiteral("http://127.0.0.1:%1/forbidden.bin").arg(port_));
        request.targetPath = dir.path() + QStringLiteral("/forbidden.bin");
        const auto id = downloader.enqueue(request);

        QString message;
        QVERIFY(waitForStatus(statusSpy, id, qtidm::DownloadStatus::Failed, 5000, &message));
        QVERIFY(message.startsWith(QStringLiteral("HTTP 403:")));
        QVERIFY(message.contains(QStringLiteral("capture it again")));
        QTest::qWait(100);

        int failedEvents = 0;
        for (const auto& event : statusSpy) {
            if (event.at(0).toString() == id
                && event.at(1).value<qtidm::DownloadStatus>() == qtidm::DownloadStatus::Failed) {
                failedEvents++;
            }
        }
        QCOMPARE(failedEvents, 1);
        downloader.stop();
    }

    void verifiesExpectedChecksums()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        qtidm::CurlEpollDownloader downloader;
        QSignalSpy statusSpy(&downloader, &qtidm::CurlEpollDownloader::statusChanged);
        downloader.start();

        qtidm::DownloadRequest valid;
        valid.url = QUrl(QStringLiteral("http://127.0.0.1:%1/range.bin").arg(port_));
        valid.targetPath = dir.path() + QStringLiteral("/verified.bin");
        valid.expectedSha256 = QString::fromLatin1(
            QCryptographicHash::hash(expectedPayload(fixtureSize), QCryptographicHash::Sha256).toHex());
        const auto validId = downloader.enqueue(valid);
        QVERIFY(waitForStatus(statusSpy, validId, qtidm::DownloadStatus::Completed, 15000));

        qtidm::DownloadRequest generic = valid;
        generic.targetPath = dir.path() + QStringLiteral("/verified-md5.bin");
        generic.expectedSha256.clear();
        generic.checksumAlgorithm = QStringLiteral("md5");
        generic.expectedChecksum = QString::fromLatin1(
            QCryptographicHash::hash(expectedPayload(fixtureSize), QCryptographicHash::Md5).toHex());
        const auto genericId = downloader.enqueue(generic);
        QVERIFY(waitForStatus(statusSpy, genericId, qtidm::DownloadStatus::Completed, 15000));

        qtidm::DownloadRequest invalid = valid;
        invalid.targetPath = dir.path() + QStringLiteral("/corrupt.bin");
        invalid.expectedSha256 = QString(64, QLatin1Char('0'));
        const auto invalidId = downloader.enqueue(invalid);
        QString message;
        QVERIFY(waitForStatus(statusSpy, invalidId, qtidm::DownloadStatus::Failed, 15000, &message));
        QVERIFY(message.startsWith(QStringLiteral("SHA-256 mismatch:")));
        downloader.stop();
    }

    void appliesDestinationConflictPolicies()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto target = dir.path() + QStringLiteral("/existing.bin");
        QFile existing(target);
        QVERIFY(existing.open(QIODevice::WriteOnly));
        QCOMPARE(existing.write("keep"), qint64(4));
        existing.close();

        qtidm::CurlEpollDownloader downloader;
        QSignalSpy addedSpy(&downloader, &qtidm::CurlEpollDownloader::downloadAdded);
        QSignalSpy statusSpy(&downloader, &qtidm::CurlEpollDownloader::statusChanged);
        downloader.start();

        qtidm::DownloadRequest skip;
        skip.url = QUrl(QStringLiteral("http://127.0.0.1:%1/range.bin").arg(port_));
        skip.targetPath = target;
        skip.fileConflictPolicy = qtidm::FileConflictPolicy::Skip;
        const auto skipId = downloader.enqueue(skip);
        QVERIFY(waitForStatus(statusSpy, skipId, qtidm::DownloadStatus::Canceled, 5000));
        QVERIFY(existing.open(QIODevice::ReadOnly));
        QCOMPARE(existing.readAll(), QByteArray("keep"));
        existing.close();

        qtidm::DownloadRequest rename = skip;
        rename.fileConflictPolicy = qtidm::FileConflictPolicy::AutoRename;
        const auto renameId = downloader.enqueue(rename);
        QVERIFY(waitForStatus(statusSpy, renameId, qtidm::DownloadStatus::Completed, 15000));
        downloader.stop();

        QCOMPARE(addedSpy.size(), 1);
        const auto renamedPath = addedSpy.first().first().value<qtidm::DownloadRecord>().targetPath;
        QVERIFY(renamedPath.endsWith(QStringLiteral("existing (1).bin")));
        QVERIFY(QFileInfo::exists(renamedPath));
        QVERIFY(existing.open(QIODevice::ReadOnly));
        QCOMPARE(existing.readAll(), QByteArray("keep"));
    }

    void enforcesAndResetsSessionDataLimit()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        qtidm::CurlEpollDownloader downloader;
        QSignalSpy statusSpy(&downloader, &qtidm::CurlEpollDownloader::statusChanged);
        downloader.start();

        qtidm::DownloadRequest request;
        request.url = QUrl(QStringLiteral("http://127.0.0.1:%1/range.bin").arg(port_));
        request.targetPath = dir.path() + QStringLiteral("/quota.bin");
        request.sessionDataLimitBytes = fixtureSize / 2;
        const auto id = downloader.enqueue(request);

        QString message;
        QVERIFY(waitForStatus(statusSpy, id, qtidm::DownloadStatus::Failed, 5000, &message));
        QVERIFY(message.startsWith(QStringLiteral("session data limit exceeded:")));
        QCOMPARE(downloader.sessionBytesReceived(), qint64(0));

        request.url = QUrl(QStringLiteral("http://127.0.0.1:%1/unknown.bin").arg(port_));
        request.targetPath = dir.path() + QStringLiteral("/unknown-quota.bin");
        const auto unknownId = downloader.enqueue(request);
        QVERIFY(waitForStatus(statusSpy, unknownId, qtidm::DownloadStatus::Failed, 5000, &message));
        QCOMPARE(message, QStringLiteral("session data limit exceeded"));
        QVERIFY(downloader.sessionBytesReceived() > request.sessionDataLimitBytes);
        downloader.resetSessionBytesReceived();
        QCOMPARE(downloader.sessionBytesReceived(), qint64(0));
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
        QVERIFY(!QFileInfo::exists(pauseRequest.targetPath));
        QVERIFY(QFileInfo::exists(pauseRequest.targetPath + QStringLiteral(".part")));

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
