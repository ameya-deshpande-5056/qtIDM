#include "integration/sites/SiteAdapterRegistry.h"

#include "integration/sites/GoFileAdapter.h"
#include "integration/sites/GoogleDriveAdapter.h"
#include "integration/sites/MediaFireAdapter.h"

namespace qtidm {

SiteAdapterRegistry::SiteAdapterRegistry()
{
    registerAdapter(std::make_unique<MediaFireAdapter>());
    registerAdapter(std::make_unique<GoogleDriveAdapter>());
    registerAdapter(std::make_unique<GoFileAdapter>());
}

const SiteAdapterRegistry& SiteAdapterRegistry::defaultRegistry()
{
    static const SiteAdapterRegistry registry;
    return registry;
}

void SiteAdapterRegistry::registerAdapter(std::unique_ptr<SiteAdapter> adapter)
{
    if (adapter) {
        adapters_.push_back(std::move(adapter));
    }
}

const SiteAdapter* SiteAdapterRegistry::findAdapterByName(const QString& name) const
{
    const auto lower = name.trimmed().toLower();
    if (lower.isEmpty()) {
        return nullptr;
    }
    for (const auto& adapter : adapters_) {
        if (adapter->name().compare(lower, Qt::CaseInsensitive) == 0) {
            return adapter.get();
        }
    }
    if (lower == QStringLiteral("gdrive")) {
        return findAdapterByName(QStringLiteral("google-drive"));
    }
    return nullptr;
}

const SiteAdapter* SiteAdapterRegistry::findAdapterFor(const QUrl& url,
                                                       const QUrl& refreshTarget,
                                                       const QString& hint) const
{
    if (!hint.isEmpty()) {
        if (const auto* byHint = findAdapterByName(hint)) {
            return byHint;
        }
    }
    for (const auto& adapter : adapters_) {
        if (adapter->matchesUrl(url)) {
            return adapter.get();
        }
    }
    if (refreshTarget.isValid()) {
        for (const auto& adapter : adapters_) {
            if (adapter->matchesUrl(refreshTarget)) {
                return adapter.get();
            }
        }
    }
    return nullptr;
}

QUrl SiteAdapterRegistry::inferRefreshTarget(const QUrl& downloadUrl, const QString& hint) const
{
    if (const auto* adapter = findAdapterFor(downloadUrl, {}, hint)) {
        return adapter->inferRefreshTarget(downloadUrl);
    }
    return {};
}

} // namespace qtidm
