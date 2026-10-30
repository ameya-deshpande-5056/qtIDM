#include "core/CurlEpollDownloader.h"
#include "storage/DownloadRepository.h"

#include <QElapsedTimer>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <arpa/inet.h>

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

    void alternateLimitCapsTheWholeSegmentedDownload()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        qtidm::CurlEpollDownloader downloader;
        QSignalSpy statusSpy(&downloader, &qtidm::CurlEpollDownloader::statusChanged);
        constexpr qint64 limit = 128 * 1024;
        downloader.setAlternateSpeedLimit(limit);
        QCOMPARE(downloader.alternateSpeedLimit(), limit);
        downloader.start();

        qtidm::DownloadRequest request;
        request.url = QUrl(QStringLiteral("http://127.0.0.1:%1/range.bin").arg(port_));
        request.targetPath = dir.path() + QStringLiteral("/alternate-limited.bin");
        request.segments = 4;
        QElapsedTimer elapsed;
        elapsed.start();
        const auto id = downloader.enqueue(request);

        QVERIFY(waitForStatus(statusSpy, id, qtidm::DownloadStatus::Completed, 15000));
        QVERIFY2(elapsed.elapsed() >= 1200,
                 qPrintable(QStringLiteral("segmented transfer bypassed its per-download cap (%1 ms)")
                                .arg(elapsed.elapsed())));
        downloader.stop();
        QCOMPARE(QFileInfo(request.targetPath).size(), fixtureSize);
    }

    void preservesEncodedQueryCharacters()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        qtidm::CurlEpollDownloader downloader;
        QSignalSpy statusSpy(&downloader, &qtidm::CurlEpollDownloader::statusChanged);
        downloader.start();

        qtidm::DownloadRequest request;
        request.url = QUrl(
            QStringLiteral("http://127.0.0.1:%1/range.bin").arg(port_)
            + QStringLiteral(
                "?response-content-disposition=attachment%3B%20filename%3DqtIDM.zip"));
        request.targetPath = dir.path() + QStringLiteral("/encoded-query.bin");
        request.segments = 2;
        const auto id = downloader.enqueue(request);

        QString message;
        QVERIFY2(waitForStatus(statusSpy, id, qtidm::DownloadStatus::Completed, 15000, &message),
                 qPrintable(message));
        downloader.stop();

        QFile file(request.targetPath);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), expectedPayload(fixtureSize));
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

    void discoversSizeWhenHeadIsRejected()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        qtidm::CurlEpollDownloader downloader;
        QSignalSpy addedSpy(&downloader, &qtidm::CurlEpollDownloader::downloadAdded);
        QSignalSpy statusSpy(&downloader, &qtidm::CurlEpollDownloader::statusChanged);
        downloader.start();

        qtidm::DownloadRequest request;
        request.url = QUrl(QStringLiteral("http://127.0.0.1:%1/head-rejected-range.bin").arg(port_));
        request.targetPath = dir.path() + QStringLiteral("/head-rejected.bin");
        request.segments = 4;
        const auto id = downloader.enqueue(request);

        QVERIFY(waitForStatus(statusSpy, id, qtidm::DownloadStatus::Completed, 15000));
        downloader.stop();
        QCOMPARE(addedSpy.size(), 1);
        QCOMPARE(addedSpy.first().first().value<qtidm::DownloadRecord>().totalBytes, fixtureSize);

        QFile file(request.targetPath);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), expectedPayload(fixtureSize));
    }

    void discoversSizeFromContentRangeWhenHeadOmitsIt()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        qtidm::CurlEpollDownloader downloader;
        QSignalSpy addedSpy(&downloader, &qtidm::CurlEpollDownloader::downloadAdded);
        QSignalSpy statusSpy(&downloader, &qtidm::CurlEpollDownloader::statusChanged);
        downloader.start();

        qtidm::DownloadRequest request;
        request.url = QUrl(QStringLiteral("http://127.0.0.1:%1/head-no-length-range.bin").arg(port_));
        request.targetPath = dir.path() + QStringLiteral("/head-no-length.bin");
        const auto id = downloader.enqueue(request);

        QVERIFY(waitForStatus(statusSpy, id, qtidm::DownloadStatus::Completed, 15000));
        downloader.stop();
        QCOMPARE(addedSpy.size(), 1);
        QCOMPARE(addedSpy.first().first().value<qtidm::DownloadRecord>().totalBytes, fixtureSize);
        QCOMPARE(QFileInfo(request.targetPath).size(), fixtureSize);
    }

    void safelyResumesUnknownLengthAfterDisconnect()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        qtidm::CurlEpollDownloader downloader;
        QSignalSpy progressSpy(&downloader, &qtidm::CurlEpollDownloader::progressChanged);
        QSignalSpy statusSpy(&downloader, &qtidm::CurlEpollDownloader::statusChanged);
        downloader.start();

        qtidm::DownloadRequest request;
        request.url = QUrl(QStringLiteral("http://127.0.0.1:%1/unknown-resume.bin").arg(port_));
        request.targetPath = dir.path() + QStringLiteral("/unknown-resume.bin");
        request.segments = 1;
        request.maxRetries = 2;
        request.retryBaseDelayMs = 10;
        const auto id = downloader.enqueue(request);

        QString message;
        QVERIFY2(waitForStatus(statusSpy, id, qtidm::DownloadStatus::Completed, 15000, &message),
                 qPrintable(message));
        downloader.stop();

        QFile file(request.targetPath);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.size(), fixtureSize);
        QCOMPARE(file.readAll(), expectedPayload(fixtureSize));
        QVERIFY(!progressSpy.isEmpty());
        const auto finalProgress = progressSpy.last();
        QCOMPARE(finalProgress.at(1).toLongLong(), fixtureSize);
        QCOMPARE(finalProgress.at(2).toLongLong(), fixtureSize);
    }

    void rejectsIgnoredUnknownLengthResumeWithoutAppending()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        qtidm::CurlEpollDownloader downloader;
        QSignalSpy statusSpy(&downloader, &qtidm::CurlEpollDownloader::statusChanged);
        downloader.start();

        qtidm::DownloadRequest request;
        request.url = QUrl(QStringLiteral("http://127.0.0.1:%1/ignored-unknown-resume.bin").arg(port_));
        request.targetPath = dir.path() + QStringLiteral("/ignored-resume.bin");
        request.segments = 1;
        request.maxRetries = 2;
        request.retryBaseDelayMs = 10;
        const auto id = downloader.enqueue(request);

        QString message;
        QVERIFY(waitForStatus(statusSpy, id, qtidm::DownloadStatus::Failed, 15000, &message));
        downloader.stop();
        QCOMPARE(message, QStringLiteral("server did not honor requested byte range"));
        QVERIFY(!QFileInfo::exists(request.targetPath));

        QFile partial(request.targetPath + QStringLiteral(".part"));
        QVERIFY(partial.open(QIODevice::ReadOnly));
        QCOMPARE(partial.size(), fixtureSize / 4);
        QCOMPARE(partial.readAll(), expectedPayload(fixtureSize / 4));
    }

    void preservesUnknownLengthPartialAcrossManualResume()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        constexpr qint64 preservedBytes = fixtureSize / 3;
        qtidm::DownloadRequest request;
        request.existingId = QStringLiteral("manual-unknown-resume");
        request.url = QUrl(QStringLiteral("http://127.0.0.1:%1/unknown-resume.bin?manual").arg(port_));
        request.targetPath = dir.path() + QStringLiteral("/manual-unknown-resume.bin");
        request.resumeSegments = {
            { 0, 0, -1, preservedBytes, qtidm::DownloadStatus::Paused }
        };

        QFile partial(request.targetPath + QStringLiteral(".part"));
        QVERIFY(partial.open(QIODevice::WriteOnly));
        QCOMPARE(partial.write(expectedPayload(preservedBytes)), preservedBytes);
        partial.close();

        qtidm::CurlEpollDownloader downloader;
        QSignalSpy statusSpy(&downloader, &qtidm::CurlEpollDownloader::statusChanged);
        downloader.start();
        const auto id = downloader.enqueue(request);

        QString message;
        QVERIFY2(waitForStatus(statusSpy, id, qtidm::DownloadStatus::Completed, 15000, &message),
                 qPrintable(message));
        downloader.stop();

        QFile completed(request.targetPath);
        QVERIFY(completed.open(QIODevice::ReadOnly));
        QCOMPARE(completed.size(), fixtureSize);
        QCOMPARE(completed.readAll(), expectedPayload(fixtureSize));
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

        const auto secondRenameId = downloader.enqueue(rename);
        QVERIFY(waitForStatus(statusSpy, secondRenameId, qtidm::DownloadStatus::Completed, 15000));
        downloader.stop();

        QCOMPARE(addedSpy.size(), 2);
        const auto firstRenamedPath = addedSpy.at(0).first().value<qtidm::DownloadRecord>().targetPath;
        const auto secondRenamedPath = addedSpy.at(1).first().value<qtidm::DownloadRecord>().targetPath;
        QVERIFY(firstRenamedPath.endsWith(QStringLiteral("existing (1).bin")));
        QVERIFY(secondRenamedPath.endsWith(QStringLiteral("existing (2).bin")));
        QVERIFY(QFileInfo::exists(firstRenamedPath));
        QVERIFY(QFileInfo::exists(secondRenamedPath));
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
        QVERIFY(downloader.sessionBytesReceived() > 0);
        QVERIFY(downloader.sessionBytesReceived() <= request.sessionDataLimitBytes);
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

    void rejectsResumeWhenEntityValidatorChanged()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        qtidm::DownloadRequest request;
        request.existingId = QStringLiteral("changed-entity-resume");
        request.url = QUrl(QStringLiteral("http://127.0.0.1:%1/changed-etag.bin").arg(port_));
        request.targetPath = dir.path() + QStringLiteral("/changed-entity.bin");
        request.expectedTotalBytes = fixtureSize;
        request.entityTag = QStringLiteral("\"old-entity\"");
        request.resumeSegments = {
            { 0, 0, fixtureSize - 1, 12, qtidm::DownloadStatus::Paused }
        };
        QFile partial(request.targetPath + QStringLiteral(".part"));
        QVERIFY(partial.open(QIODevice::WriteOnly));
        QCOMPARE(partial.write(expectedPayload(12)), qint64(12));
        partial.close();

        qtidm::CurlEpollDownloader downloader;
        QSignalSpy statusSpy(&downloader, &qtidm::CurlEpollDownloader::statusChanged);
        downloader.start();
        const auto id = downloader.enqueue(request);

        QString message;
        QVERIFY(waitForStatus(statusSpy, id, qtidm::DownloadStatus::Failed, 5000, &message));
        QCOMPARE(message, QStringLiteral("remote entity changed; resume refused"));
        downloader.stop();
        QCOMPARE(QFileInfo(request.targetPath + QStringLiteral(".part")).size(), qint64(12));
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

void forwardsCookiesAcrossRedirects()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        qtidm::CurlEpollDownloader downloader;
        QSignalSpy statusSpy(&downloader, &qtidm::CurlEpollDownloader::statusChanged);
        downloader.start();

        qtidm::DownloadRequest request;
        request.url = QUrl(QStringLiteral("http://127.0.0.1:%1/redirect-cookie.bin").arg(port_));
        request.targetPath = dir.path() + QStringLiteral("/cookie_redirect.bin");
        request.maximumRedirects = 5;
        request.segments = 2;
        const auto id = downloader.enqueue(request);

        QVERIFY(waitForStatus(statusSpy, id, qtidm::DownloadStatus::Completed, 10000));
        downloader.stop();

        QFile file(request.targetPath);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.size(), qint64(256 * 1024));
        QCOMPARE(file.readAll(), expectedPayload(256 * 1024));
    }

    void stripsCredentialsAndCookiesOnCrossOriginRedirect()
    {
        // 1. Direct unit assertion on cross-origin header policy
        {
            QVariantMap headers;
            headers.insert(QStringLiteral("User-Agent"), QStringLiteral("qtIDM/1.0"));
            headers.insert(QStringLiteral("Accept"), QStringLiteral("*/*"));
            headers.insert(QStringLiteral("Authorization"), QStringLiteral("Bearer secret"));
            headers.insert(QStringLiteral("Proxy-Authorization"), QStringLiteral("Basic dXNlcjpwYXNz"));
            headers.insert(QStringLiteral("X-Api-Key"), QStringLiteral("secret-token-123"));
            headers.insert(QStringLiteral("X-Auth-Token"), QStringLiteral("session-abc"));
            headers.insert(QStringLiteral("Cookie"), QStringLiteral("sid=xyz"));

            QString user = QStringLiteral("admin");
            QString pass = QStringLiteral("password123");

            qtidm::CurlEpollDownloader::applyCrossOriginHeaderPolicy(&headers, &user, &pass);

            QVERIFY(user.isEmpty());
            QVERIFY(pass.isEmpty());
            QVERIFY(headers.contains(QStringLiteral("User-Agent")));
            QVERIFY(headers.contains(QStringLiteral("Accept")));
            QVERIFY(!headers.contains(QStringLiteral("Authorization")));
            QVERIFY(!headers.contains(QStringLiteral("Proxy-Authorization")));
            QVERIFY(!headers.contains(QStringLiteral("X-Api-Key")));
            QVERIFY(!headers.contains(QStringLiteral("X-Auth-Token")));
            QVERIFY(!headers.contains(QStringLiteral("Cookie")));
        }

        // 2. Integration test over network redirect chain
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        qtidm::CurlEpollDownloader downloader;
        QSignalSpy statusSpy(&downloader, &qtidm::CurlEpollDownloader::statusChanged);
        downloader.start();

        qtidm::DownloadRequest request;
        request.url = QUrl(QStringLiteral("http://127.0.0.1:%1/cross-origin-auth-redirect.bin").arg(port_));
        request.targetPath = dir.path() + QStringLiteral("/cross_origin_auth.bin");
        request.headers.insert(QStringLiteral("Authorization"), QStringLiteral("Bearer secret_token_xyz"));
        request.headers.insert(QStringLiteral("Cookie"), QStringLiteral("secret_127_0_0_1=should_be_stripped"));
        request.headers.insert(QStringLiteral("X-Api-Key"), QStringLiteral("secret_api_key_456"));
        request.headers.insert(QStringLiteral("X-Auth-Token"), QStringLiteral("secret_auth_token_789"));
        request.headers.insert(QStringLiteral("X-Custom-Secret"), QStringLiteral("secret_custom_token"));
        request.headers.insert(QStringLiteral("User-Agent"), QStringLiteral("qtIDM-Security-Audit/1.0"));
        request.maximumRedirects = 5;
        request.segments = 2;
        const auto id = downloader.enqueue(request);

        QVERIFY(waitForStatus(statusSpy, id, qtidm::DownloadStatus::Completed, 10000));
        downloader.stop();

        QFile file(request.targetPath);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.size(), qint64(256 * 1024));
        QCOMPARE(file.readAll(), expectedPayload(256 * 1024));
    }

    void resumesExclusivelyFromDurableCheckpointsAfterCrash()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const auto targetPath = dir.path() + QStringLiteral("/durable_resume.bin");
        const auto stagingPath = targetPath + QStringLiteral(".part");

        // 1. Prepare initial staging file on disk with partial synchronized prefix
        // 2 segments: segment 0 [0, 131071], segment 1 [131072, 262143]
        // Segment 0 durable = 65536, volatile = 98304
        // Segment 1 durable = 32768, volatile = 65536
        qtidm::SparseFileWriter writer;
        QVERIFY(writer.open(stagingPath, 256 * 1024));
        const auto seg0Data = expectedPayload(65536);
        const auto seg1Data = expectedPayload(256 * 1024).mid(131072, 32768);
        QVERIFY(writer.writeAt(0, seg0Data.constData(), seg0Data.size()));
        QVERIFY(writer.writeAt(131072, seg1Data.constData(), seg1Data.size()));
        QVERIFY(writer.sync());
        writer.close();

        // 2. Prepare resumeSegments representing state loaded from SQLite
        // where volatile written was saved higher, but durableWritten is the authoritative checkpoint
        qtidm::DownloadRequest request;
        request.url = QUrl(QStringLiteral("http://127.0.0.1:%1/range.bin").arg(port_));
        request.targetPath = targetPath;
        request.expectedTotalBytes = 256 * 1024;
        request.segments = 2;
        request.resumeSegments = {
            qtidm::SegmentInfo(0, 0, 131071, 98304, 65536, qtidm::DownloadStatus::Queued),
            qtidm::SegmentInfo(1, 131072, 262143, 65536, 32768, qtidm::DownloadStatus::Queued)
        };

        qtidm::CurlEpollDownloader downloader;
        QSignalSpy statusSpy(&downloader, &qtidm::CurlEpollDownloader::statusChanged);
        downloader.start();

        const auto id = downloader.enqueue(request);
        QVERIFY(waitForStatus(statusSpy, id, qtidm::DownloadStatus::Completed, 10000));
        downloader.stop();

        // 3. Verify file completed with exact SHA-256 and byte content
        QFile file(targetPath);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.size(), qint64(256 * 1024));
        QCOMPARE(file.readAll(), expectedPayload(256 * 1024));
    }

    void validatesRedirectUrlPolicy()
    {
        QString error;

        // 1. Disallowed schemes
        QVERIFY(!qtidm::CurlEpollDownloader::validateRedirectUrl(
            QUrl(QStringLiteral("http://example.com/file")),
            QUrl(QStringLiteral("file:///etc/passwd")), false, &error));
        QVERIFY(error.contains(QStringLiteral("Disallowed redirect scheme 'file'")));

        QVERIFY(!qtidm::CurlEpollDownloader::validateRedirectUrl(
            QUrl(QStringLiteral("http://example.com/file")),
            QUrl(QStringLiteral("gopher://127.0.0.1:70/0")), false, &error));
        QVERIFY(error.contains(QStringLiteral("Disallowed redirect scheme 'gopher'")));

        QVERIFY(!qtidm::CurlEpollDownloader::validateRedirectUrl(
            QUrl(QStringLiteral("http://example.com/file")),
            QUrl(QStringLiteral("data:text/plain;base64,AAAA")), false, &error));
        QVERIFY(error.contains(QStringLiteral("Disallowed redirect scheme 'data'")));

        QVERIFY(!qtidm::CurlEpollDownloader::validateRedirectUrl(
            QUrl(QStringLiteral("http://example.com/file")),
            QUrl(QStringLiteral("javascript:alert(1)")), false, &error));
        QVERIFY(error.contains(QStringLiteral("Disallowed redirect scheme 'javascript'")));

        // HTTP cannot redirect to FTP
        QVERIFY(!qtidm::CurlEpollDownloader::validateRedirectUrl(
            QUrl(QStringLiteral("http://example.com/file")),
            QUrl(QStringLiteral("ftp://example.com/file")), false, &error));
        QVERIFY(error.contains(QStringLiteral("only HTTP and HTTPS are permitted")));

        // FTP cannot redirect to HTTP
        QVERIFY(!qtidm::CurlEpollDownloader::validateRedirectUrl(
            QUrl(QStringLiteral("ftp://example.com/file")),
            QUrl(QStringLiteral("http://example.com/file")), false, &error));
        QVERIFY(error.contains(QStringLiteral("only FTP is permitted")));

        // 2. Reject empty host / invalid URL
        QVERIFY(!qtidm::CurlEpollDownloader::validateRedirectUrl(
            QUrl(QStringLiteral("http://example.com/file")),
            QUrl(QStringLiteral("http:///path")), false, &error));
        QVERIFY(error.contains(QStringLiteral("missing a valid hostname")));

        // 3. Embedded credentials
        QVERIFY(!qtidm::CurlEpollDownloader::validateRedirectUrl(
            QUrl(QStringLiteral("http://example.com/file")),
            QUrl(QStringLiteral("http://admin:secret@cdn.example.com/file")), false, &error));
        QVERIFY(error.contains(QStringLiteral("embedded user credentials")));

        // 4. Private network & SSRF protection
        // Public -> Loopback (blocked by default)
        QVERIFY(!qtidm::CurlEpollDownloader::validateRedirectUrl(
            QUrl(QStringLiteral("http://example.com/file")),
            QUrl(QStringLiteral("http://127.0.0.1:8080/file")), false, &error));
        QVERIFY(error.contains(QStringLiteral("blocked for SSRF security")));

        // Public -> 10.0.0.0/8 (blocked by default)
        QVERIFY(!qtidm::CurlEpollDownloader::validateRedirectUrl(
            QUrl(QStringLiteral("http://example.com/file")),
            QUrl(QStringLiteral("http://10.1.2.3/file")), false, &error));
        QVERIFY(error.contains(QStringLiteral("blocked for SSRF security")));

        // Public -> 172.16.0.0/12 (blocked by default)
        QVERIFY(!qtidm::CurlEpollDownloader::validateRedirectUrl(
            QUrl(QStringLiteral("http://example.com/file")),
            QUrl(QStringLiteral("http://172.20.1.1/file")), false, &error));
        QVERIFY(error.contains(QStringLiteral("blocked for SSRF security")));

        // Public -> 192.168.0.0/16 (blocked by default)
        QVERIFY(!qtidm::CurlEpollDownloader::validateRedirectUrl(
            QUrl(QStringLiteral("http://example.com/file")),
            QUrl(QStringLiteral("http://192.168.1.1/file")), false, &error));
        QVERIFY(error.contains(QStringLiteral("blocked for SSRF security")));

        // Public -> Cloud metadata 169.254.169.254 (blocked by default)
        QVERIFY(!qtidm::CurlEpollDownloader::validateRedirectUrl(
            QUrl(QStringLiteral("http://example.com/file")),
            QUrl(QStringLiteral("http://169.254.169.254/latest/meta-data/")), false, &error));
        QVERIFY(error.contains(QStringLiteral("blocked for SSRF security")));

        // Public -> localhost (blocked by default)
        QVERIFY(!qtidm::CurlEpollDownloader::validateRedirectUrl(
            QUrl(QStringLiteral("http://example.com/file")),
            QUrl(QStringLiteral("http://localhost:8080/file")), false, &error));
        QVERIFY(error.contains(QStringLiteral("blocked for SSRF security")));

        // Public -> Private allowed when allowPrivateNetworks == true
        QVERIFY(qtidm::CurlEpollDownloader::validateRedirectUrl(
            QUrl(QStringLiteral("http://example.com/file")),
            QUrl(QStringLiteral("http://127.0.0.1:8080/file")), true, &error));

        // Private -> Private allowed (e.g. LAN server redirect)
        QVERIFY(qtidm::CurlEpollDownloader::validateRedirectUrl(
            QUrl(QStringLiteral("http://192.168.1.100/file")),
            QUrl(QStringLiteral("http://192.168.1.101/file")), false, &error));

        // Loopback -> Loopback allowed
        QVERIFY(qtidm::CurlEpollDownloader::validateRedirectUrl(
            QUrl(QStringLiteral("http://127.0.0.1:8080/file")),
            QUrl(QStringLiteral("http://127.0.0.1:8080/target")), false, &error));

        // 5. Whitelisted public redirects
        QVERIFY(qtidm::CurlEpollDownloader::validateRedirectUrl(
            QUrl(QStringLiteral("http://example.com/file")),
            QUrl(QStringLiteral("https://cdn.example.com/file")), false, &error));
        QVERIFY(qtidm::CurlEpollDownloader::validateRedirectUrl(
            QUrl(QStringLiteral("https://example.com/file")),
            QUrl(QStringLiteral("http://mirror.example.com/file")), false, &error));
        QVERIFY(qtidm::CurlEpollDownloader::validateRedirectUrl(
            QUrl(QStringLiteral("ftp://ftp.example.com/file")),
            QUrl(QStringLiteral("ftp://mirror.example.com/file")), false, &error));
    }

    void rejectsRedirectToDisallowedSchemes()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        qtidm::CurlEpollDownloader downloader;
        QSignalSpy statusSpy(&downloader, &qtidm::CurlEpollDownloader::statusChanged);
        downloader.start();

        // 1. file:// redirect
        {
            qtidm::DownloadRequest request;
            request.url = QUrl(QStringLiteral("http://127.0.0.1:%1/redirect-file.bin").arg(port_));
            request.targetPath = dir.path() + QStringLiteral("/file_redir.bin");
            request.maximumRedirects = 5;
            const auto id = downloader.enqueue(request);

            QVERIFY(waitForStatus(statusSpy, id, qtidm::DownloadStatus::Failed, 5000));
            bool sawSchemeError = false;
            for (const auto& sig : statusSpy) {
                if (sig.at(0).toString() == id && sig.at(1).value<qtidm::DownloadStatus>() == qtidm::DownloadStatus::Failed) {
                    if (sig.at(2).toString().contains(QStringLiteral("Disallowed redirect scheme 'file'"))) {
                        sawSchemeError = true;
                    }
                }
            }
            QVERIFY(sawSchemeError);
        }

        // 2. gopher:// redirect
        {
            statusSpy.clear();
            qtidm::DownloadRequest request;
            request.url = QUrl(QStringLiteral("http://127.0.0.1:%1/redirect-gopher.bin").arg(port_));
            request.targetPath = dir.path() + QStringLiteral("/gopher_redir.bin");
            request.maximumRedirects = 5;
            const auto id = downloader.enqueue(request);

            QVERIFY(waitForStatus(statusSpy, id, qtidm::DownloadStatus::Failed, 5000));
            bool sawSchemeError = false;
            for (const auto& sig : statusSpy) {
                if (sig.at(0).toString() == id && sig.at(1).value<qtidm::DownloadStatus>() == qtidm::DownloadStatus::Failed) {
                    if (sig.at(2).toString().contains(QStringLiteral("Disallowed redirect scheme 'gopher'"))) {
                        sawSchemeError = true;
                    }
                }
            }
            QVERIFY(sawSchemeError);
        }

        // 3. data: redirect
        {
            statusSpy.clear();
            qtidm::DownloadRequest request;
            request.url = QUrl(QStringLiteral("http://127.0.0.1:%1/redirect-unsupported-scheme.bin").arg(port_));
            request.targetPath = dir.path() + QStringLiteral("/data_redir.bin");
            request.maximumRedirects = 5;
            const auto id = downloader.enqueue(request);

            QVERIFY(waitForStatus(statusSpy, id, qtidm::DownloadStatus::Failed, 5000));
            bool sawSchemeError = false;
            for (const auto& sig : statusSpy) {
                if (sig.at(0).toString() == id && sig.at(1).value<qtidm::DownloadStatus>() == qtidm::DownloadStatus::Failed) {
                    if (sig.at(2).toString().contains(QStringLiteral("Disallowed redirect scheme 'data'"))) {
                        sawSchemeError = true;
                    }
                }
            }
            QVERIFY(sawSchemeError);
        }

        downloader.stop();
    }

    void rejectsRedirectWithEmbeddedCredentials()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        qtidm::CurlEpollDownloader downloader;
        QSignalSpy statusSpy(&downloader, &qtidm::CurlEpollDownloader::statusChanged);
        downloader.start();

        qtidm::DownloadRequest request;
        request.url = QUrl(QStringLiteral("http://127.0.0.1:%1/redirect-embedded-creds.bin").arg(port_));
        request.targetPath = dir.path() + QStringLiteral("/creds_redir.bin");
        request.maximumRedirects = 5;
        const auto id = downloader.enqueue(request);

        QVERIFY(waitForStatus(statusSpy, id, qtidm::DownloadStatus::Failed, 5000));
        bool sawCredsError = false;
        for (const auto& sig : statusSpy) {
            if (sig.at(0).toString() == id && sig.at(1).value<qtidm::DownloadStatus>() == qtidm::DownloadStatus::Failed) {
                if (sig.at(2).toString().contains(QStringLiteral("embedded user credentials"))) {
                    sawCredsError = true;
                }
            }
        }
        QVERIFY(sawCredsError);

        downloader.stop();
    }

void structuredCookiesRfc6265Scoping()
    {
        // 1. Format Unit Verifications
        {
            // Host-only cookie
            qtidm::CookieRecord hostOnly;
            hostOnly.name = QStringLiteral("c_host");
            hostOnly.value = QStringLiteral("v_host");
            hostOnly.domain = QStringLiteral("example.com");
            hostOnly.hostOnly = true;
            hostOnly.path = QStringLiteral("/api");
            hostOnly.secure = true;
            hostOnly.session = true;
            const auto line = qtidm::CurlEpollDownloader::toNetscapeCookieLine(hostOnly);
            QCOMPARE(line, QStringLiteral("example.com\tFALSE\t/api\tTRUE\t0\tc_host\tv_host"));
        }
        {
            // Domain cookie
            qtidm::CookieRecord domainCookie;
            domainCookie.name = QStringLiteral("c_domain");
            domainCookie.value = QStringLiteral("v_domain");
            domainCookie.domain = QStringLiteral(".example.com");
            domainCookie.hostOnly = false;
            domainCookie.path = QStringLiteral("/");
            domainCookie.secure = false;
            domainCookie.session = false;
            domainCookie.expirationDate = 1893456000;
            const auto line = qtidm::CurlEpollDownloader::toNetscapeCookieLine(domainCookie);
            QCOMPARE(line, QStringLiteral(".example.com\tTRUE\t/\tFALSE\t1893456000\tc_domain\tv_domain"));
        }

        // 2. Integration with Downloader & Test Server
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        qtidm::CurlEpollDownloader downloader;
        QSignalSpy statusSpy(&downloader, &qtidm::CurlEpollDownloader::statusChanged);
        downloader.start();

        // Test: Session cookie sent, Expired cookie dropped
        {
            qtidm::DownloadRequest request;
            request.url = QUrl(QStringLiteral("http://127.0.0.1:%1/check-cookies.bin?must=session_val&must_not=expired_val").arg(port_));
            request.targetPath = dir.path() + QStringLiteral("/session_test.bin");
            request.segments = 1;

            // Session cookie (valid)
            request.cookies.append(qtidm::CookieRecord(
                QStringLiteral("session_val"), QStringLiteral("active123"),
                QStringLiteral("127.0.0.1"), true, QStringLiteral("/"),
                false, false, true, 0));

            // Expired cookie (must not be sent)
            request.cookies.append(qtidm::CookieRecord(
                QStringLiteral("expired_val"), QStringLiteral("dead456"),
                QStringLiteral("127.0.0.1"), true, QStringLiteral("/"),
                false, false, false, 1000));

            const auto id = downloader.enqueue(request);
            QVERIFY(waitForStatus(statusSpy, id, qtidm::DownloadStatus::Completed, 5000));
        }

        // Test: Secure cookie is NOT transmitted over insecure HTTP
        {
            statusSpy.clear();
            qtidm::DownloadRequest request;
            request.url = QUrl(QStringLiteral("http://127.0.0.1:%1/check-cookies.bin?must_not=secret_secure_val").arg(port_));
            request.targetPath = dir.path() + QStringLiteral("/secure_http_test.bin");
            request.segments = 1;

            // Secure cookie (should NOT be sent over HTTP)
            request.cookies.append(qtidm::CookieRecord(
                QStringLiteral("secret_secure_val"), QStringLiteral("tls_only_token"),
                QStringLiteral("127.0.0.1"), true, QStringLiteral("/"),
                true, true, true, 0));

            const auto id = downloader.enqueue(request);
            QVERIFY(waitForStatus(statusSpy, id, qtidm::DownloadStatus::Completed, 5000));
        }

        // Test: Path restriction - cookie for /check-cookies.bin is transmitted
        {
            statusSpy.clear();
            qtidm::DownloadRequest request;
            request.url = QUrl(QStringLiteral("http://127.0.0.1:%1/check-cookies.bin?must=path_specific_token").arg(port_));
            request.targetPath = dir.path() + QStringLiteral("/path_match_test.bin");
            request.segments = 1;

            request.cookies.append(qtidm::CookieRecord(
                QStringLiteral("path_specific_token"), QStringLiteral("scoped_ok"),
                QStringLiteral("127.0.0.1"), true, QStringLiteral("/check-cookies.bin"),
                false, false, true, 0));

            const auto id = downloader.enqueue(request);
            QVERIFY(waitForStatus(statusSpy, id, qtidm::DownloadStatus::Completed, 5000));
        }

        // Test: Path restriction - cookie for /other-path is NOT transmitted
        {
            statusSpy.clear();
            qtidm::DownloadRequest request;
            request.url = QUrl(QStringLiteral("http://127.0.0.1:%1/check-cookies.bin?must_not=unmatched_path_token").arg(port_));
            request.targetPath = dir.path() + QStringLiteral("/path_mismatch_test.bin");
            request.segments = 1;

            request.cookies.append(qtidm::CookieRecord(
                QStringLiteral("unmatched_path_token"), QStringLiteral("should_be_omitted"),
                QStringLiteral("127.0.0.1"), true, QStringLiteral("/unrelated/path"),
                false, false, true, 0));

            const auto id = downloader.enqueue(request);
            QVERIFY(waitForStatus(statusSpy, id, qtidm::DownloadStatus::Completed, 5000));
        }

        downloader.stop();
    }

void persistsDurableCheckpointsWithAcknowledgement()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        qtidm::DownloadRepository repo;
        const auto dbPath = dir.path() + QStringLiteral("/ack_test.sqlite3");
        QVERIFY(repo.open(dbPath));

        qtidm::CurlEpollDownloader downloader;
        downloader.setPersistenceHandler([&repo](const QString& id, const QVector<qtidm::SegmentInfo>& segs) {
            return repo.checkpoint(id, segs);
        });

        QSignalSpy statusSpy(&downloader, &qtidm::CurlEpollDownloader::statusChanged);
        downloader.start();

        qtidm::DownloadRequest request;
        request.url = QUrl(QStringLiteral("http://127.0.0.1:%1/range.bin").arg(port_));
        request.targetPath = dir.path() + QStringLiteral("/ack_test.bin");
        request.segments = 2;

        connect(&downloader, &qtidm::CurlEpollDownloader::downloadAdded,
            [&repo](const qtidm::DownloadRecord& record) {
                repo.upsertDownload(record);
            });

        connect(&downloader, &qtidm::CurlEpollDownloader::segmentsChanged,
            [&downloader, &repo](const QString& id, const QVector<qtidm::SegmentInfo>& segs) {
                if (repo.checkpoint(id, segs)) {
                    downloader.acknowledgeCheckpoint(id, segs);
                }
            });

        const auto id = downloader.enqueue(request);
        QVERIFY(waitForStatus(statusSpy, id, qtidm::DownloadStatus::Completed, 10000));
        downloader.stop();

        const auto records = repo.listDownloads();
        QVERIFY(!records.isEmpty());
        QCOMPARE(records.first().id, id);
        for (const auto& s : records.first().segments) {
            QVERIFY2(s.durableWritten > 0, "Durable written must advance via acknowledged checkpoints");
            QVERIFY2(s.durableWritten <= s.written, "Durable written must not exceed volatile written");
        }
    }

    void persistsDynamicSplitBeforeTransfersStart()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        qtidm::DownloadRepository repo;
        const auto dbPath = dir.path() + QStringLiteral("/split_persist.sqlite3");
        QVERIFY(repo.open(dbPath));

        qtidm::CurlEpollDownloader downloader;
        std::atomic<int> splitsPersisted = 0;
        std::atomic<bool> injectFailure = false;

        downloader.setPersistenceHandler([&repo, &splitsPersisted, &injectFailure](const QString& id, const QVector<qtidm::SegmentInfo>& segs) {
            if (segs.size() > 1) {
                splitsPersisted++;
                if (injectFailure.load()) {
                    return false; // Injected persistence failure
                }
            }
            return repo.checkpoint(id, segs);
        });

        QSignalSpy statusSpy(&downloader, &qtidm::CurlEpollDownloader::statusChanged);
        downloader.start();

        // 1. Successful dynamic split with prior persistence
        {
            qtidm::DownloadRequest request;
            request.url = QUrl(QStringLiteral("http://127.0.0.1:%1/range.bin").arg(port_));
            request.targetPath = dir.path() + QStringLiteral("/split_success.bin");
            request.segments = 2;
            request.dynamicSegmentation = true;

            connect(&downloader, &qtidm::CurlEpollDownloader::downloadAdded,
                [&repo](const qtidm::DownloadRecord& record) {
                    repo.upsertDownload(record);
                });

            const auto id = downloader.enqueue(request);
            QVERIFY(waitForStatus(statusSpy, id, qtidm::DownloadStatus::Completed, 15000));
            QVERIFY(splitsPersisted.load() > 0);
        }

        // 2. Injected persistence failure safely rolls back split and completes download
        {
            injectFailure.store(true);
            statusSpy.clear();

            qtidm::DownloadRequest request;
            request.url = QUrl(QStringLiteral("http://127.0.0.1:%1/range.bin").arg(port_));
            request.targetPath = dir.path() + QStringLiteral("/split_failover.bin");
            request.segments = 2;
            request.dynamicSegmentation = true;

            const auto id = downloader.enqueue(request);
            QVERIFY(waitForStatus(statusSpy, id, qtidm::DownloadStatus::Completed, 15000));

            QFile file(request.targetPath);
            QVERIFY(file.open(QIODevice::ReadOnly));
            QCOMPARE(file.size(), fixtureSize);
            QCOMPARE(file.readAll(), expectedPayload(fixtureSize));
        }

        downloader.stop();
    }

    void epollSocketGenerationsPreventStaleEventDispatch()
    {
        qtidm::CurlEpollDownloader downloader;

        // 1. Register socket 42
        downloader.updateSocket(42, CURL_POLL_IN);
        const auto gen1 = downloader.socketGeneration(42);
        QVERIFY(gen1 > 0);

        // Matching generation event is accepted
        QVERIFY(!downloader.isStaleEvent(42, gen1));

        // Stale or future generations are dropped
        QVERIFY(downloader.isStaleEvent(42, gen1 - 1));
        QVERIFY(downloader.isStaleEvent(42, gen1 + 1));
        QVERIFY(downloader.isStaleEvent(42, 0));

        // 2. Remove socket 42
        downloader.removeSocket(42);
        QCOMPARE(downloader.socketGeneration(42), 0U);
        // Events arriving after close are dropped as stale
        QVERIFY(downloader.isStaleEvent(42, gen1));

        // 3. Re-register socket 42 (simulating OS recycling the file descriptor)
        downloader.updateSocket(42, CURL_POLL_IN);
        const auto gen2 = downloader.socketGeneration(42);
        QVERIFY(gen2 > gen1);

        // Stale event from old socket lifespan is dropped
        QVERIFY(downloader.isStaleEvent(42, gen1));

        // Event for new socket lifespan is accepted
        QVERIFY(!downloader.isStaleEvent(42, gen2));

        downloader.removeSocket(42);
    }

    void epollGenerationCounterOverflowHandling()
    {
        qtidm::CurlEpollDownloader downloader;

        // Simulate generation counter near 32-bit ceiling
        downloader.setNextGenerationForTesting(0xFFFFFFFFU - 1);

        // Register socket 50: generation reaches 0xFFFFFFFF
        downloader.updateSocket(50, CURL_POLL_IN);
        QCOMPARE(downloader.socketGeneration(50), 0xFFFFFFFFU);
        QVERIFY(!downloader.isStaleEvent(50, 0xFFFFFFFFU));

        // Register socket 51: generation overflows past 0xFFFFFFFF
        // Invariant: generation wraps around to 1 (skipping 0)
        downloader.updateSocket(51, CURL_POLL_IN);
        QCOMPARE(downloader.socketGeneration(51), 1U);
        QVERIFY(!downloader.isStaleEvent(51, 1U));

        // Generation 0 is always treated as stale / invalid
        QVERIFY(downloader.isStaleEvent(51, 0U));

        // Old event with pre-overflow generation is dropped as stale
        QVERIFY(downloader.isStaleEvent(51, 0xFFFFFFFFU));

        downloader.removeSocket(50);
        downloader.removeSocket(51);
    }

    void http2FailureTriggersAutomaticFallbackToHttp1()
    {
        QProcess h2Server;
        h2Server.setProgram(QStringLiteral("python3"));
        h2Server.setArguments({
            QStringLiteral(QTIDM_SOURCE_DIR) + QStringLiteral("/tests/fixtures/http2_mock_server.py"),
            QStringLiteral("fallback")
        });
        h2Server.setProcessChannelMode(QProcess::SeparateChannels);
        h2Server.start();
        QVERIFY(h2Server.waitForStarted());
        QVERIFY(h2Server.waitForReadyRead(5000));
        const int h2Port = QString::fromUtf8(h2Server.readLine()).trimmed().toInt();
        QVERIFY(h2Port > 0);

        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        qtidm::CurlEpollDownloader downloader;
        QSignalSpy statusSpy(&downloader, &qtidm::CurlEpollDownloader::statusChanged);
        downloader.start();

        qtidm::DownloadRequest request;
        request.url = QUrl(QStringLiteral("http://127.0.0.1:%1/data.bin").arg(h2Port));
        request.targetPath = dir.path() + QStringLiteral("/h2_fallback.bin");
        request.segments = 1;
        request.maxRetries = 3;
        request.retryBaseDelayMs = 50;

        const auto id = downloader.enqueue(request);
        QVERIFY(waitForStatus(statusSpy, id, qtidm::DownloadStatus::Completed, 10000));
        downloader.stop();

        h2Server.terminate();
        h2Server.waitForFinished(3000);

        QFile file(request.targetPath);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.size(), qint64(8192));
        QCOMPARE(file.readAll(), expectedPayload(8192));
    }

    void validatesDnsRebindingAndAlternativeIpRepresentations()
    {
        // 1. Alternative IPv4 representations: decimal, octal, hex
        QVERIFY(qtidm::CurlEpollDownloader::isPrivateOrLoopbackHost(QStringLiteral("2130706433"))); // 127.0.0.1 in decimal
        QVERIFY(qtidm::CurlEpollDownloader::isPrivateOrLoopbackHost(QStringLiteral("0177.0.0.1"))); // 127.0.0.1 in octal
        QVERIFY(qtidm::CurlEpollDownloader::isPrivateOrLoopbackHost(QStringLiteral("0x7f000001"))); // 127.0.0.1 in hex

        // 2. IPv6 bracketed and literal variants
        QVERIFY(qtidm::CurlEpollDownloader::isPrivateOrLoopbackHost(QStringLiteral("[::1]")));
        QVERIFY(qtidm::CurlEpollDownloader::isPrivateOrLoopbackHost(QStringLiteral("[fe80::1]")));
        QVERIFY(qtidm::CurlEpollDownloader::isPrivateOrLoopbackHost(QStringLiteral("[fd00:ec2::254]"))); // AWS IPv6 IMDS

        // 3. Cloud metadata endpoints
        QVERIFY(qtidm::CurlEpollDownloader::isPrivateOrLoopbackHost(QStringLiteral("169.254.169.254")));
        QVERIFY(qtidm::CurlEpollDownloader::isPrivateOrLoopbackHost(QStringLiteral("100.100.100.200"))); // Alibaba metadata
        QVERIFY(qtidm::CurlEpollDownloader::isPrivateOrLoopbackHost(QStringLiteral("metadata.google.internal")));
        QVERIFY(qtidm::CurlEpollDownloader::isPrivateOrLoopbackHost(QStringLiteral("metadata")));

        // 4. Single-label local network names
        QVERIFY(qtidm::CurlEpollDownloader::isPrivateOrLoopbackHost(QStringLiteral("router")));
        QVERIFY(qtidm::CurlEpollDownloader::isPrivateOrLoopbackHost(QStringLiteral("nas")));
        QVERIFY(qtidm::CurlEpollDownloader::isPrivateOrLoopbackHost(QStringLiteral("intranet")));

        // 5. DNS resolution check (localhost resolves to 127.0.0.1 / ::1)
        QVERIFY(qtidm::CurlEpollDownloader::isPrivateOrLoopbackHost(QStringLiteral("localhost")));

        // 6. Public hostname
        QVERIFY(!qtidm::CurlEpollDownloader::isPrivateOrLoopbackHost(QStringLiteral("example.com")));

        // 7. validateRedirectUrl SSRF containment: redirect from public origin to alternative/DNS private endpoints
        const QUrl publicOrigin(QStringLiteral("https://cdn.example.com/download/file.tar.gz"));
        QString error;

        // Redirect to decimal 127.0.0.1
        QVERIFY(!qtidm::CurlEpollDownloader::validateRedirectUrl(
            publicOrigin, QUrl(QStringLiteral("http://2130706433/admin")), false, &error));
        QVERIFY(error.contains(QStringLiteral("SSRF")));

        // Redirect to bracketed IPv6 loopback
        QVERIFY(!qtidm::CurlEpollDownloader::validateRedirectUrl(
            publicOrigin, QUrl(QStringLiteral("http://[::1]/internal")), false, &error));
        QVERIFY(error.contains(QStringLiteral("SSRF")));

        // Redirect to GCP metadata
        QVERIFY(!qtidm::CurlEpollDownloader::validateRedirectUrl(
            publicOrigin, QUrl(QStringLiteral("http://metadata.google.internal/computeMetadata/v1/")), false, &error));
        QVERIFY(error.contains(QStringLiteral("SSRF")));

        // Redirect to Alibaba metadata
        QVERIFY(!qtidm::CurlEpollDownloader::validateRedirectUrl(
            publicOrigin, QUrl(QStringLiteral("http://100.100.100.200/latest/meta-data/")), false, &error));
        QVERIFY(error.contains(QStringLiteral("SSRF")));
    }

    void cookieStandardsScopingAndHttpOnly()
    {
        // 1. Host-only cookie: includeSubdomains must be FALSE and domain must omit leading dot
        qtidm::CookieRecord hostOnlyCookie(QStringLiteral("sess"), QStringLiteral("h_val"), QStringLiteral(".example.com"), true);
        const auto hostLine = qtidm::CurlEpollDownloader::toNetscapeCookieLine(hostOnlyCookie);
        QVERIFY(hostLine.startsWith(QStringLiteral("example.com\tFALSE\t/")));

        // 2. Domain cookie: includeSubdomains must be TRUE and domain must have leading dot
        qtidm::CookieRecord domainCookie(QStringLiteral("dom_sess"), QStringLiteral("d_val"), QStringLiteral("example.com"), false);
        const auto domainLine = qtidm::CurlEpollDownloader::toNetscapeCookieLine(domainCookie);
        QVERIFY(domainLine.startsWith(QStringLiteral(".example.com\tTRUE\t/")));

        // 3. HttpOnly cookie: domain must be prefixed with #HttpOnly_
        qtidm::CookieRecord httpOnlyCookie(QStringLiteral("sec_token"), QStringLiteral("secret"), QStringLiteral("example.com"),
                                           true, QStringLiteral("/"), false, true /*httpOnly*/);
        const auto httpOnlyLine = qtidm::CurlEpollDownloader::toNetscapeCookieLine(httpOnlyCookie);
        QVERIFY(httpOnlyLine.startsWith(QStringLiteral("#HttpOnly_example.com\tFALSE\t/")));

        // 4. Session cookie: expiration column must be 0
        qtidm::CookieRecord sessionCookie(QStringLiteral("session_var"), QStringLiteral("active"), QStringLiteral("example.com"),
                                          true, QStringLiteral("/"), false, false, true /*session*/, 0);
        const auto sessionLine = qtidm::CurlEpollDownloader::toNetscapeCookieLine(sessionCookie);
        QVERIFY(sessionLine.contains(QStringLiteral("\t0\tsession_var\tactive")));

        // 5. Multiple cookies with same name but different paths remain distinct
        qtidm::CookieRecord rootPathCookie(QStringLiteral("token"), QStringLiteral("root_val"), QStringLiteral("example.com"), true, QStringLiteral("/"));
        qtidm::CookieRecord apiPathCookie(QStringLiteral("token"), QStringLiteral("api_val"), QStringLiteral("example.com"), true, QStringLiteral("/api"));
        const auto rootLine = qtidm::CurlEpollDownloader::toNetscapeCookieLine(rootPathCookie);
        const auto apiLine = qtidm::CurlEpollDownloader::toNetscapeCookieLine(apiPathCookie);
        QVERIFY(rootLine.contains(QStringLiteral("\t/\t")));
        QVERIFY(apiLine.contains(QStringLiteral("\t/api\t")));
        QVERIFY(rootLine != apiLine);

        // 6. Same cookie name on two different domains remain distinct
        qtidm::CookieRecord dom1Cookie(QStringLiteral("auth_token"), QStringLiteral("val1"), QStringLiteral("sub1.example.com"), false);
        qtidm::CookieRecord dom2Cookie(QStringLiteral("auth_token"), QStringLiteral("val2"), QStringLiteral("sub2.example.com"), false);
        const auto dom1Line = qtidm::CurlEpollDownloader::toNetscapeCookieLine(dom1Cookie);
        const auto dom2Line = qtidm::CurlEpollDownloader::toNetscapeCookieLine(dom2Cookie);
        QVERIFY(dom1Line.contains(QStringLiteral(".sub1.example.com")));
        QVERIFY(dom2Line.contains(QStringLiteral(".sub2.example.com")));
        QVERIFY(dom1Line != dom2Line);
    }

    void http2MultiplexesConcurrentStreamsOverSingleConnection()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto statsPath = dir.path() + QStringLiteral("/h2_stats.json");

        QProcess h2Server;
        h2Server.setProgram(QStringLiteral("python3"));
        h2Server.setArguments({
            QStringLiteral(QTIDM_SOURCE_DIR) + QStringLiteral("/tests/fixtures/http2_mock_server.py"),
            QStringLiteral("multiplex"),
            statsPath
        });
        h2Server.setProcessChannelMode(QProcess::SeparateChannels);
        h2Server.start();
        QVERIFY(h2Server.waitForStarted());
        QVERIFY(h2Server.waitForReadyRead(5000));
        const int h2Port = QString::fromUtf8(h2Server.readLine()).trimmed().toInt();
        QVERIFY(h2Port > 0);

        qtidm::CurlEpollDownloader downloader;
        QSignalSpy statusSpy(&downloader, &qtidm::CurlEpollDownloader::statusChanged);
        downloader.start();

        // Enqueue two concurrent transfers to the same HTTP/2 host
        qtidm::DownloadRequest r1;
        r1.url = QUrl(QStringLiteral("http://127.0.0.1:%1/stream1.bin").arg(h2Port));
        r1.targetPath = dir.path() + QStringLiteral("/stream1.bin");
        r1.segments = 1;

        qtidm::DownloadRequest r2;
        r2.url = QUrl(QStringLiteral("http://127.0.0.1:%1/stream2.bin").arg(h2Port));
        r2.targetPath = dir.path() + QStringLiteral("/stream2.bin");
        r2.segments = 1;

        const auto id1 = downloader.enqueue(r1);
        const auto id2 = downloader.enqueue(r2);
        QVERIFY(waitForStatus(statusSpy, id1, qtidm::DownloadStatus::Completed, 6000));
        QVERIFY(waitForStatus(statusSpy, id2, qtidm::DownloadStatus::Completed, 6000));
        downloader.stop();

        h2Server.terminate();
        h2Server.waitForFinished(3000);

        // Verify payload integrity for both streams
        QFile f1(r1.targetPath);
        QVERIFY(f1.open(QIODevice::ReadOnly));
        QCOMPARE(f1.size(), qint64(8192));

        QFile f2(r2.targetPath);
        QVERIFY(f2.open(QIODevice::ReadOnly));
        QCOMPARE(f2.size(), qint64(8192));

        // Verify that the server served multiple concurrent streams over a SINGLE TCP connection
        QFile statsFile(statsPath);
        QVERIFY(statsFile.open(QIODevice::ReadOnly));
        const auto statsDoc = QJsonDocument::fromJson(statsFile.readAll());
        QVERIFY(!statsDoc.isNull());
        const auto statsObj = statsDoc.object();

        const int connections = statsObj.value(QStringLiteral("connections")).toInt();
        const auto streams = statsObj.value(QStringLiteral("streams")).toArray();

        // Proves that multiple streams multiplexed over 1 single TCP connection!
        QCOMPARE(connections, 1);
        QVERIFY(streams.size() >= 2);
    }

    void http2ServerNoUpgradeStaysOnHttp1()
    {
        QProcess h2Server;
        h2Server.setProgram(QStringLiteral("python3"));
        h2Server.setArguments({
            QStringLiteral(QTIDM_SOURCE_DIR) + QStringLiteral("/tests/fixtures/http2_mock_server.py"),
            QStringLiteral("no_upgrade")
        });
        h2Server.setProcessChannelMode(QProcess::SeparateChannels);
        h2Server.start();
        QVERIFY(h2Server.waitForStarted());
        QVERIFY(h2Server.waitForReadyRead(5000));
        const int h2Port = QString::fromUtf8(h2Server.readLine()).trimmed().toInt();
        QVERIFY(h2Port > 0);

        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        qtidm::CurlEpollDownloader downloader;
        QSignalSpy statusSpy(&downloader, &qtidm::CurlEpollDownloader::statusChanged);
        downloader.start();

        qtidm::DownloadRequest request;
        request.url = QUrl(QStringLiteral("http://127.0.0.1:%1/data.bin").arg(h2Port));
        request.targetPath = dir.path() + QStringLiteral("/h2_no_upgrade.bin");
        request.segments = 1;

        const auto id = downloader.enqueue(request);
        QVERIFY(waitForStatus(statusSpy, id, qtidm::DownloadStatus::Completed, 5000));
        downloader.stop();

        h2Server.terminate();
        h2Server.waitForFinished(3000);

        QFile file(request.targetPath);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.size(), qint64(8192));
        QCOMPARE(file.readAll(), expectedPayload(8192));
    }

    void dnsValidationPinsResolvedIpViaCurlOptResolve()
    {
        // 1. Verify that isPrivateOrLoopbackHost populates resolvedPublicIp for a public domain
        QString publicIp;
        const bool isPriv = qtidm::CurlEpollDownloader::isPrivateOrLoopbackHost(QStringLiteral("example.com"), &publicIp);
        QVERIFY(!isPriv);
        QVERIFY(!publicIp.isEmpty());
        struct in_addr in4;
        struct in6_addr in6;
        const auto ipBytes = publicIp.toUtf8();
        QVERIFY(::inet_aton(ipBytes.constData(), &in4) == 1 || ::inet_pton(AF_INET6, ipBytes.constData(), &in6) == 1);
        QVERIFY(publicIp != QStringLiteral("127.0.0.1"));

        // 2. Address Pinning via CURLOPT_RESOLVE in Downloader:
        // Enqueue a request to a simulated non-existent domain "rebinding-test.invalid",
        // but pin its IP to 127.0.0.1 via request.pinnedIp.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        qtidm::CurlEpollDownloader downloader;
        QSignalSpy statusSpy(&downloader, &qtidm::CurlEpollDownloader::statusChanged);
        downloader.start();

        qtidm::DownloadRequest request;
        request.url = QUrl(QStringLiteral("http://rebinding-test.invalid:%1/range.bin").arg(port_));
        request.targetPath = dir.path() + QStringLiteral("/pinned_resolve_test.bin");
        request.segments = 1;
        request.pinnedIp = QStringLiteral("127.0.0.1"); // Pin directly to test server loopback IP
        request.allowPrivateNetworkRedirects = true;

        const auto id = downloader.enqueue(request);
        // If CURLOPT_RESOLVE pins the IP, libcurl connects to 127.0.0.1 without querying DNS
        QVERIFY(waitForStatus(statusSpy, id, qtidm::DownloadStatus::Completed, 5000));
        downloader.stop();

        QFile file(request.targetPath);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.size(), fixtureSize);
    }

    void http2HttpsAlpnMultiplexesConcurrentStreamsOverSingleTlsConnection()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto statsPath = dir.path() + QStringLiteral("/h2_alpn_stats.json");

        QProcess h2Server;
        h2Server.setProgram(QStringLiteral("python3"));
        h2Server.setArguments({
            QStringLiteral(QTIDM_SOURCE_DIR) + QStringLiteral("/tests/fixtures/http2_mock_server.py"),
            QStringLiteral("https_alpn"),
            statsPath
        });
        h2Server.setProcessChannelMode(QProcess::SeparateChannels);
        h2Server.start();
        QVERIFY(h2Server.waitForStarted());
        QVERIFY(h2Server.waitForReadyRead(5000));
        const int h2Port = QString::fromUtf8(h2Server.readLine()).trimmed().toInt();
        QVERIFY(h2Port > 0);

        qtidm::CurlEpollDownloader downloader;
        QSignalSpy statusSpy(&downloader, &qtidm::CurlEpollDownloader::statusChanged);
        downloader.start();

        // Enqueue two concurrent transfers to the same HTTPS HTTP/2 host with ALPN
        qtidm::DownloadRequest r1;
        r1.url = QUrl(QStringLiteral("https://127.0.0.1:%1/stream1.bin").arg(h2Port));
        r1.targetPath = dir.path() + QStringLiteral("/alpn_stream1.bin");
        r1.segments = 1;
        r1.insecureSkipSslVerification = true;
        r1.allowPrivateNetworkRedirects = true;

        qtidm::DownloadRequest r2;
        r2.url = QUrl(QStringLiteral("https://127.0.0.1:%1/stream2.bin").arg(h2Port));
        r2.targetPath = dir.path() + QStringLiteral("/alpn_stream2.bin");
        r2.segments = 1;
        r2.insecureSkipSslVerification = true;
        r2.allowPrivateNetworkRedirects = true;

        const auto id1 = downloader.enqueue(r1);
        const auto id2 = downloader.enqueue(r2);
        QVERIFY(waitForStatus(statusSpy, id1, qtidm::DownloadStatus::Completed, 6000));
        QVERIFY(waitForStatus(statusSpy, id2, qtidm::DownloadStatus::Completed, 6000));
        downloader.stop();

        h2Server.terminate();
        h2Server.waitForFinished(3000);

        // Verify payload integrity for both streams
        QFile f1(r1.targetPath);
        QVERIFY(f1.open(QIODevice::ReadOnly));
        QCOMPARE(f1.size(), qint64(8192));

        QFile f2(r2.targetPath);
        QVERIFY(f2.open(QIODevice::ReadOnly));
        QCOMPARE(f2.size(), qint64(8192));

        // Verify that TLS ALPN negotiated 'h2' and served multiple concurrent streams over a SINGLE TLS connection
        QFile statsFile(statsPath);
        QVERIFY(statsFile.open(QIODevice::ReadOnly));
        const auto statsDoc = QJsonDocument::fromJson(statsFile.readAll());
        QVERIFY(!statsDoc.isNull());
        const auto statsObj = statsDoc.object();

        const auto alpn = statsObj.value(QStringLiteral("alpn")).toString();
        const int maxStreams = statsObj.value(QStringLiteral("max_streams_on_single_connection")).toInt();
        const auto streamIds = statsObj.value(QStringLiteral("multiplexed_stream_ids")).toArray();

        QCOMPARE(alpn, QStringLiteral("h2"));
        QVERIFY(maxStreams >= 2);
        QVERIFY(streamIds.size() >= 2);
    }


private:
    QProcess server_;
    int port_ = 0;
};

QTEST_MAIN(DownloaderIntegrationTest)
#include "DownloaderIntegrationTest.moc"
