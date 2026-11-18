#include "integration/sites/MediaFireAdapter.h"

#include <QRegularExpression>
#include <QUrlQuery>

namespace qtidm {

namespace {
bool isValidKey(const QString& key)
{
    if (key.size() < 6 || key.size() > 64) {
        return false;
    }
    for (const QChar ch : key) {
        if (!ch.isLetterOrNumber()) {
            return false;
        }
    }
    return true;
}

QString decodeHtmlEntities(QString value)
{
    value.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
    value.replace(QStringLiteral("&#38;"), QStringLiteral("&"));
    value.replace(QStringLiteral("&quot;"), QStringLiteral("\""));
    value.replace(QStringLiteral("&#39;"), QStringLiteral("'"));
    value.replace(QStringLiteral("&lt;"), QStringLiteral("<"));
    value.replace(QStringLiteral("&gt;"), QStringLiteral(">"));
    return value;
}

QString readHtmlAttribute(const QString& tag, const QString& attrName)
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
            return decodeHtmlEntities(m.captured(g).trimmed());
        }
    }
    return {};
}
} // namespace

QString MediaFireAdapter::name() const
{
    return QStringLiteral("mediafire");
}

bool MediaFireAdapter::isMediaFireHost(const QUrl& url)
{
    const auto scheme = url.scheme().toLower();
    if (scheme != QStringLiteral("http") && scheme != QStringLiteral("https")) {
        return false;
    }
    const auto host = url.host().toLower();
    return host == QStringLiteral("mediafire.com") || host.endsWith(QStringLiteral(".mediafire.com"));
}

bool MediaFireAdapter::isMediaFireFileHost(const QUrl& url)
{
    if (!isMediaFireHost(url)) {
        return false;
    }
    const auto host = url.host().toLower();
    if (!host.endsWith(QStringLiteral(".mediafire.com"))) {
        return false;
    }
    const auto sub = host.left(host.size() - QStringLiteral(".mediafire.com").size());
    if (!sub.startsWith(QStringLiteral("download"))) {
        return false;
    }
    const auto digits = sub.mid(8);
    if (digits.isEmpty()) {
        return false;
    }
    for (const QChar ch : digits) {
        if (!ch.isDigit()) {
            return false;
        }
    }
    return true;
}

QString MediaFireAdapter::extractFileKey(const QUrl& url)
{
    const QUrlQuery query(url);
    if (url.path().endsWith(QStringLiteral("/download_repair.php"), Qt::CaseInsensitive)) {
        const auto qkey = query.queryItemValue(QStringLiteral("qkey"));
        if (isValidKey(qkey)) {
            return qkey;
        }
    }

    const auto segments = url.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (segments.size() >= 2 && (segments[0] == QStringLiteral("file") || segments[0] == QStringLiteral("file_premium") || segments[0] == QStringLiteral("download"))) {
        if (isValidKey(segments[1])) {
            return segments[1];
        }
    }
    // Direct file host path: /<ticket>/<key>/<filename>
    if (isMediaFireFileHost(url) && segments.size() >= 3 && !segments[0].isEmpty()
        && isValidKey(segments[1]) && !segments[2].isEmpty()) {
        return segments[1];
    }
    return {};
}

QUrl MediaFireAdapter::extractDownloadButtonUrl(const QString& html, const QUrl& pageUrl)
{
    static const QRegularExpression anchorRe(QStringLiteral(R"(<a\b[^>]*>)"), QRegularExpression::CaseInsensitiveOption);
    auto it = anchorRe.globalMatch(html);
    while (it.hasNext()) {
        const QString tag = it.next().captured(0);
        const QString id = readHtmlAttribute(tag, QStringLiteral("id"));
        if (id != QStringLiteral("downloadButton")) {
            continue;
        }
        const QString scrambled = readHtmlAttribute(tag, QStringLiteral("data-scrambled-url"));
        if (!scrambled.isEmpty()) {
            const auto decoded = QString::fromUtf8(QByteArray::fromBase64(scrambled.toLatin1())).trimmed();
            const QUrl candidate = pageUrl.resolved(QUrl(decoded));
            if (candidate.isValid() && (candidate.scheme() == QStringLiteral("http") || candidate.scheme() == QStringLiteral("https"))) {
                return candidate;
            }
        }
        const QString href = readHtmlAttribute(tag, QStringLiteral("href"));
        if (!href.isEmpty()) {
            const QUrl candidate = pageUrl.resolved(QUrl(href));
            if (candidate.isValid() && (candidate.scheme() == QStringLiteral("http") || candidate.scheme() == QStringLiteral("https"))) {
                return candidate;
            }
        }
    }
    return {};
}

bool MediaFireAdapter::matchesUrl(const QUrl& url) const
{
    return isMediaFireHost(url);
}

QUrl MediaFireAdapter::inferRefreshTarget(const QUrl& downloadUrl) const
{
    if (!isMediaFireHost(downloadUrl)) {
        return {};
    }
    const QString key = extractFileKey(downloadUrl);
    if (key.isEmpty()) {
        return {};
    }
    QUrl page = downloadUrl;
    page.setHost(QStringLiteral("www.mediafire.com"));
    page.setPath(QStringLiteral("/file/") + key);
    page.setQuery(QString {});
    page.setFragment(QString {});
    return page;
}

bool MediaFireAdapter::indicatesExpired(const QUrl& url,
                                        long httpStatus,
                                        const QString& contentType,
                                        const QUrl& redirectLocation,
                                        const QByteArray& bodyPreview) const
{
    if (redirectLocation.isValid()
        && redirectLocation.path().endsWith(QStringLiteral("/download_repair.php"), Qt::CaseInsensitive)) {
        return true;
    }
    if (url.path().endsWith(QStringLiteral("/download_repair.php"), Qt::CaseInsensitive)) {
        return true;
    }
    if (httpStatus == 403 || httpStatus == 404 || httpStatus == 410) {
        return true;
    }
    const auto lowerType = contentType.toLower();
    if (lowerType.contains(QStringLiteral("text/html"))) {
        return true;
    }
    const auto trimmedBody = bodyPreview.trimmed().toLower();
    if (trimmedBody.startsWith("<!doctype html") || trimmedBody.startsWith("<html")) {
        return true;
    }
    return false;
}

SiteRecoveryResult MediaFireAdapter::recover(const QUrl& failedUrl,
                                             const QUrl& refreshTarget,
                                             const QString& /*token*/,
                                             const QVariantMap& requestHeaders,
                                             const SiteHttpFetcher& fetcher) const
{
    SiteRecoveryResult result;
    if (!fetcher) {
        result.error = QStringLiteral("no HTTP fetcher provided");
        return result;
    }

    QUrl originPage = refreshTarget;
    if (!originPage.isValid() || originPage.isEmpty()) {
        originPage = inferRefreshTarget(failedUrl);
    }
    if (!originPage.isValid() || originPage.isEmpty()) {
        result.error = QStringLiteral("could not determine MediaFire origin file page");
        return result;
    }

    // If originPage itself is a repair page, jump to the canonical file page first
    if (originPage.path().endsWith(QStringLiteral("/download_repair.php"), Qt::CaseInsensitive)) {
        const QString key = extractFileKey(originPage);
        if (!key.isEmpty()) {
            QUrl canonical = originPage;
            canonical.setPath(QStringLiteral("/file/") + key);
            canonical.setQuery(QString {});
            originPage = canonical;
        }
    }

    QVariantMap headers = requestHeaders;
    headers.remove(QStringLiteral("Range"));
    headers.remove(QStringLiteral("If-Range"));
    const SiteHttpResponse resp = fetcher(originPage, headers, QStringLiteral("GET"), {});
    if (resp.statusCode < 200 || resp.statusCode >= 300 || resp.body.isEmpty()) {
        result.error = QStringLiteral("MediaFire origin page returned HTTP %1").arg(resp.statusCode);
        return result;
    }

    const QUrl base = resp.finalUrl.isValid() ? resp.finalUrl : originPage;
    const QUrl buttonUrl = extractDownloadButtonUrl(QString::fromUtf8(resp.body), base);
    if (!buttonUrl.isValid()) {
        result.error = QStringLiteral("MediaFire downloadButton link not found on origin page");
        return result;
    }
    // Reject extracting a repair page or non-file URL when talking to real MediaFire hosts
    if (isMediaFireHost(buttonUrl) && !isMediaFireFileHost(buttonUrl)) {
        result.error = QStringLiteral("extracted MediaFire link is not a direct file host URL");
        return result;
    }

    result.recovered = true;
    result.refreshedUrl = buttonUrl;
    result.updatedRefreshTarget = originPage;
    return result;
}

} // namespace qtidm
