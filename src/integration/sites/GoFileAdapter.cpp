#include "integration/sites/GoFileAdapter.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace qtidm {

namespace {
bool isValidContentId(const QString& id)
{
    if (id.size() < 4 || id.size() > 64) {
        return false;
    }
    for (const QChar ch : id) {
        if (!ch.isLetterOrNumber() && ch != QLatin1Char('-') && ch != QLatin1Char('_')) {
            return false;
        }
    }
    return true;
}
} // namespace

QString GoFileAdapter::name() const
{
    return QStringLiteral("gofile");
}

bool GoFileAdapter::isGoFileHost(const QUrl& url)
{
    const auto scheme = url.scheme().toLower();
    if (scheme != QStringLiteral("http") && scheme != QStringLiteral("https")) {
        return false;
    }
    const auto host = url.host().toLower();
    return host == QStringLiteral("gofile.io") || host.endsWith(QStringLiteral(".gofile.io"));
}

QString GoFileAdapter::extractContentId(const QUrl& url)
{
    const auto parts = url.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.size() >= 2 && (parts[0] == QStringLiteral("d") || parts[0] == QStringLiteral("contents"))
        && isValidContentId(parts[1])) {
        return parts[1];
    }
    return {};
}

bool GoFileAdapter::matchesUrl(const QUrl& url) const
{
    return isGoFileHost(url);
}

QUrl GoFileAdapter::inferRefreshTarget(const QUrl& downloadUrl) const
{
    if (!isGoFileHost(downloadUrl)) {
        return {};
    }
    const QString contentId = extractContentId(downloadUrl);
    if (contentId.isEmpty()) {
        return {};
    }
    QUrl target = downloadUrl;
    target.setHost(QStringLiteral("gofile.io"));
    target.setPath(QStringLiteral("/d/") + contentId);
    target.setQuery(QString {});
    target.setFragment(QString {});
    return target;
}

bool GoFileAdapter::indicatesExpired(const QUrl& /*url*/,
                                     long httpStatus,
                                     const QString& contentType,
                                     const QUrl& redirectLocation,
                                     const QByteArray& bodyPreview) const
{
    if (httpStatus == 401 || httpStatus == 403 || httpStatus == 404 || httpStatus == 410) {
        return true;
    }
    if (redirectLocation.isValid() && !extractContentId(redirectLocation).isEmpty()) {
        return true;
    }
    if (contentType.toLower().contains(QStringLiteral("text/html"))) {
        return true;
    }
    const auto trimmed = bodyPreview.trimmed().toLower();
    return trimmed.startsWith("<!doctype html") || trimmed.startsWith("<html");
}

SiteRecoveryResult GoFileAdapter::recover(const QUrl& failedUrl,
                                          const QUrl& refreshTarget,
                                          const QString& token,
                                          const QVariantMap& requestHeaders,
                                          const SiteHttpFetcher& fetcher) const
{
    SiteRecoveryResult result;
    if (!fetcher) {
        result.error = QStringLiteral("no HTTP fetcher provided");
        return result;
    }

    QString contentId = extractContentId(refreshTarget);
    if (contentId.isEmpty()) {
        contentId = extractContentId(failedUrl);
    }
    if (contentId.isEmpty()) {
        result.error = QStringLiteral("GoFile content ID not found in refreshTarget");
        return result;
    }

    QString effectiveToken = token.trimmed();
    QString apiBase = QStringLiteral("https://api.gofile.io");
    const auto tokenDoc = QJsonDocument::fromJson(effectiveToken.toUtf8());
    if (tokenDoc.isObject()) {
        const auto obj = tokenDoc.object();
        effectiveToken = obj.value(QStringLiteral("token")).toString().trimmed();
        const auto customApi = obj.value(QStringLiteral("apiBase")).toString().trimmed();
        if (!customApi.isEmpty()) {
            apiBase = customApi;
        }
    }
    while (apiBase.endsWith(QLatin1Char('/'))) {
        apiBase.chop(1);
    }

    // 1. Obtain a guest account token if none was supplied
    if (effectiveToken.isEmpty()) {
        const SiteHttpResponse acctResp = fetcher(QUrl(apiBase + QStringLiteral("/accounts")), {}, QStringLiteral("POST"), {});
        if (acctResp.statusCode >= 200 && acctResp.statusCode < 300) {
            const auto root = QJsonDocument::fromJson(acctResp.body).object();
            effectiveToken = root.value(QStringLiteral("data")).toObject().value(QStringLiteral("token")).toString().trimmed();
        }
    }
    if (effectiveToken.isEmpty()) {
        result.error = QStringLiteral("could not obtain GoFile API token");
        return result;
    }

    // 2. Re-query GoFile contents API for fresh CDN download link
    QVariantMap headers = requestHeaders;
    headers.remove(QStringLiteral("Range"));
    headers.remove(QStringLiteral("If-Range"));
    headers.insert(QStringLiteral("Authorization"), QStringLiteral("Bearer ") + effectiveToken);
    const QUrl contentsUrl(apiBase + QStringLiteral("/contents/") + contentId);
    const SiteHttpResponse contentsResp = fetcher(contentsUrl, headers, QStringLiteral("GET"), {});
    if (contentsResp.statusCode < 200 || contentsResp.statusCode >= 300) {
        result.error = QStringLiteral("GoFile API returned HTTP %1").arg(contentsResp.statusCode);
        return result;
    }

    const auto root = QJsonDocument::fromJson(contentsResp.body).object();
    if (root.value(QStringLiteral("status")).toString() != QStringLiteral("ok")) {
        result.error = QStringLiteral("GoFile API status was not ok");
        return result;
    }

    const auto dataObj = root.value(QStringLiteral("data")).toObject();
    const auto childrenVal = dataObj.value(QStringLiteral("children"));
    QList<QJsonObject> candidates;
    if (childrenVal.isObject()) {
        const auto obj = childrenVal.toObject();
        for (auto it = obj.begin(); it != obj.end(); ++it) {
            if (it.value().isObject()) {
                candidates.append(it.value().toObject());
            }
        }
    } else if (childrenVal.isArray()) {
        for (const auto& item : childrenVal.toArray()) {
            if (item.isObject()) {
                candidates.append(item.toObject());
            }
        }
    }

    const QString desiredLeaf = QFileInfo(failedUrl.path()).fileName();
    QJsonObject chosen;
    for (const auto& child : candidates) {
        const QString link = child.value(QStringLiteral("link")).toString().trimmed();
        if (link.isEmpty()) {
            continue;
        }
        const QString childName = child.value(QStringLiteral("name")).toString().trimmed();
        if (!desiredLeaf.isEmpty() && childName.compare(desiredLeaf, Qt::CaseInsensitive) == 0) {
            chosen = child;
            break;
        }
        if (chosen.isEmpty()) {
            chosen = child;
        }
    }

    const QUrl freshLink(chosen.value(QStringLiteral("link")).toString().trimmed());
    if (!freshLink.isValid() || (freshLink.scheme() != QStringLiteral("http") && freshLink.scheme() != QStringLiteral("https"))) {
        result.error = QStringLiteral("GoFile API response did not contain a downloadable file link");
        return result;
    }

    result.recovered = true;
    result.refreshedUrl = freshLink;
    result.updatedRefreshTarget = refreshTarget.isValid() ? refreshTarget : inferRefreshTarget(failedUrl);
    result.updatedToken = tokenDoc.isObject() ? token : effectiveToken;
    result.suggestedFilename = chosen.value(QStringLiteral("name")).toString().trimmed();
    result.extraHeaders.insert(QStringLiteral("Cookie"), QStringLiteral("accountToken=") + effectiveToken);
    return result;
}

} // namespace qtidm
