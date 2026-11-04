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
    struct CommandInvocation {
        QString program;
        QStringList prefixArguments;
        bool isValid() const { return !program.isEmpty(); }
    };

    explicit SocialMediaExtractor(QString executable = {});

    static bool supports(const QUrl& url);
    static QUrl normalizeUrl(const QUrl& url);
    static CommandInvocation selectLatestCommand(const QList<CommandInvocation>& candidates);

    bool isAvailable() const;
    CommandInvocation resolveCommand() const;
    SocialMediaExtraction extract(const QUrl& url, const QString& browserCookies,
        QString* error = nullptr) const;

private:
    QString executable() const;

    QString executable_;
    mutable bool commandResolved_ = false;
    mutable CommandInvocation cachedCommand_;
};

}
