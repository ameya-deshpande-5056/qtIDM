#include "core/BrowserCamouflage.h"

#include <QStringList>
#include <algorithm>

namespace qtidm {

namespace {
int headerRankChrome(const QString& lowerKey)
{
    static const QStringList order {
        QStringLiteral("host"),
        QStringLiteral("connection"),
        QStringLiteral("cache-control"),
        QStringLiteral("sec-ch-ua"),
        QStringLiteral("sec-ch-ua-mobile"),
        QStringLiteral("sec-ch-ua-platform"),
        QStringLiteral("upgrade-insecure-requests"),
        QStringLiteral("user-agent"),
        QStringLiteral("accept"),
        QStringLiteral("sec-fetch-site"),
        QStringLiteral("sec-fetch-mode"),
        QStringLiteral("sec-fetch-user"),
        QStringLiteral("sec-fetch-dest"),
        QStringLiteral("referer"),
        QStringLiteral("origin"),
        QStringLiteral("accept-encoding"),
        QStringLiteral("accept-language"),
        QStringLiteral("authorization"),
        QStringLiteral("if-range"),
        QStringLiteral("range"),
        QStringLiteral("cookie")
    };
    const int idx = order.indexOf(lowerKey);
    return idx >= 0 ? idx : 1000;
}

int headerRankFirefox(const QString& lowerKey)
{
    static const QStringList order {
        QStringLiteral("host"),
        QStringLiteral("user-agent"),
        QStringLiteral("accept"),
        QStringLiteral("accept-language"),
        QStringLiteral("accept-encoding"),
        QStringLiteral("referer"),
        QStringLiteral("origin"),
        QStringLiteral("connection"),
        QStringLiteral("upgrade-insecure-requests"),
        QStringLiteral("sec-fetch-dest"),
        QStringLiteral("sec-fetch-mode"),
        QStringLiteral("sec-fetch-site"),
        QStringLiteral("sec-fetch-user"),
        QStringLiteral("authorization"),
        QStringLiteral("if-range"),
        QStringLiteral("range"),
        QStringLiteral("cookie")
    };
    const int idx = order.indexOf(lowerKey);
    return idx >= 0 ? idx : 1000;
}
} // namespace

BrowserTlsProfile BrowserCamouflage::parseProfile(const QString& value)
{
    const auto lower = value.trimmed().toLower();
    if (lower == QStringLiteral("none") || lower == QStringLiteral("off") || lower == QStringLiteral("disabled")) {
        return BrowserTlsProfile::None;
    }
    if (lower == QStringLiteral("chrome") || lower == QStringLiteral("chromium") || lower == QStringLiteral("edge")) {
        return BrowserTlsProfile::Chrome;
    }
    if (lower == QStringLiteral("firefox") || lower == QStringLiteral("gecko")) {
        return BrowserTlsProfile::Firefox;
    }
    return BrowserTlsProfile::Auto;
}

QString BrowserCamouflage::profileToString(BrowserTlsProfile profile)
{
    switch (profile) {
    case BrowserTlsProfile::None: return QStringLiteral("none");
    case BrowserTlsProfile::Chrome: return QStringLiteral("chrome");
    case BrowserTlsProfile::Firefox: return QStringLiteral("firefox");
    case BrowserTlsProfile::Auto:
    default: return QStringLiteral("auto");
    }
}

BrowserTlsProfile BrowserCamouflage::resolveProfile(BrowserTlsProfile requested, const QString& userAgent)
{
    if (requested != BrowserTlsProfile::Auto) {
        return requested;
    }
    if (userAgent.contains(QStringLiteral("Firefox/"), Qt::CaseInsensitive)) {
        return BrowserTlsProfile::Firefox;
    }
    return BrowserTlsProfile::Chrome;
}

QString BrowserCamouflage::defaultUserAgent(BrowserTlsProfile profile)
{
    if (profile == BrowserTlsProfile::Firefox) {
        return QStringLiteral("Mozilla/5.0 (X11; Linux x86_64; rv:136.0) Gecko/20100101 Firefox/136.0");
    }
    return QStringLiteral("Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/134.0.0.0 Safari/537.36");
}

QByteArray BrowserCamouflage::tls12CipherList(BrowserTlsProfile profile)
{
    if (profile == BrowserTlsProfile::None) {
        return {};
    }
    if (profile == BrowserTlsProfile::Firefox) {
        return QByteArrayLiteral(
            "ECDHE-ECDSA-AES128-GCM-SHA256:"
            "ECDHE-RSA-AES128-GCM-SHA256:"
            "ECDHE-ECDSA-CHACHA20-POLY1305:"
            "ECDHE-RSA-CHACHA20-POLY1305:"
            "ECDHE-ECDSA-AES256-GCM-SHA384:"
            "ECDHE-RSA-AES256-GCM-SHA384:"
            "ECDHE-ECDSA-AES256-SHA:"
            "ECDHE-ECDSA-AES128-SHA:"
            "ECDHE-RSA-AES128-SHA:"
            "ECDHE-RSA-AES256-SHA:"
            "AES128-GCM-SHA256:"
            "AES256-GCM-SHA384:"
            "AES128-SHA:"
            "AES256-SHA");
    }
    return QByteArrayLiteral(
        "ECDHE-ECDSA-AES128-GCM-SHA256:"
        "ECDHE-RSA-AES128-GCM-SHA256:"
        "ECDHE-ECDSA-AES256-GCM-SHA384:"
        "ECDHE-RSA-AES256-GCM-SHA384:"
        "ECDHE-ECDSA-CHACHA20-POLY1305:"
        "ECDHE-RSA-CHACHA20-POLY1305:"
        "ECDHE-RSA-AES128-SHA:"
        "ECDHE-RSA-AES256-SHA:"
        "AES128-GCM-SHA256:"
        "AES256-GCM-SHA384:"
        "AES128-SHA:"
        "AES256-SHA");
}

QByteArray BrowserCamouflage::tls13CipherSuites(BrowserTlsProfile profile)
{
    if (profile == BrowserTlsProfile::None) {
        return {};
    }
    if (profile == BrowserTlsProfile::Firefox) {
        return QByteArrayLiteral("TLS_AES_128_GCM_SHA256:TLS_CHACHA20_POLY1305_SHA256:TLS_AES_256_GCM_SHA384");
    }
    return QByteArrayLiteral("TLS_AES_128_GCM_SHA256:TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256");
}

QByteArray BrowserCamouflage::ecCurves(BrowserTlsProfile profile)
{
    if (profile == BrowserTlsProfile::None) {
        return {};
    }
    if (profile == BrowserTlsProfile::Firefox) {
        return QByteArrayLiteral("X25519:P-256:P-384:P-521");
    }
    return QByteArrayLiteral("X25519:P-256:P-384");
}

void BrowserCamouflage::applyTlsProfile(CURL* easy, BrowserTlsProfile profile)
{
    if (!easy || profile == BrowserTlsProfile::None) {
        return;
    }
    curl_easy_setopt(easy, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
    const auto ciphers12 = tls12CipherList(profile);
    if (!ciphers12.isEmpty()) {
        curl_easy_setopt(easy, CURLOPT_SSL_CIPHER_LIST, ciphers12.constData());
    }
#ifdef CURLOPT_TLS13_CIPHERS
    const auto ciphers13 = tls13CipherSuites(profile);
    if (!ciphers13.isEmpty()) {
        curl_easy_setopt(easy, CURLOPT_TLS13_CIPHERS, ciphers13.constData());
    }
#endif
#ifdef CURLOPT_SSL_EC_CURVES
    const auto curves = ecCurves(profile);
    if (!curves.isEmpty()) {
        curl_easy_setopt(easy, CURLOPT_SSL_EC_CURVES, curves.constData());
    }
#endif
}

QList<QPair<QString, QString>> BrowserCamouflage::buildOrderedHeaders(const QVariantMap& headers,
                                                                      BrowserTlsProfile profile,
                                                                      const QUrl& url,
                                                                      bool includeDefaults)
{
    Q_UNUSED(url);
    QList<QPair<QString, QString>> entries;
    bool hasUserAgent = false;
    bool hasAccept = false;
    bool hasAcceptLanguage = false;

    for (auto it = headers.cbegin(); it != headers.cend(); ++it) {
        const auto key = it.key().trimmed();
        if (key.isEmpty() || key.startsWith(QLatin1Char('_'))) {
            continue;
        }
        const auto lower = key.toLower();
        if (lower == QStringLiteral("cookie")) {
            continue;
        }
        const auto value = it.value().toString();
        if (lower == QStringLiteral("user-agent") && !value.trimmed().isEmpty()) {
            hasUserAgent = true;
        } else if (lower == QStringLiteral("accept") && !value.trimmed().isEmpty()) {
            hasAccept = true;
        } else if (lower == QStringLiteral("accept-language") && !value.trimmed().isEmpty()) {
            hasAcceptLanguage = true;
        }
        entries.append({ key, value });
    }

    const auto effectiveProfile = resolveProfile(profile, hasUserAgent
        ? [&]() {
              for (const auto& pair : entries) {
                  if (pair.first.compare(QStringLiteral("User-Agent"), Qt::CaseInsensitive) == 0) {
                      return pair.second;
                  }
              }
              return QString {};
          }()
        : QString {});

    if (includeDefaults && effectiveProfile != BrowserTlsProfile::None) {
        if (!hasUserAgent) {
            entries.append({ QStringLiteral("User-Agent"), defaultUserAgent(effectiveProfile) });
        }
        if (!hasAccept) {
            entries.append({ QStringLiteral("Accept"), QStringLiteral("*/*") });
        }
        if (!hasAcceptLanguage) {
            entries.append({ QStringLiteral("Accept-Language"), QStringLiteral("en-US,en;q=0.9") });
        }
    }

    auto rankFn = (effectiveProfile == BrowserTlsProfile::Firefox) ? &headerRankFirefox : &headerRankChrome;
    std::stable_sort(entries.begin(), entries.end(), [rankFn](const auto& left, const auto& right) {
        const auto lLower = left.first.toLower();
        const auto rLower = right.first.toLower();
        const int lRank = rankFn(lLower);
        const int rRank = rankFn(rLower);
        if (lRank != rRank) {
            return lRank < rRank;
        }
        return lLower < rLower;
    });
    return entries;
}

} // namespace qtidm
