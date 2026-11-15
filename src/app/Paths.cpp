#include "app/Paths.h"

#include <QStandardPaths>

namespace qtidm {

// These are deliberately derived from the generic locations plus a fixed name
// rather than from the application name: qtidm-cli, the native messaging host
// and the application itself must all resolve the same directory.
QString Paths::configDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
        + QStringLiteral("/qtIDM");
}

QString Paths::dataDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
        + QStringLiteral("/qtIDM");
}

QString Paths::databasePath()
{
    return dataDir() + QStringLiteral("/downloads.sqlite3");
}

QString Paths::logPath()
{
    return dataDir() + QStringLiteral("/qtidm.log");
}

void Paths::ensureRuntimeDirs()
{
    QDir().mkpath(configDir());
    QDir().mkpath(dataDir());
}

}
