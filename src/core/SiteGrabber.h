#pragma once

#include "core/DownloadTypes.h"

#include <QList>
#include <QSet>
#include <QUrl>

namespace qtidm {

class SiteGrabber final {
public:
    QList<DownloadRequest> grab(const QUrl& root, const QString& targetDir, int depth, QString* error = nullptr);

private:
    QByteArray fetch(const QUrl& url, QString* error) const;
    void crawl(const QUrl& url, const QString& targetDir, int depth, QList<DownloadRequest>* output, QString* error);

    QSet<QString> visited_;
};

}
