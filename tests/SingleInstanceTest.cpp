#include "app/SingleInstance.h"

#include <QTest>

using namespace qtidm;

class SingleInstanceTest final : public QObject {
    Q_OBJECT

private slots:
    void decodesStructuredDownloads()
    {
        SingleInstance instance;
        QVariantList received;
        instance.setDownloadsHandler([&received](QVariantList downloads) {
            received = std::move(downloads);
            return true;
        });

        const auto json = QStringLiteral(
            "[{\"url\":\"https://example.test/video.m3u8\","
            "\"headers\":{\"Referer\":\"https://example.test/page\","
            "\"_qtidmMediaType\":\"HLS\"},\"method\":\"\",\"body\":\"\","
            "\"suggestedFilename\":\"episode.m3u8\"}]");
        QVERIFY(instance.AddDownloadsJson(json));
        QCOMPARE(received.size(), 1);
        const auto download = received.constFirst().toMap();
        QCOMPARE(download.value(QStringLiteral("url")).toString(),
                 QStringLiteral("https://example.test/video.m3u8"));
        QCOMPARE(download.value(QStringLiteral("headers")).toMap()
                     .value(QStringLiteral("_qtidmMediaType")).toString(),
                 QStringLiteral("HLS"));
    }

    void rejectsInvalidJson()
    {
        SingleInstance instance;
        instance.setDownloadsHandler([](QVariantList) { return true; });
        QVERIFY(!instance.AddDownloadsJson(QStringLiteral("{}")));
        QVERIFY(!instance.AddDownloadsJson(QStringLiteral("not json")));
    }

    void dispatchesSocialMediaExtraction()
    {
        SingleInstance instance;
        QString receivedUrl;
        instance.setSocialMediaHandler([&receivedUrl](QString url) {
            receivedUrl = url;
            return true;
        });
        QVERIFY(instance.ExtractSocialMedia(QStringLiteral("https://www.youtube.com/watch?v=example")));
        QCOMPARE(receivedUrl, QStringLiteral("https://www.youtube.com/watch?v=example"));
    }

    void reportsUnavailableAutomation()
    {
        SingleInstance instance;
        const auto answer = instance.ListDownloads();
        QVERIFY(answer.contains(QStringLiteral("\"error\"")));
        QVERIFY(answer.contains(QStringLiteral("unavailable")));
        QVERIFY(instance.PauseDownloads({}).contains(QStringLiteral("unavailable")));
    }

    void dispatchesAutomationVerbs()
    {
        SingleInstance instance;
        QString verb;
        QStringList arguments;
        instance.setAutomationHandler([&verb, &arguments](const QString& receivedVerb,
                                                          const QStringList& receivedArguments) {
            verb = receivedVerb;
            arguments = receivedArguments;
            return QStringLiteral("{\"ok\":true}");
        });

        QCOMPARE(instance.PauseDownloads({ QStringLiteral("a"), QStringLiteral("b") }),
                 QStringLiteral("{\"ok\":true}"));
        QCOMPARE(verb, QStringLiteral("pause"));
        QCOMPARE(arguments, QStringList({ QStringLiteral("a"), QStringLiteral("b") }));

        instance.StartQueue(QStringLiteral("Night"));
        QCOMPARE(verb, QStringLiteral("start-queue"));
        QCOMPARE(arguments, QStringList { QStringLiteral("Night") });

        instance.DeleteDownloads({ QStringLiteral("c") });
        QCOMPARE(verb, QStringLiteral("delete"));
    }
};

QTEST_GUILESS_MAIN(SingleInstanceTest)
#include "SingleInstanceTest.moc"
