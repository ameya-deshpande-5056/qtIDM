#pragma once

#include <QList>
#include <QString>
#include <QUrl>
#include <QVariantMap>

namespace qtidm {

struct SocialMediaFormat {
    QString id;
    QString label;
    QString extension;
    QUrl url;
    QUrl audioUrl;
    QVariantMap headers;
    QVariantMap audioHeaders;
    bool adaptive = false;
    bool isCombined = false;
};

struct SocialMediaExtraction {
    QString title;
    QString extractor;
    QList<SocialMediaFormat> formats;
};

class SocialMediaExtractor final {
public:
    explicit SocialMediaExtractor(QString executable = {});

    static bool supports(const QUrl& url);
    bool isAvailable() const;
    SocialMediaExtraction extract(const QUrl& url, const QString& browserCookies,
        QString* error = nullptr) const;

private:
    QString executable() const;

    QString executable_;
};

}
