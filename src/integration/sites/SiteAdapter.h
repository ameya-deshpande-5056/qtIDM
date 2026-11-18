#pragma once

#include <QByteArray>
#include <QString>
#include <QUrl>
#include <QVariantMap>
#include <functional>

namespace qtidm {

struct SiteHttpResponse {
    long statusCode = 0;
    QUrl finalUrl;
    QString contentType;
    QString locationHeader;
    QByteArray body;
};

using SiteHttpFetcher = std::function<SiteHttpResponse(const QUrl& url,
                                                       const QVariantMap& headers,
                                                       const QString& method,
                                                       const QByteArray& body)>;

struct SiteRecoveryResult {
    bool recovered = false;
    QUrl refreshedUrl;
    QUrl updatedRefreshTarget;
    QString updatedToken;
    QVariantMap extraHeaders;
    QString suggestedFilename;
    QString error;
};

class SiteAdapter {
public:
    virtual ~SiteAdapter() = default;

    virtual QString name() const = 0;
    virtual bool matchesUrl(const QUrl& url) const = 0;
    virtual QUrl inferRefreshTarget(const QUrl& downloadUrl) const = 0;
    virtual bool indicatesExpired(const QUrl& url,
                                  long httpStatus,
                                  const QString& contentType,
                                  const QUrl& redirectLocation,
                                  const QByteArray& bodyPreview) const = 0;
    virtual SiteRecoveryResult recover(const QUrl& failedUrl,
                                       const QUrl& refreshTarget,
                                       const QString& token,
                                       const QVariantMap& requestHeaders,
                                       const SiteHttpFetcher& fetcher) const = 0;
};

} // namespace qtidm
