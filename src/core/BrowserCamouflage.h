#pragma once

#include <QList>
#include <QPair>
#include <QString>
#include <QUrl>
#include <QVariantMap>
#include <curl/curl.h>

namespace qtidm {

enum class BrowserTlsProfile {
    None,
    Auto,
    Chrome,
    Firefox
};

class BrowserCamouflage final {
public:
    static BrowserTlsProfile parseProfile(const QString& value);
    static QString profileToString(BrowserTlsProfile profile);
    static BrowserTlsProfile resolveProfile(BrowserTlsProfile requested, const QString& userAgent);

    static QString defaultUserAgent(BrowserTlsProfile profile);
    static QByteArray tls12CipherList(BrowserTlsProfile profile);
    static QByteArray tls13CipherSuites(BrowserTlsProfile profile);
    static QByteArray ecCurves(BrowserTlsProfile profile);

    static void applyTlsProfile(CURL* easy, BrowserTlsProfile profile);

    // Produces outgoing HTTP headers ordered to match Chrome or Firefox wire order,
    // excluding internal `_qtidm*` metadata and `Cookie` (handled via libcurl's cookie jar).
    static QList<QPair<QString, QString>> buildOrderedHeaders(const QVariantMap& headers,
                                                              BrowserTlsProfile profile,
                                                              const QUrl& url,
                                                              bool includeDefaults = true);
};

} // namespace qtidm
