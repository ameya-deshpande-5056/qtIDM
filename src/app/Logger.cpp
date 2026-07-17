#include "app/Logger.h"

#include "app/Paths.h"

#include <QDateTime>
#include <QFile>
#include <QMutex>
#include <QStringList>
#include <QTextStream>
#include <QtGlobal>
#include <cstdlib>
#include <execinfo.h>
#include <iterator>

namespace qtidm::Logger {

namespace {

QMutex logMutex;

QString stackTrace()
{
    void* frames[64] {};
    const int count = backtrace(frames, std::size(frames));
    char** symbols = backtrace_symbols(frames, count);
    QStringList lines;
    for (int index = 0; symbols && index < count; ++index) {
        lines.append(QString::fromLocal8Bit(symbols[index]));
    }
    std::free(symbols);
    return lines.join(QLatin1Char('\n'));
}

void write(const QString& level, const QString& context, const QString& message, bool includeStack)
{
    QMutexLocker lock(&logMutex);
    Paths::ensureRuntimeDirs();
    QFile file(Paths::logPath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return;
    }
    QTextStream output(&file);
    output << QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)
           << " [" << level << "] [" << context << "] " << message << '\n';
    if (includeStack) {
        output << "Stack trace:\n" << stackTrace() << '\n';
    }
}

void messageHandler(QtMsgType type, const QMessageLogContext&, const QString& message)
{
    const bool isError = type == QtCriticalMsg || type == QtFatalMsg;
    write(isError ? QStringLiteral("ERROR") : QStringLiteral("QT"),
          QStringLiteral("Qt"), message, isError);
}

}

void install()
{
    qInstallMessageHandler(messageHandler);
}

void info(const QString& context, const QString& message)
{
    write(QStringLiteral("INFO"), context, message, false);
}

void error(const QString& context, const QString& message)
{
    write(QStringLiteral("ERROR"), context, message, true);
}

}
