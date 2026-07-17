#pragma once

#include <QDir>
#include <QString>

namespace qtidm {

class Paths final {
public:
    static QString configDir();
    static QString dataDir();
    static QString databasePath();
    static QString logPath();
    static void ensureRuntimeDirs();
};

}
