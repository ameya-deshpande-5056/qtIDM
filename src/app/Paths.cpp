#include "app/Paths.h"

#include <QStandardPaths>

namespace qtidm {

QString Paths::configDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + QStringLiteral("/qtIDM");
}

QString Paths::dataDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
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
