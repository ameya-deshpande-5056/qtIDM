#pragma once

#include "core/DownloadTypes.h"

#include <QList>
#include <QSet>
#include <QStringList>
#include <QUrl>

namespace qtidm {

struct SiteGrabberOptions {
    int depth { 1 };
    bool renderJavaScript { false };
    QString rendererExecutable;
    int renderTimeoutMs { 30000 };
    int virtualTimeBudgetMs { 3000 };
};

struct RenderedSiteDocument {
    QUrl url;
    QString targetPath;
    QByteArray html;
};

struct SiteGrabberResult {
    QList<DownloadRequest> requests;
    QList<RenderedSiteDocument> renderedDocuments;
    QString error;
    QStringList warnings;
};

class SiteGrabber final {
public:
    QList<DownloadRequest> grab(const QUrl& root, const QString& targetDir, int depth, QString* error = nullptr);
    SiteGrabberResult grab(const QUrl& root, const QString& targetDir, const SiteGrabberOptions& options);

private:
    QByteArray fetch(const QUrl& url, QString* error) const;
    QByteArray render(const QUrl& url, const SiteGrabberOptions& options, QString* error) const;
    void crawl(const QUrl& url, const QUrl& origin, const QString& targetDir, int depth,
        const SiteGrabberOptions& options, SiteGrabberResult* result);

    QSet<QString> visited_;
};

}
