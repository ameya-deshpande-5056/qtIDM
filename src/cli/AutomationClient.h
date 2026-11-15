#pragma once

#include <QString>
#include <QVariantList>

namespace qtidm {

// Widgets-free client for a running qtIDM's automation channel.
class AutomationClient final {
public:
    // An empty directory means Paths::dataDir().
    explicit AutomationClient(QString directory = {});

    // Returns the JSON answer, or an empty string with *error set.
    QString call(const QString& method, const QVariantList& arguments,
                 QString* error = nullptr) const;

private:
    bool resolve(QString* error) const;

    QString directory_;
    mutable bool resolved_ = false;
    mutable QString discoveryPath_;
    mutable QString resolveError_;
};

}
