#pragma once

#include "app/AutomationNames.h"

#include <QString>

namespace qtidm {

// The discovery file a running qtIDM publishes so that other programs can find
// its automation channel without linking against it.
class AutomationIpc final {
public:
    static constexpr auto serviceName = automation::service;
    static constexpr auto objectPath = automation::objectPath;
    static constexpr auto interfaceName = automation::interface;
    static constexpr auto fileName = automation::fileName;

    // An empty directory means Paths::dataDir(), the same place the application
    // keeps its download database.
    explicit AutomationIpc(QString directory = {});

    // The session bus name this process uses: the canonical name, or that name
    // with the instance suffix when a data directory was chosen explicitly.
    static QString channelServiceName();

    bool start();
    void stop();

    QString path() const;
    QString token() const;
    QString lastError() const;

    static QString newToken();

private:
    QString directory_;
    QString token_;
    mutable QString lastError_;
};

}
