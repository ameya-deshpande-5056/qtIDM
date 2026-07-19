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
};

QTEST_GUILESS_MAIN(SingleInstanceTest)
#include "SingleInstanceTest.moc"
