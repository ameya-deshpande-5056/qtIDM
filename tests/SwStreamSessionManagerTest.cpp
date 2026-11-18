#include "integration/SwStreamSessionManager.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest/QtTest>

using qtidm::SwStreamSessionManager;

class SwStreamSessionManagerTest final : public QObject {
    Q_OBJECT

private slots:
    void sanitizeFilenameStripsTraversalAndControlChars()
    {
        QCOMPARE(SwStreamSessionManager::sanitizeFilename(QStringLiteral("../../etc/passwd")), QStringLiteral("passwd"));
        QCOMPARE(SwStreamSessionManager::sanitizeFilename(QStringLiteral("C:\\Users\\test\\..\\report:final*.pdf")),
            QStringLiteral("report_final_.pdf"));
        QCOMPARE(SwStreamSessionManager::sanitizeFilename(QStringLiteral("...hidden")), QStringLiteral("hidden"));
        QCOMPARE(SwStreamSessionManager::sanitizeFilename(QStringLiteral("   ")), QStringLiteral("sw_download.bin"));
    }

    void streamsKnownSizeMultiChunkFileCleanly()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        SwStreamSessionManager manager(tempDir.path());

        QByteArray payload;
        payload.reserve(100000);
        for (int i = 0; i < 100000; ++i) {
            payload.append(static_cast<char>((i * 31 + 17) & 0xFF));
        }

        const auto begin = manager.handleBegin({
            { QStringLiteral("streamId"), QStringLiteral("sw-known-1") },
            { QStringLiteral("url"), QStringLiteral("https://mega.example.test/stream/abc") },
            { QStringLiteral("filename"), QStringLiteral("DecryptedArchive.bin") },
            { QStringLiteral("expectedSize"), static_cast<double>(payload.size()) }
        });
        QVERIFY(begin.value(QStringLiteral("ok")).toBool());
        const QString expectedPath = begin.value(QStringLiteral("path")).toString();
        QVERIFY(expectedPath.startsWith(tempDir.path()));
        QCOMPARE(manager.activeSessionCount(), 1);

        constexpr int kSlice = 32768;
        int seq = 0;
        for (int offset = 0; offset < payload.size(); offset += kSlice) {
            const QByteArray slice = payload.mid(offset, kSlice);
            const auto reply = manager.handleChunk({
                { QStringLiteral("streamId"), QStringLiteral("sw-known-1") },
                { QStringLiteral("sequence"), seq++ },
                { QStringLiteral("offset"), offset },
                { QStringLiteral("data"), QString::fromLatin1(slice.toBase64()) }
            });
            QVERIFY(reply.value(QStringLiteral("ok")).toBool());
            QCOMPARE(static_cast<int>(reply.value(QStringLiteral("bytesWritten")).toDouble()), offset + slice.size());
        }

        const auto finish = manager.handleFinish({
            { QStringLiteral("streamId"), QStringLiteral("sw-known-1") },
            { QStringLiteral("totalBytes"), static_cast<double>(payload.size()) }
        });
        QVERIFY(finish.value(QStringLiteral("ok")).toBool());
        QCOMPARE(manager.activeSessionCount(), 0);
        QVERIFY(!QFileInfo::exists(expectedPath + QStringLiteral(".qtidm-part")));

        QFile out(expectedPath);
        QVERIFY(out.open(QIODevice::ReadOnly));
        QCOMPARE(out.readAll(), payload);
    }

    void streamsUnknownLengthAndResolvesCollisions()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        // Pre-create collision file
        QFile existing(tempDir.filePath(QStringLiteral("Export.zip")));
        QVERIFY(existing.open(QIODevice::WriteOnly));
        existing.write("older");
        existing.close();

        SwStreamSessionManager manager(tempDir.path());
        const auto begin = manager.handleBegin({
            { QStringLiteral("streamId"), QStringLiteral("sw-unknown-1") },
            { QStringLiteral("url"), QStringLiteral("https://telegram.example.test/k/stream/1") },
            { QStringLiteral("filename"), QStringLiteral("Export.zip") },
            { QStringLiteral("expectedSize"), -1 }
        });
        QVERIFY(begin.value(QStringLiteral("ok")).toBool());
        const QString finalPath = begin.value(QStringLiteral("path")).toString();
        QCOMPARE(QFileInfo(finalPath).fileName(), QStringLiteral("Export (1).zip"));

        const QByteArray part1("Hello, ");
        const QByteArray part2("ServiceWorker Stream!");
        QVERIFY(manager.handleChunk({
            { QStringLiteral("streamId"), QStringLiteral("sw-unknown-1") },
            { QStringLiteral("sequence"), 0 },
            { QStringLiteral("offset"), 0 },
            { QStringLiteral("data"), QString::fromLatin1(part1.toBase64()) }
        }).value(QStringLiteral("ok")).toBool());
        QVERIFY(manager.handleChunk({
            { QStringLiteral("streamId"), QStringLiteral("sw-unknown-1") },
            { QStringLiteral("sequence"), 1 },
            { QStringLiteral("offset"), part1.size() },
            { QStringLiteral("data"), QString::fromLatin1(part2.toBase64()) }
        }).value(QStringLiteral("ok")).toBool());

        const auto finish = manager.handleFinish({
            { QStringLiteral("streamId"), QStringLiteral("sw-unknown-1") },
            { QStringLiteral("totalBytes"), part1.size() + part2.size() }
        });
        QVERIFY(finish.value(QStringLiteral("ok")).toBool());

        QFile out(finalPath);
        QVERIFY(out.open(QIODevice::ReadOnly));
        QCOMPARE(out.readAll(), part1 + part2);
    }

    void rejectsOutOfOrderInvalidBase64AndCleansUpOnAbort()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        SwStreamSessionManager manager(tempDir.path());

        // Reject SSRF & invalid stream IDs
        QVERIFY(!manager.handleBegin({
            { QStringLiteral("streamId"), QStringLiteral("bad/id") },
            { QStringLiteral("url"), QStringLiteral("https://example.test/s") }
        }).value(QStringLiteral("ok")).toBool());
        QVERIFY(!manager.handleBegin({
            { QStringLiteral("streamId"), QStringLiteral("sw-ssrf") },
            { QStringLiteral("url"), QStringLiteral("http://169.254.169.254/latest") }
        }).value(QStringLiteral("ok")).toBool());

        // Out-of-order chunk must fail and remove part file
        const auto b1 = manager.handleBegin({
            { QStringLiteral("streamId"), QStringLiteral("sw-ooo") },
            { QStringLiteral("url"), QStringLiteral("https://example.test/s1") },
            { QStringLiteral("filename"), QStringLiteral("ooo.bin") }
        });
        QVERIFY(b1.value(QStringLiteral("ok")).toBool());
        const QString oooPath = b1.value(QStringLiteral("path")).toString();
        const auto badSeq = manager.handleChunk({
            { QStringLiteral("streamId"), QStringLiteral("sw-ooo") },
            { QStringLiteral("sequence"), 1 },
            { QStringLiteral("offset"), 0 },
            { QStringLiteral("data"), QString::fromLatin1(QByteArray("abc").toBase64()) }
        });
        QVERIFY(!badSeq.value(QStringLiteral("ok")).toBool());
        QCOMPARE(badSeq.value(QStringLiteral("code")).toString(), QStringLiteral("out-of-order-chunk"));
        QCOMPARE(manager.activeSessionCount(), 0);
        QVERIFY(!QFileInfo::exists(oooPath + QStringLiteral(".qtidm-part")));

        // Invalid base64 must fail and clean up
        const auto b2 = manager.handleBegin({
            { QStringLiteral("streamId"), QStringLiteral("sw-b64") },
            { QStringLiteral("url"), QStringLiteral("https://example.test/s2") },
            { QStringLiteral("filename"), QStringLiteral("b64.bin") }
        });
        QVERIFY(b2.value(QStringLiteral("ok")).toBool());
        const auto badB64 = manager.handleChunk({
            { QStringLiteral("streamId"), QStringLiteral("sw-b64") },
            { QStringLiteral("sequence"), 0 },
            { QStringLiteral("offset"), 0 },
            { QStringLiteral("data"), QStringLiteral("!!!not-valid-base64!!!") }
        });
        QVERIFY(!badB64.value(QStringLiteral("ok")).toBool());
        QCOMPARE(badB64.value(QStringLiteral("code")).toString(), QStringLiteral("invalid-chunk-data"));
        QCOMPARE(manager.activeSessionCount(), 0);

        // Explicit abort removes part file cleanly
        const auto b3 = manager.handleBegin({
            { QStringLiteral("streamId"), QStringLiteral("sw-abort") },
            { QStringLiteral("url"), QStringLiteral("https://example.test/s3") },
            { QStringLiteral("filename"), QStringLiteral("aborted.bin") }
        });
        QVERIFY(b3.value(QStringLiteral("ok")).toBool());
        const QString abortPath = b3.value(QStringLiteral("path")).toString();
        QVERIFY(QFileInfo::exists(abortPath + QStringLiteral(".qtidm-part")));
        QVERIFY(manager.handleAbort({ { QStringLiteral("streamId"), QStringLiteral("sw-abort") } }).value(QStringLiteral("ok")).toBool());
        QVERIFY(!QFileInfo::exists(abortPath + QStringLiteral(".qtidm-part")));
        QCOMPARE(manager.activeSessionCount(), 0);
    }

    void enforcesConcurrencyLimitAndDestructorCleanup()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        QString firstPartPath;
        {
            SwStreamSessionManager manager(tempDir.path());
            for (int i = 0; i < 4; ++i) {
                const auto res = manager.handleBegin({
                    { QStringLiteral("streamId"), QStringLiteral("sw-c-%1").arg(i) },
                    { QStringLiteral("url"), QStringLiteral("https://example.test/c/%1").arg(i) },
                    { QStringLiteral("filename"), QStringLiteral("file_%1.bin").arg(i) }
                });
                QVERIFY(res.value(QStringLiteral("ok")).toBool());
                if (i == 0) firstPartPath = res.value(QStringLiteral("path")).toString() + QStringLiteral(".qtidm-part");
            }
            const auto overflow = manager.handleBegin({
                { QStringLiteral("streamId"), QStringLiteral("sw-c-overflow") },
                { QStringLiteral("url"), QStringLiteral("https://example.test/c/5") },
                { QStringLiteral("filename"), QStringLiteral("overflow.bin") }
            });
            QVERIFY(!overflow.value(QStringLiteral("ok")).toBool());
            QCOMPARE(overflow.value(QStringLiteral("code")).toString(), QStringLiteral("too-many-streams"));
            QVERIFY(QFileInfo::exists(firstPartPath));
        }
        // Destructor must remove all incomplete .qtidm-part files
        QVERIFY(!QFileInfo::exists(firstPartPath));
    }
};

QTEST_MAIN(SwStreamSessionManagerTest)
#include "SwStreamSessionManagerTest.moc"
