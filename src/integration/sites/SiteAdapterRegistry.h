#pragma once

#include "integration/sites/SiteAdapter.h"

#include <memory>
#include <vector>

namespace qtidm {

class SiteAdapterRegistry final {
public:
    SiteAdapterRegistry();

    static const SiteAdapterRegistry& defaultRegistry();

    void registerAdapter(std::unique_ptr<SiteAdapter> adapter);
    const SiteAdapter* findAdapterByName(const QString& name) const;
    const SiteAdapter* findAdapterFor(const QUrl& url,
                                      const QUrl& refreshTarget = {},
                                      const QString& hint = {}) const;
    QUrl inferRefreshTarget(const QUrl& downloadUrl, const QString& hint = {}) const;

private:
    std::vector<std::unique_ptr<SiteAdapter>> adapters_;
};

} // namespace qtidm
