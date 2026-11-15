#pragma once

#include <QDir>
#include <QString>

namespace qtidm {

class Paths final {
public:
    // Redirects everything qtIDM writes into one directory, for portable copies
    // and for running beside an installed instance. Call before anything else
    // reads a path; an empty value restores the standard locations.
    static void setDataDirectory(const QString& directory);
    static QString dataDirOverride();
    // A stable suffix for this data directory, so a second instance gets its own
    // D-Bus name instead of taking over the first one.
    static QString instanceSuffix();

    static QString configDir();
    static QString dataDir();
    static QString databasePath();
    static QString logPath();
    static void ensureRuntimeDirs();
};

}
