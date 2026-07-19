#include "core/SiteGrabber.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtGlobal>
#include <curl/curl.h>

namespace qtidm {

namespace {
size_t collect(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    auto* buffer = static_cast<QByteArray*>(userdata);
    buffer->append(ptr, static_cast<qsizetype>(size * nmemb));
    return size * nmemb;
}

QString safePath(const QUrl& url)
{
    auto path = url.path();
    if (path.isEmpty() || path.endsWith(QLatin1Char('/'))) {
        path += QStringLiteral("index.html");
    }
    if (path.startsWith(QLatin1Char('/'))) {
        path.remove(0, 1);
    }
    const auto parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QStringList safeParts;
    for (auto part : parts) {
        safeParts.append(part.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]")), QStringLiteral("_")));
    }
    return safeParts.isEmpty() ? QStringLiteral("index.html") : safeParts.join(QLatin1Char('/'));
}

QString normalizedKey(QUrl url)
{
    url.setFragment({});
    return url.adjusted(QUrl::NormalizePathSegments | QUrl::StripTrailingSlash).toString(QUrl::FullyEncoded);
}

int effectivePort(const QUrl& url)
{
    if (url.port() >= 0) {
        return url.port();
    }
    if (url.scheme() == QStringLiteral("https")) {
        return 443;
    }
    if (url.scheme() == QStringLiteral("http")) {
        return 80;
    }
    return -1;
}

bool hasSameOrigin(const QUrl& left, const QUrl& right)
{
    return left.scheme().compare(right.scheme(), Qt::CaseInsensitive) == 0
        && left.host().compare(right.host(), Qt::CaseInsensitive) == 0
        && effectivePort(left) == effectivePort(right);
}

bool canFetch(const QUrl& url)
{
    const auto scheme = url.scheme().toLower();
    return scheme == QStringLiteral("http") || scheme == QStringLiteral("https") || scheme == QStringLiteral("file");
}

bool isHtmlLike(const QUrl& url)
{
    auto path = url.path();
    if (path.isEmpty() || path.endsWith(QLatin1Char('/'))) {
        return true;
    }
    const auto suffix = QFileInfo(path).suffix().toLower();
    if (suffix.isEmpty()) {
        return true;
    }
    static const QSet<QString> documentSuffixes {
        QStringLiteral("asp"), QStringLiteral("aspx"), QStringLiteral("cfm"),
        QStringLiteral("cgi"), QStringLiteral("htm"), QStringLiteral("html"),
        QStringLiteral("jsp"), QStringLiteral("php"), QStringLiteral("shtml")
    };
    return documentSuffixes.contains(suffix);
}

QString findRenderer(const SiteGrabberOptions& options)
{
    if (!options.rendererExecutable.trimmed().isEmpty()) {
        return options.rendererExecutable;
    }
    const auto configured = qEnvironmentVariable("QTIDM_SITE_RENDERER");
    if (!configured.isEmpty()) {
        return configured;
    }
    static const QStringList candidates {
        QStringLiteral("google-chrome-stable"),
        QStringLiteral("google-chrome"),
        QStringLiteral("chromium"),
        QStringLiteral("chromium-browser")
    };
    for (const auto& candidate : candidates) {
        const auto executable = QStandardPaths::findExecutable(candidate);
        if (!executable.isEmpty()) {
            return executable;
        }
    }
    return {};
}

void setFirstError(QString* destination, const QString& value)
{
    if (destination && destination->isEmpty() && !value.isEmpty()) {
        *destination = value;
    }
}
}

QList<DownloadRequest> SiteGrabber::grab(const QUrl& root, const QString& targetDir, int depth, QString* error)
{
    SiteGrabberOptions options;
    options.depth = depth;
    auto result = grab(root, targetDir, options);
    if (error) {
        *error = result.error;
    }
    return result.requests;
}

SiteGrabberResult SiteGrabber::grab(const QUrl& root, const QString& targetDir, const SiteGrabberOptions& options)
{
    visited_.clear();
    SiteGrabberResult result;
    if (!root.isValid() || !canFetch(root)) {
        result.error = QStringLiteral("The site URL must use HTTP, HTTPS, or the local file scheme.");
        return result;
    }
    crawl(root, root, targetDir, qMax(0, options.depth), options, &result);
    return result;
}

QByteArray SiteGrabber::fetch(const QUrl& url, QString* error) const
{
    QByteArray buffer;
    auto* easy = curl_easy_init();
    if (!easy) {
        if (error) {
            *error = QStringLiteral("Could not initialize the HTTP client.");
        }
        return {};
    }
    const auto urlBytes = url.toString(QUrl::FullyEncoded).toUtf8();
    curl_easy_setopt(easy, CURLOPT_URL, urlBytes.constData());
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, 15000L);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, &buffer);
    const auto rc = curl_easy_perform(easy);
    curl_easy_cleanup(easy);
    if (rc != CURLE_OK && error) {
        *error = QString::fromUtf8(curl_easy_strerror(rc));
    }
    return rc == CURLE_OK ? buffer : QByteArray {};
}

QByteArray SiteGrabber::render(const QUrl& url, const SiteGrabberOptions& options, QString* error) const
{
    const auto executable = findRenderer(options);
    if (executable.isEmpty()) {
        if (error) {
            *error = QStringLiteral("No Chromium-family renderer was found. Install Chromium or Chrome, or set QTIDM_SITE_RENDERER.");
        }
        return {};
    }

    QTemporaryDir profile;
    if (!profile.isValid()) {
        if (error) {
            *error = QStringLiteral("Could not create a temporary browser profile.");
        }
        return {};
    }

    QProcess process;
    QStringList arguments {
        QStringLiteral("--headless=new"),
        QStringLiteral("--disable-gpu"),
        QStringLiteral("--disable-background-networking"),
        QStringLiteral("--disable-component-update"),
        QStringLiteral("--disable-default-apps"),
        QStringLiteral("--disable-sync"),
        QStringLiteral("--no-first-run"),
        QStringLiteral("--no-default-browser-check"),
        QStringLiteral("--user-data-dir=%1").arg(profile.path()),
        QStringLiteral("--virtual-time-budget=%1").arg(qMax(0, options.virtualTimeBudgetMs)),
        QStringLiteral("--dump-dom"),
        url.toString(QUrl::FullyEncoded)
    };
    process.start(executable, arguments, QIODevice::ReadOnly);
    const int timeout = qMax(100, options.renderTimeoutMs);
    if (!process.waitForStarted(qMin(timeout, 5000))) {
        if (error) {
            *error = QStringLiteral("Could not start JavaScript renderer %1: %2").arg(executable, process.errorString());
        }
        return {};
    }
    if (!process.waitForFinished(timeout)) {
        process.terminate();
        if (!process.waitForFinished(1000)) {
            process.kill();
            process.waitForFinished(1000);
        }
        if (error) {
            *error = QStringLiteral("JavaScript rendering timed out for %1.").arg(url.toDisplayString());
        }
        return {};
    }

    const auto html = process.readAllStandardOutput();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0 || html.trimmed().isEmpty()) {
        auto detail = QString::fromUtf8(process.readAllStandardError()).trimmed();
        if (detail.size() > 300) {
            detail = detail.left(300) + QStringLiteral("…");
        }
        if (error) {
            *error = detail.isEmpty()
                ? QStringLiteral("JavaScript renderer failed for %1.").arg(url.toDisplayString())
                : QStringLiteral("JavaScript renderer failed for %1: %2").arg(url.toDisplayString(), detail);
        }
        return {};
    }
    return html;
}

void SiteGrabber::crawl(const QUrl& inputUrl, const QUrl& origin, const QString& targetDir, int depth,
    const SiteGrabberOptions& options, SiteGrabberResult* result)
{
    QUrl url = inputUrl;
    url.setFragment({});
    const auto key = normalizedKey(url);
    if (!url.isValid() || !canFetch(url) || !hasSameOrigin(url, origin) || visited_.contains(key) || depth < 0) {
        return;
    }
    visited_.insert(key);

    DownloadRequest page;
    page.url = url;
    page.targetPath = QDir(targetDir).filePath(safePath(url));
    page.category = QStringLiteral("Site");
    page.segments = 4;

    const bool document = isHtmlLike(url);
    QByteArray content;
    bool rendered = false;
    if (document && options.renderJavaScript) {
        QString renderError;
        content = render(url, options, &renderError);
        if (!content.isEmpty()) {
            rendered = true;
            result->renderedDocuments.append({ url, page.targetPath, content });
        } else {
            result->warnings.append(renderError);
        }
    }

    if (!rendered) {
        result->requests.append(page);
        if (document && depth > 0) {
            QString fetchError;
            content = fetch(url, &fetchError);
            setFirstError(&result->error, fetchError);
        }
    }

    if (!document || content.isEmpty() || depth == 0) {
        return;
    }

    const auto html = QString::fromUtf8(content);
    const QRegularExpression linkRe(
        QStringLiteral("(?:href|src)\\s*=\\s*[\"']([^\"']+)[\"']"),
        QRegularExpression::CaseInsensitiveOption);
    auto match = linkRe.globalMatch(html);
    while (match.hasNext()) {
        const auto href = match.next().captured(1).trimmed();
        const auto child = url.resolved(QUrl(href));
        crawl(child, origin, targetDir, depth - 1, options, result);
    }

    const QRegularExpression srcsetRe(
        QStringLiteral("srcset\\s*=\\s*[\"']([^\"']+)[\"']"),
        QRegularExpression::CaseInsensitiveOption);
    auto srcsets = srcsetRe.globalMatch(html);
    while (srcsets.hasNext()) {
        const auto candidates = srcsets.next().captured(1).split(QLatin1Char(','));
        for (const auto& candidate : candidates) {
            const auto href = candidate.trimmed().section(QRegularExpression(QStringLiteral("\\s+")), 0, 0);
            if (!href.isEmpty()) {
                crawl(url.resolved(QUrl(href)), origin, targetDir, depth - 1, options, result);
            }
        }
    }
}

}
