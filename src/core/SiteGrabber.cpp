#include "core/SiteGrabber.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
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
}

QList<DownloadRequest> SiteGrabber::grab(const QUrl& root, const QString& targetDir, int depth, QString* error)
{
    visited_.clear();
    QList<DownloadRequest> output;
    crawl(root, targetDir, qMax(0, depth), &output, error);
    return output;
}

QByteArray SiteGrabber::fetch(const QUrl& url, QString* error) const
{
    QByteArray buffer;
    auto* easy = curl_easy_init();
    const auto urlBytes = url.toString().toUtf8();
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

void SiteGrabber::crawl(const QUrl& url, const QString& targetDir, int depth, QList<DownloadRequest>* output, QString* error)
{
    if (!url.isValid() || visited_.contains(url.toString()) || depth < 0) {
        return;
    }
    visited_.insert(url.toString());

    DownloadRequest page;
    page.url = url;
    page.targetPath = QDir(targetDir).filePath(safePath(url));
    page.category = QStringLiteral("Site");
    page.segments = 4;
    output->append(page);

    const auto html = QString::fromUtf8(fetch(url, error));
    if (html.isEmpty() || depth == 0) {
        return;
    }

    const QRegularExpression linkRe(QStringLiteral("(?:href|src)\\s*=\\s*[\"']([^\"'#]+)[\"']"), QRegularExpression::CaseInsensitiveOption);
    auto match = linkRe.globalMatch(html);
    while (match.hasNext()) {
        const auto href = match.next().captured(1);
        const auto child = url.resolved(QUrl(href));
        if (child.host() != url.host()) {
            continue;
        }
        crawl(child, targetDir, depth - 1, output, error);
    }
}

}
