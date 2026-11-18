#include "core/HlsDownloader.h"
#include "core/HlsPlaylistParser.h"

#include <openssl/evp.h>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest/QtTest>

namespace {

QByteArray encryptAes128(const QByteArray& plaintext, const QByteArray& key, const QByteArray& iv)
{
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    EVP_EncryptInit_ex(ctx, EVP_aes_128_cbc(), nullptr,
                       reinterpret_cast<const unsigned char*>(key.constData()),
                       reinterpret_cast<const unsigned char*>(iv.constData()));

    QByteArray ciphertext;
    ciphertext.resize(plaintext.size() + EVP_MAX_BLOCK_LENGTH);
    int outLen1 = 0;
    EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char*>(ciphertext.data()), &outLen1,
                      reinterpret_cast<const unsigned char*>(plaintext.constData()), plaintext.size());
    int outLen2 = 0;
    EVP_EncryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(ciphertext.data()) + outLen1, &outLen2);
    ciphertext.resize(outLen1 + outLen2);
    EVP_CIPHER_CTX_free(ctx);
    return ciphertext;
}

class HlsMockHttpServer : public QTcpServer {
    Q_OBJECT
public:
    QByteArray key = "0123456789abcdef"; // 16 bytes
    QByteArray iv  = "fedcba9876543210"; // 16 bytes
    QByteArray chunk0 = "CHUNK0_DATA_PAYLOAD_1234567890";
    QByteArray chunk1 = "CHUNK1_DATA_PAYLOAD_1234567890";
    QByteArray chunk2 = "CHUNK2_DATA_PAYLOAD_1234567890";
    QByteArray chunk3 = "CHUNK3_DATA_PAYLOAD_1234567890";

    HlsMockHttpServer(QObject* parent = nullptr) : QTcpServer(parent)
    {
        listen(QHostAddress::LocalHost, 0);
    }

protected:
    void incomingConnection(qintptr socketDescriptor) override
    {
        auto* socket = new QTcpSocket(this);
        socket->setSocketDescriptor(socketDescriptor);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
            const QByteArray req = socket->readAll();
            if (req.isEmpty()) return;

            const int firstSpace = req.indexOf(' ');
            const int secondSpace = req.indexOf(' ', firstSpace + 1);
            if (firstSpace < 0 || secondSpace < 0) return;

            const QString path = QString::fromUtf8(req.mid(firstSpace + 1, secondSpace - firstSpace - 1));

            QByteArray body;
            QString contentType = QStringLiteral("video/mp2t");

            if (path == QStringLiteral("/master.m3u8")) {
                contentType = QStringLiteral("application/vnd.apple.mpegurl");
                body = "#EXTM3U\n"
                       "#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=640x360\n"
                       "/360p.m3u8\n"
                       "#EXT-X-STREAM-INF:BANDWIDTH=3000000,RESOLUTION=1920x1080\n"
                       "/1080p/index.m3u8\n";
            } else if (path == QStringLiteral("/1080p/index.m3u8")) {
                contentType = QStringLiteral("application/vnd.apple.mpegurl");
                body = "#EXTM3U\n"
                       "#EXT-X-TARGETDURATION:6\n"
                       "#EXT-X-PLAYLIST-TYPE:VOD\n"
                       "#EXTINF:5.0,\n"
                       "/chunk0.ts\n"
                       "#EXTINF:5.0,\n"
                       "/chunk1.ts\n"
                       "#EXTINF:5.0,\n"
                       "/chunk2.ts\n"
                       "#EXTINF:5.0,\n"
                       "/chunk3.ts\n"
                       "#EXT-X-ENDLIST\n";
            } else if (path == QStringLiteral("/chunk0.ts")) {
                body = chunk0;
            } else if (path == QStringLiteral("/chunk1.ts")) {
                body = chunk1;
            } else if (path == QStringLiteral("/chunk2.ts")) {
                body = chunk2;
            } else if (path == QStringLiteral("/chunk3.ts")) {
                body = chunk3;
            } else if (path == QStringLiteral("/encrypted.m3u8")) {
                contentType = QStringLiteral("application/vnd.apple.mpegurl");
                body = "#EXTM3U\n"
                       "#EXT-X-TARGETDURATION:6\n"
                       "#EXT-X-KEY:METHOD=AES-128,URI=\"/key.bin\",IV=0x66656463626139383736353433323130\n"
                       "#EXTINF:5.0,\n"
                       "/enc_chunk0.ts\n"
                       "#EXTINF:5.0,\n"
                       "/enc_chunk1.ts\n"
                       "#EXT-X-ENDLIST\n";
            } else if (path == QStringLiteral("/key.bin")) {
                contentType = QStringLiteral("application/octet-stream");
                body = key;
            } else if (path == QStringLiteral("/enc_chunk0.ts")) {
                body = encryptAes128(chunk0, key, iv);
            } else if (path == QStringLiteral("/enc_chunk1.ts")) {
                body = encryptAes128(chunk1, key, iv);
            } else if (path == QStringLiteral("/multikey.m3u8")) {
                contentType = QStringLiteral("application/vnd.apple.mpegurl");
                body = "#EXTM3U\n"
                       "#EXT-X-TARGETDURATION:6\n"
                       "#EXT-X-KEY:METHOD=AES-128,URI=\"/key1.bin\",IV=0x66656463626139383736353433323130\n"
                       "#EXTINF:5.0,\n"
                       "/enc1_chunk0.ts\n"
                       "#EXT-X-KEY:METHOD=AES-128,URI=\"/key2.bin\",IV=0x30313233343536373839616263646566\n"
                       "#EXTINF:5.0,\n"
                       "/enc2_chunk1.ts\n"
                       "#EXT-X-ENDLIST\n";
            } else if (path == QStringLiteral("/key1.bin")) {
                contentType = QStringLiteral("application/octet-stream");
                body = "1111222233334444";
            } else if (path == QStringLiteral("/key2.bin")) {
                contentType = QStringLiteral("application/octet-stream");
                body = "5555666677778888";
            } else if (path == QStringLiteral("/enc1_chunk0.ts")) {
                body = encryptAes128(chunk0, "1111222233334444", iv);
            } else if (path == QStringLiteral("/enc2_chunk1.ts")) {
                const QByteArray iv2 = "0123456789abcdef";
                body = encryptAes128(chunk1, "5555666677778888", iv2);
            } else if (path == QStringLiteral("/dual_master.m3u8")) {
                contentType = QStringLiteral("application/vnd.apple.mpegurl");
                body = "#EXTM3U\n"
                       "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"audio-aac\",NAME=\"English\",DEFAULT=YES,AUTOSELECT=YES,LANGUAGE=\"en\",URI=\"/audio_index.m3u8\"\n"
                       "#EXT-X-STREAM-INF:BANDWIDTH=2000000,RESOLUTION=1280x720,AUDIO=\"audio-aac\"\n"
                       "/video_index.m3u8\n";
            } else if (path == QStringLiteral("/video_index.m3u8")) {
                contentType = QStringLiteral("application/vnd.apple.mpegurl");
                body = "#EXTM3U\n"
                       "#EXT-X-TARGETDURATION:6\n"
                       "#EXT-X-PLAYLIST-TYPE:VOD\n"
                       "#EXTINF:5.0,\n"
                       "/video_0.ts\n"
                       "#EXTINF:5.0,\n"
                       "/video_1.ts\n"
                       "#EXT-X-ENDLIST\n";
            } else if (path == QStringLiteral("/audio_index.m3u8")) {
                contentType = QStringLiteral("application/vnd.apple.mpegurl");
                body = "#EXTM3U\n"
                       "#EXT-X-TARGETDURATION:6\n"
                       "#EXT-X-PLAYLIST-TYPE:VOD\n"
                       "#EXTINF:5.0,\n"
                       "/audio_0.ts\n"
                       "#EXTINF:5.0,\n"
                       "/audio_1.ts\n"
                       "#EXT-X-ENDLIST\n";
            } else if (path == QStringLiteral("/video_0.ts")) {
                body = "VIDEO_PAYLOAD_CHUNK_0";
            } else if (path == QStringLiteral("/video_1.ts")) {
                body = "VIDEO_PAYLOAD_CHUNK_1";
            } else if (path == QStringLiteral("/audio_0.ts")) {
                body = "AUDIO_PAYLOAD_CHUNK_0";
            } else if (path == QStringLiteral("/audio_1.ts")) {
                body = "AUDIO_PAYLOAD_CHUNK_1";
            }

            QByteArray response;
            if (body.isEmpty() && path != QStringLiteral("/empty")) {
                response = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            } else {
                response = "HTTP/1.1 200 OK\r\nContent-Type: " + contentType.toUtf8()
                         + "\r\nContent-Length: " + QByteArray::number(body.size())
                         + "\r\nConnection: close\r\n\r\n" + body;
            }
            socket->write(response);
            socket->flush();
            socket->disconnectFromHost();
        });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    }
};

bool waitForHlsStatus(QSignalSpy& spy, const QString& expectedId, qtidm::DownloadStatus expectedStatus, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    qsizetype seen = 0;
    while (timer.elapsed() < timeoutMs) {
        while (seen < spy.size()) {
            const auto args = spy.at(seen++);
            if (args.at(0).toString() == expectedId && args.at(1).value<qtidm::DownloadStatus>() == expectedStatus) {
                return true;
            }
        }
        QTest::qWait(50);
    }
    return false;
}

} // namespace

class HlsDownloaderTest final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase()
    {
        qRegisterMetaType<qtidm::DownloadStatus>("qtidm::DownloadStatus");
        qRegisterMetaType<QVector<qtidm::SegmentInfo>>("QVector<qtidm::SegmentInfo>");
    }

    void downloadsHlsStreamInParallelAndAssembles()
    {
        HlsMockHttpServer server;
        QVERIFY(server.isListening());

        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString targetPath = tempDir.filePath(QStringLiteral("output.ts"));

        qtidm::HlsDownloader downloader;
        QSignalSpy statusSpy(&downloader, &qtidm::HlsDownloader::statusChanged);
        QSignalSpy progressSpy(&downloader, &qtidm::HlsDownloader::progressChanged);

        const QUrl masterUrl(QStringLiteral("http://127.0.0.1:%1/master.m3u8").arg(server.serverPort()));
        const QString downloadId = QStringLiteral("test-hls-1");

        QVERIFY(downloader.start(downloadId, masterUrl, targetPath, {}, {}, 4));
        QVERIFY(waitForHlsStatus(statusSpy, downloadId, qtidm::DownloadStatus::Completed, 10000));

        QFile target(targetPath);
        QVERIFY(target.exists());
        QVERIFY(target.open(QIODevice::ReadOnly));
        const QByteArray content = target.readAll();

        const QByteArray expected = server.chunk0 + server.chunk1 + server.chunk2 + server.chunk3;
        QCOMPARE(content, expected);
    }

    void downloadsAndDecryptsAes128HlsStream()
    {
        HlsMockHttpServer server;
        QVERIFY(server.isListening());

        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString targetPath = tempDir.filePath(QStringLiteral("output_decrypted.ts"));

        qtidm::HlsDownloader downloader;
        QSignalSpy statusSpy(&downloader, &qtidm::HlsDownloader::statusChanged);

        const QUrl playlistUrl(QStringLiteral("http://127.0.0.1:%1/encrypted.m3u8").arg(server.serverPort()));
        const QString downloadId = QStringLiteral("test-hls-enc");

        QVERIFY(downloader.start(downloadId, playlistUrl, targetPath, {}, {}, 2));
        QVERIFY(waitForHlsStatus(statusSpy, downloadId, qtidm::DownloadStatus::Completed, 10000));

        QFile target(targetPath);
        QVERIFY(target.exists());
        QVERIFY(target.open(QIODevice::ReadOnly));
        const QByteArray content = target.readAll();

        const QByteArray expected = server.chunk0 + server.chunk1;
        QCOMPARE(content, expected);
    }

    void pausesAndResumesHlsStream()
    {
        HlsMockHttpServer server;
        QVERIFY(server.isListening());

        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString targetPath = tempDir.filePath(QStringLiteral("output_resume.ts"));

        qtidm::HlsDownloader downloader;
        QSignalSpy statusSpy(&downloader, &qtidm::HlsDownloader::statusChanged);

        const QUrl masterUrl(QStringLiteral("http://127.0.0.1:%1/master.m3u8").arg(server.serverPort()));
        const QString downloadId = QStringLiteral("test-hls-pause");

        QVERIFY(downloader.start(downloadId, masterUrl, targetPath, {}, {}, 1));
        downloader.pause();
        QVERIFY(downloader.isPaused());

        downloader.resume();
        QVERIFY(!downloader.isPaused());

        QVERIFY(waitForHlsStatus(statusSpy, downloadId, qtidm::DownloadStatus::Completed, 10000));

        QFile target(targetPath);
        QVERIFY(target.exists());
        QVERIFY(target.open(QIODevice::ReadOnly));
        const QByteArray content = target.readAll();
        const QByteArray expected = server.chunk0 + server.chunk1 + server.chunk2 + server.chunk3;
        QCOMPARE(content, expected);
    }

    void downloadsWithKeyRotation()
    {
        HlsMockHttpServer server;
        QVERIFY(server.isListening());

        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString targetPath = tempDir.filePath(QStringLiteral("output_multikey.ts"));

        qtidm::HlsDownloader downloader;
        QSignalSpy statusSpy(&downloader, &qtidm::HlsDownloader::statusChanged);

        const QUrl playlistUrl(QStringLiteral("http://127.0.0.1:%1/multikey.m3u8").arg(server.serverPort()));
        const QString downloadId = QStringLiteral("test-hls-multikey");

        QVERIFY(downloader.start(downloadId, playlistUrl, targetPath, {}, {}, 2));
        QVERIFY(waitForHlsStatus(statusSpy, downloadId, qtidm::DownloadStatus::Completed, 10000));

        QFile target(targetPath);
        QVERIFY(target.exists());
        QVERIFY(target.open(QIODevice::ReadOnly));
        const QByteArray content = target.readAll();

        const QByteArray expected = server.chunk0 + server.chunk1;
        QCOMPARE(content, expected);
    }

    void downloadsMasterWithSeparateAudioRenditions()
    {
        HlsMockHttpServer server;
        QVERIFY(server.isListening());

        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString targetPath = tempDir.filePath(QStringLiteral("output_dual.ts"));

        qtidm::HlsDownloader downloader;
        QSignalSpy statusSpy(&downloader, &qtidm::HlsDownloader::statusChanged);

        const QUrl masterUrl(QStringLiteral("http://127.0.0.1:%1/dual_master.m3u8").arg(server.serverPort()));
        const QString downloadId = QStringLiteral("test-hls-dual");

        QVERIFY(downloader.start(downloadId, masterUrl, targetPath, {}, {}, 3));
        QVERIFY(waitForHlsStatus(statusSpy, downloadId, qtidm::DownloadStatus::Completed, 10000));

        QFile target(targetPath);
        QVERIFY(target.exists());
        QVERIFY(target.size() > 0);
    }

    void emitsSegmentsChangedSignals()
    {
        HlsMockHttpServer server;
        QVERIFY(server.isListening());

        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString targetPath = tempDir.filePath(QStringLiteral("output_segments.ts"));

        qtidm::HlsDownloader downloader;
        QSignalSpy segmentsSpy(&downloader, &qtidm::HlsDownloader::segmentsChanged);
        QSignalSpy statusSpy(&downloader, &qtidm::HlsDownloader::statusChanged);

        const QUrl masterUrl(QStringLiteral("http://127.0.0.1:%1/master.m3u8").arg(server.serverPort()));
        const QString downloadId = QStringLiteral("test-hls-seg");

        QVERIFY(downloader.start(downloadId, masterUrl, targetPath, {}, {}, 2));
        QVERIFY(waitForHlsStatus(statusSpy, downloadId, qtidm::DownloadStatus::Completed, 10000));

        QVERIFY(!segmentsSpy.isEmpty());
        // Last emitted segments list should reflect all segments as Completed
        const auto lastArgs = segmentsSpy.last();
        QCOMPARE(lastArgs.at(0).toString(), downloadId);
        const auto segments = lastArgs.at(1).value<QVector<qtidm::SegmentInfo>>();
        QCOMPARE(segments.size(), 4);
        for (const auto& seg : segments) {
            QCOMPARE(seg.status, qtidm::DownloadStatus::Completed);
        }
    }
};

QTEST_MAIN(HlsDownloaderTest)
#include "HlsDownloaderTest.moc"
