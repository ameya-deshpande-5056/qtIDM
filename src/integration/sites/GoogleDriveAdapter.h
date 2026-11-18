#pragma once

#include "integration/sites/SiteAdapter.h"

namespace qtidm {

class GoogleDriveAdapter final : public SiteAdapter {
public:
    QString name() const override;
    bool matchesUrl(const QUrl& url) const override;
    QUrl inferRefreshTarget(const QUrl& downloadUrl) const override;
    bool indicatesExpired(const QUrl& url,
                          long httpStatus,
                          const QString& contentType,
                          const QUrl& redirectLocation,
                          const QByteArray& bodyPreview) const override;
    SiteRecoveryResult recover(const QUrl& failedUrl,
                               const QUrl& refreshTarget,
                               const QString& token,
                               const QVariantMap& requestHeaders,
                               const SiteHttpFetcher& fetcher) const override;

    static bool isGoogleDriveHost(const QUrl& url);
    static QString extractFileId(const QUrl& url);
    static QUrl extractConfirmDownloadUrl(const QString& html, const QUrl& pageUrl, const QString& fallbackFileId);
};

} // namespace qtidm
