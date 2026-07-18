#include "core/SiteGrabber.h"

#include <QFile>
#include <QFileDevice>
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

    void savesRenderedDomAndDiscoversDynamicLinks()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        QFile renderer(dir.path() + QStringLiteral("/fake-renderer.sh"));
        QVERIFY(renderer.open(QIODevice::WriteOnly));
        renderer.write("#!/bin/sh\n"
                       "printf '%s' '<html><body><a href=\"dynamic.html\">dynamic</a>"
                       "<img src=\"asset.bin\" srcset=\"small.png 1x, large.png 2x\"></body></html>'\n");
        renderer.close();
        QVERIFY(renderer.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));

        QFile index(dir.path() + QStringLiteral("/index.html"));
        QVERIFY(index.open(QIODevice::WriteOnly));
        index.write("<html><body><script>/* links are inserted at runtime */</script></body></html>");
        index.close();

        qtidm::SiteGrabberOptions options;
        options.depth = 1;
        options.renderJavaScript = true;
        options.rendererExecutable = renderer.fileName();
        options.renderTimeoutMs = 2000;
        const auto result = qtidm::SiteGrabber().grab(
            QUrl::fromLocalFile(index.fileName()), dir.path() + QStringLiteral("/out"), options);

        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QVERIFY(result.warnings.isEmpty());
        QCOMPARE(result.renderedDocuments.size(), 2);
        QCOMPARE(result.renderedDocuments[0].url, QUrl::fromLocalFile(index.fileName()));
        QVERIFY(result.renderedDocuments[0].html.contains("dynamic.html"));

        QSet<QString> names;
        for (const auto& request : result.requests) {
            names.insert(QFileInfo(request.targetPath).fileName());
        }
        QCOMPARE(names, QSet<QString>({ QStringLiteral("asset.bin"), QStringLiteral("small.png"), QStringLiteral("large.png") }));
    }

    void renderingFailureFallsBackToStaticGrab()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        QFile index(dir.path() + QStringLiteral("/index.html"));
        QVERIFY(index.open(QIODevice::WriteOnly));
        index.write("<a href=\"static.bin\">static</a>");
        index.close();

        qtidm::SiteGrabberOptions options;
        options.depth = 1;
        options.renderJavaScript = true;
        options.rendererExecutable = dir.path() + QStringLiteral("/missing-renderer");
        const auto result = qtidm::SiteGrabber().grab(
            QUrl::fromLocalFile(index.fileName()), dir.path() + QStringLiteral("/out"), options);

        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QCOMPARE(result.warnings.size(), 1);
        QVERIFY(result.renderedDocuments.isEmpty());
        QCOMPARE(result.requests.size(), 2);
    }
};

QTEST_MAIN(SiteGrabberTest)
#include "SiteGrabberTest.moc"
