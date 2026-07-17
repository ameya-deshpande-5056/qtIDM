#include "core/SiteGrabber.h"

#include <QFile>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest/QtTest>

class SiteGrabberTest final : public QObject {
    Q_OBJECT
private slots:
    void crawlsFileLinksWithinDepth()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        QFile index(dir.path() + QStringLiteral("/index.html"));
        QVERIFY(index.open(QIODevice::WriteOnly));
        index.write("<a href=\"child.html\">child</a><img src=\"image.bin\">");
        index.close();

        QFile child(dir.path() + QStringLiteral("/child.html"));
        QVERIFY(child.open(QIODevice::WriteOnly));
        child.write("<a href=\"grandchild.html\">grandchild</a>");
        child.close();

        QFile image(dir.path() + QStringLiteral("/image.bin"));
        QVERIFY(image.open(QIODevice::WriteOnly));
        image.write("img");
        image.close();

        QString error;
        const auto requests = qtidm::SiteGrabber().grab(QUrl::fromLocalFile(index.fileName()), dir.path() + QStringLiteral("/out"), 1, &error);

        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(requests.size(), 3);
        QCOMPARE(requests[0].category, QStringLiteral("Site"));
        QCOMPARE(requests[0].url, QUrl::fromLocalFile(index.fileName()));
    }

    void reportsFetchErrors()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        QString error;
        const auto requests = qtidm::SiteGrabber().grab(QUrl(QStringLiteral("file:///definitely/missing/qtidm.html")), dir.path(), 1, &error);

        QCOMPARE(requests.size(), 1);
        QVERIFY(!error.isEmpty());
    }
};

QTEST_MAIN(SiteGrabberTest)
#include "SiteGrabberTest.moc"
