#include "integration/sites/GoogleDriveAdapter.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUrlQuery>

namespace qtidm {

namespace {
bool isValidDriveId(const QString& id)
{
    if (id.size() < 10 || id.size() > 128) {
        return false;
    }
    for (const QChar ch : id) {
        if (!ch.isLetterOrNumber() && ch != QLatin1Char('_') && ch != QLatin1Char('-')) {
            return false;
        }
    }
    return true;
}

QString decodeEntities(QString value)
{
    value.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
    value.replace(QStringLiteral("&#38;"), QStringLiteral("&"));
    value.replace(QStringLiteral("&quot;"), QStringLiteral("\""));
    value.replace(QStringLiteral("&#39;"), QStringLiteral("'"));
    return value;
}

QString readAttr(const QString& tag, const QString& attrName)
{
    const QRegularExpression re(
        QStringLiteral(R"re((?:^|\s)%1\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s>]+)))re").arg(QRegularExpression::escape(attrName)),
        QRegularExpression::CaseInsensitiveOption);
    const auto m = re.match(tag);
    if (!m.hasMatch()) {
        return {};
    }
    for (int g = 1; g <= 3; ++g) {
        if (!m.captured(g).isEmpty()) {
            return decodeEntities(m.captured(g).trimmed());
        }
    }
    return {};
}
} // namespace

QString GoogleDriveAdapter::name() const
{
    return QStringLiteral("google-drive");
}

bool GoogleDriveAdapter::isGoogleDriveHost(const QUrl& url)
{
    const auto scheme = url.scheme().toLower();
    if (scheme != QStringLiteral("http") && scheme != QStringLiteral("https")) {
        return false;
    }
    const auto host = url.host().toLower();
    if (host == QStringLiteral("drive.google.com")
        || host == QStringLiteral("drive.usercontent.google.com")
        || host == QStringLiteral("docs.google.com")) {
        return true;
    }
    if (host == QStringLiteral("www.googleapis.com")
        && url.path().startsWith(QStringLiteral("/drive/v3/files/"))) {
        return true;
    }
    return false;
}

QString GoogleDriveAdapter::extractFileId(const QUrl& url)
{
    const QUrlQuery query(url);
    const QString queryId = query.queryItemValue(QStringLiteral("id"));
    if (isValidDriveId(queryId)) {
        return queryId;
    }

    const auto parts = url.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
    // /file/d/<id>/... or /d/<id>/...
    for (int i = 0; i + 1 < parts.size(); ++i) {
        if (parts[i] == QStringLiteral("d") && isValidDriveId(parts[i + 1])) {
            return parts[i + 1];
        }
    }
    // /drive/v3/files/<id>
    if (parts.size() >= 4 && parts[0] == QStringLiteral("drive")
        && parts[1] == QStringLiteral("v3") && parts[2] == QStringLiteral("files")
        && isValidDriveId(parts[3])) {
        return parts[3];
    }
    return {};
}

QUrl GoogleDriveAdapter::extractConfirmDownloadUrl(const QString& html, const QUrl& pageUrl, const QString& fallbackFileId)
{
    // 1. Modern Google Drive virus-scan confirmation form: <form id="download-form" action="...">
    static const QRegularExpression formRe(
        QStringLiteral(R"(<form\b[^>]*id\s*=\s*["']download-form["'][^>]*>([\s\S]*?)</form>)"),
        QRegularExpression::CaseInsensitiveOption);
    const auto formMatch = formRe.match(html);
    if (formMatch.hasMatch()) {
        const QString fullForm = formMatch.captured(0);
        const int openEnd = fullForm.indexOf(QLatin1Char('>'));
        const QString openTag = openEnd > 0 ? fullForm.left(openEnd + 1) : fullForm;
        const QString action = readAttr(openTag, QStringLiteral("action"));
        if (!action.isEmpty()) {
            QUrl target = pageUrl.resolved(QUrl(action));
            QUrlQuery query(target);
            static const QRegularExpression inputRe(QStringLiteral(R"(<input\b[^>]*>)"), QRegularExpression::CaseInsensitiveOption);
            auto it = inputRe.globalMatch(formMatch.captured(1));
            while (it.hasNext()) {
                const QString inputTag = it.next().captured(0);
                const QString name = readAttr(inputTag, QStringLiteral("name"));
                const QString value = readAttr(inputTag, QStringLiteral("value"));
                if (!name.isEmpty()) {
                    query.removeAllQueryItems(name);
                    query.addQueryItem(name, value);
                }
            }
            if (!query.hasQueryItem(QStringLiteral("id")) && !fallbackFileId.isEmpty()) {
                query.addQueryItem(QStringLiteral("id"), fallbackFileId);
            }
            if (!query.hasQueryItem(QStringLiteral("confirm"))) {
                query.addQueryItem(QStringLiteral("confirm"), QStringLiteral("t"));
            }
            target.setQuery(query);
            if (target.isValid() && (target.scheme() == QStringLiteral("http") || target.scheme() == QStringLiteral("https"))) {
                return target;
            }
        }
    }

    // 2. Legacy confirmation link: href="...confirm=<token>..."
    static const QRegularExpression confirmLinkRe(
        QStringLiteral(R"(href\s*=\s*["']([^"']*confirm=[^"']+)["'])"),
        QRegularExpression::CaseInsensitiveOption);
    const auto linkMatch = confirmLinkRe.match(html);
    if (linkMatch.hasMatch()) {
        const QUrl resolved = pageUrl.resolved(QUrl(decodeEntities(linkMatch.captured(1))));
        if (resolved.isValid() && (resolved.scheme() == QStringLiteral("http") || resolved.scheme() == QStringLiteral("https"))) {
            return resolved;
        }
    }

    return {};
}

bool GoogleDriveAdapter::matchesUrl(const QUrl& url) const
{
    return isGoogleDriveHost(url);
}

QUrl GoogleDriveAdapter::inferRefreshTarget(const QUrl& downloadUrl) const
{
    if (!isGoogleDriveHost(downloadUrl)) {
        return {};
    }
    const QString fileId = extractFileId(downloadUrl);
    if (fileId.isEmpty()) {
        return {};
    }
    QUrl target = downloadUrl;
    target.setHost(QStringLiteral("drive.google.com"));
    target.setPath(QStringLiteral("/uc"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("export"), QStringLiteral("download"));
    query.addQueryItem(QStringLiteral("id"), fileId);
    target.setQuery(query);
    target.setFragment(QString {});
    return target;
}

bool GoogleDriveAdapter::indicatesExpired(const QUrl& /*url*/,
                                          long httpStatus,
                                          const QString& contentType,
                                          const QUrl& redirectLocation,
                                          const QByteArray& bodyPreview) const
{
    if (httpStatus == 401 || httpStatus == 403 || httpStatus == 410) {
        return true;
    }
    if (redirectLocation.isValid() && redirectLocation.host().toLower().contains(QStringLiteral("accounts.google.com"))) {
        return true;
    }
    if (contentType.toLower().contains(QStringLiteral("text/html"))) {
        return true;
    }
    const auto lowerBody = bodyPreview.toLower();
    if (lowerBody.contains("download-form") || lowerBody.contains("confirm=") || lowerBody.contains("uc-download-link")) {
        return true;
    }
    return false;
}

SiteRecoveryResult GoogleDriveAdapter::recover(const QUrl& failedUrl,
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

    QString fileId = extractFileId(failedUrl);
    if (fileId.isEmpty() && refreshTarget.isValid()) {
        fileId = extractFileId(refreshTarget);
    }

    // 1. OAuth 2.0 refresh token path (when token JSON contains refresh_token + client credentials)
    const QJsonObject tokenObj = QJsonDocument::fromJson(token.toUtf8()).object();
    const QString refreshToken = tokenObj.value(QStringLiteral("refresh_token")).toString().trimmed();
    const QString clientId = tokenObj.value(QStringLiteral("client_id")).toString().trimmed();
    const QString clientSecret = tokenObj.value(QStringLiteral("client_secret")).toString().trimmed();
    if (!refreshToken.isEmpty() && !clientId.isEmpty() && !fileId.isEmpty()) {
        const QUrl tokenEndpoint(tokenObj.value(QStringLiteral("token_uri"))
            .toString(QStringLiteral("https://oauth2.googleapis.com/token")));
        QUrlQuery form;
        form.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("refresh_token"));
        form.addQueryItem(QStringLiteral("client_id"), clientId);
        if (!clientSecret.isEmpty()) {
            form.addQueryItem(QStringLiteral("client_secret"), clientSecret);
        }
        form.addQueryItem(QStringLiteral("refresh_token"), refreshToken);

        QVariantMap tokenHeaders;
        tokenHeaders.insert(QStringLiteral("Content-Type"), QStringLiteral("application/x-www-form-urlencoded"));
        const SiteHttpResponse tokenResp = fetcher(tokenEndpoint, tokenHeaders, QStringLiteral("POST"),
            form.toString(QUrl::FullyEncoded).toUtf8());
        if (tokenResp.statusCode >= 200 && tokenResp.statusCode < 300) {
            const auto parsed = QJsonDocument::fromJson(tokenResp.body).object();
            const QString accessToken = parsed.value(QStringLiteral("access_token")).toString().trimmed();
            if (!accessToken.isEmpty()) {
                const QString apiBase = tokenObj.value(QStringLiteral("api_base"))
                    .toString(QStringLiteral("https://www.googleapis.com"));
                QUrl mediaUrl(apiBase + QStringLiteral("/drive/v3/files/") + fileId + QStringLiteral("?alt=media"));
                result.recovered = true;
                result.refreshedUrl = mediaUrl;
                result.updatedRefreshTarget = refreshTarget.isValid() ? refreshTarget : inferRefreshTarget(failedUrl);
                result.updatedToken = token;
                result.extraHeaders.insert(QStringLiteral("Authorization"), QStringLiteral("Bearer ") + accessToken);
                return result;
            }
        }
    }

    // 2. Virus-scan confirmation token extraction path
    QUrl originPage = refreshTarget.isValid() ? refreshTarget : inferRefreshTarget(failedUrl);
    if (!originPage.isValid()) {
        result.error = QStringLiteral("could not determine Google Drive origin page");
        return result;
    }

    QVariantMap headers = requestHeaders;
    headers.remove(QStringLiteral("Range"));
    headers.remove(QStringLiteral("If-Range"));
    const SiteHttpResponse resp = fetcher(originPage, headers, QStringLiteral("GET"), {});
    if (resp.statusCode < 200 || resp.statusCode >= 300 || resp.body.isEmpty()) {
        result.error = QStringLiteral("Google Drive confirmation page returned HTTP %1").arg(resp.statusCode);
        return result;
    }

    const QUrl base = resp.finalUrl.isValid() ? resp.finalUrl : originPage;
    const QUrl confirmed = extractConfirmDownloadUrl(QString::fromUtf8(resp.body), base, fileId);
    if (!confirmed.isValid()) {
        result.error = QStringLiteral("Google Drive virus-scan confirmation token not found");
        return result;
    }

    result.recovered = true;
    result.refreshedUrl = confirmed;
    result.updatedRefreshTarget = originPage;
    return result;
}

} // namespace qtidm
