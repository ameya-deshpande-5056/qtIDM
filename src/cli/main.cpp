#include "cli/AutomationClient.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdio>

namespace {

constexpr auto usageText =
    "qtidm-cli - control a running qtIDM instance\n"
    "\n"
    "Usage: qtidm-cli [options] <command> [arguments]\n"
    "\n"
    "Commands:\n"
    "  list                 Show downloads and queues\n"
    "  pause <id>...        Pause active downloads\n"
    "  start <id>...        Start or resume stopped downloads\n"
    "  remove <id>...       Remove from the list, keeping the files\n"
    "  delete <id>...       Remove from the list and delete the files\n"
    "  start-queue <name>   Let a queue dispatch downloads\n"
    "  stop-queue <name>    Hold a queue without interrupting active downloads\n"
    "\n"
    "Options:\n"
    "  -h, --help           Show this help\n"
    "  -V, --version        Show the version\n"
    "      --json           Print the exact JSON answer\n"
    "      --data <dir>     Read ipc.json from this qtIDM data directory\n"
    "\n"
    "Commands that fail exit with status 1; usage errors exit with 2.\n";

QString formatBytes(qint64 bytes)
{
    static const char* units[] = { "B", "KiB", "MiB", "GiB", "TiB", "PiB" };
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 5) {
        value /= 1024.0;
        ++unit;
    }
    if (unit == 0) {
        return QStringLiteral("%1 B").arg(bytes);
    }
    return QStringLiteral("%1 %2").arg(value, 0, 'f', 1).arg(QLatin1String(units[unit]));
}

void printList(const QJsonObject& answer)
{
    const auto downloads = answer.value(QStringLiteral("downloads")).toArray();
    if (downloads.isEmpty()) {
        std::printf("No downloads.\n");
    }
    for (const auto& entry : downloads) {
        const auto download = entry.toObject();
        const auto size = download.value(QStringLiteral("size"));
        const auto sizeText = size.isNull() ? QStringLiteral("?") : formatBytes(size.toInteger());
        std::printf("%-12s %10s  %s\n",
                    qPrintable(download.value(QStringLiteral("status")).toString()),
                    qPrintable(sizeText),
                    qPrintable(download.value(QStringLiteral("file")).toString()));
        std::printf("             id %s\n",
                    qPrintable(download.value(QStringLiteral("id")).toString()));
    }
    std::printf("\n%lld download(s), %s/s total",
                static_cast<long long>(downloads.size()),
                qPrintable(formatBytes(answer.value(QStringLiteral("speed")).toInteger())));
    const auto free = answer.value(QStringLiteral("free"));
    if (!free.isNull()) {
        std::printf(", %s free", qPrintable(formatBytes(free.toInteger())));
    }
    std::printf("\n");
    for (const auto& entry : answer.value(QStringLiteral("queues")).toArray()) {
        const auto queue = entry.toObject();
        std::printf("queue %s: %s, %lld active, %lld pending\n",
                    qPrintable(queue.value(QStringLiteral("name")).toString()),
                    queue.value(QStringLiteral("running")).toBool() ? "running" : "held",
                    static_cast<long long>(queue.value(QStringLiteral("active")).toInteger()),
                    static_cast<long long>(queue.value(QStringLiteral("pending")).toInteger()));
    }
}

void printOutcome(const QJsonObject& answer)
{
    if (answer.contains(QStringLiteral("error"))) {
        std::fprintf(stderr, "error: %s\n",
                     qPrintable(answer.value(QStringLiteral("error")).toString()));
        return;
    }
    if (answer.contains(QStringLiteral("running"))) {
        std::printf("queue %s: %s\n",
                    qPrintable(answer.value(QStringLiteral("queue")).toString()),
                    answer.value(QStringLiteral("running")).toBool() ? "running" : "held");
        return;
    }
    const auto done = answer.value(QStringLiteral("done")).toArray();
    const auto skipped = answer.value(QStringLiteral("skipped")).toArray();
    std::printf("%s: %lld done, %lld skipped\n",
                qPrintable(answer.value(QStringLiteral("action")).toString()),
                static_cast<long long>(done.size()), static_cast<long long>(skipped.size()));
    for (const auto& entry : skipped) {
        const auto skip = entry.toObject();
        std::printf("  skipped %s: %s\n",
                    qPrintable(skip.value(QStringLiteral("id")).toString()),
                    qPrintable(skip.value(QStringLiteral("why")).toString()));
    }
}

}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("qtidm-cli"));
    QCoreApplication::setApplicationVersion(QString::fromLatin1(QTIDM_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Control a running qtIDM instance."));
    parser.setSingleDashWordOptionMode(QCommandLineParser::ParseAsLongOptions);
    // Registered as plain options so the compact usage text below is printed
    // instead of QCommandLineParser's generated one.
    const QCommandLineOption helpOption({ QStringLiteral("h"), QStringLiteral("help") },
                                        QStringLiteral("Show this help."));
    const QCommandLineOption versionOption({ QStringLiteral("V"), QStringLiteral("version") },
                                           QStringLiteral("Show the version."));
    parser.addOption(helpOption);
    parser.addOption(versionOption);
    QCommandLineOption jsonOption(QStringLiteral("json"),
                                  QStringLiteral("Print the exact JSON answer."));
    QCommandLineOption dataOption(QStringLiteral("data"),
                                  QStringLiteral("qtIDM data directory containing ipc.json."),
                                  QStringLiteral("dir"));
    parser.addOption(jsonOption);
    parser.addOption(dataOption);
    parser.process(app);

    if (parser.isSet(helpOption)) {
        std::printf("%s", usageText);
        return 0;
    }
    if (parser.isSet(versionOption)) {
        std::printf("qtidm-cli %s\n", qPrintable(QCoreApplication::applicationVersion()));
        return 0;
    }

    const auto arguments = parser.positionalArguments();
    if (arguments.isEmpty()) {
        std::fprintf(stderr, "%s", usageText);
        return 2;
    }
    const auto command = arguments.first();
    const auto operands = arguments.mid(1);
    const bool json = parser.isSet(jsonOption);

    QString method;
    QVariantList callArguments;
    if (command == QLatin1String("list") || command == QLatin1String("status")) {
        method = QStringLiteral("ListDownloads");
    } else if (command == QLatin1String("pause")) {
        method = QStringLiteral("PauseDownloads");
        callArguments.append(operands);
    } else if (command == QLatin1String("start")) {
        method = QStringLiteral("StartDownloads");
        callArguments.append(operands);
    } else if (command == QLatin1String("remove")) {
        method = QStringLiteral("RemoveDownloads");
        callArguments.append(operands);
    } else if (command == QLatin1String("delete")) {
        method = QStringLiteral("DeleteDownloads");
        callArguments.append(operands);
    } else if (command == QLatin1String("start-queue")) {
        method = QStringLiteral("StartQueue");
        callArguments.append(operands.value(0));
    } else if (command == QLatin1String("stop-queue")) {
        method = QStringLiteral("StopQueue");
        callArguments.append(operands.value(0));
    } else {
        std::fprintf(stderr, "qtidm-cli: unknown command '%s'\n\n%s",
                     qPrintable(command), usageText);
        return 2;
    }

    if (method != QLatin1String("ListDownloads") && operands.isEmpty()) {
        std::fprintf(stderr, "qtidm-cli: '%s' needs %s\n", qPrintable(command),
                     command.endsWith(QLatin1String("-queue")) ? "a queue name" : "at least one download id");
        return 2;
    }

    qtidm::AutomationClient client(parser.value(dataOption));
    QString error;
    const auto raw = client.call(method, callArguments, &error);
    if (!error.isEmpty()) {
        std::fprintf(stderr, "qtidm-cli: %s\n", qPrintable(error));
        return 1;
    }
    if (json) {
        std::printf("%s\n", qPrintable(raw));
    }

    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(raw.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (!json) {
            std::printf("%s\n", qPrintable(raw));
        }
        return 0;
    }
    const auto answer = document.object();
    if (!json) {
        if (command == QLatin1String("list") || command == QLatin1String("status")) {
            printList(answer);
        } else {
            printOutcome(answer);
        }
    }
    return answer.contains(QStringLiteral("error")) ? 1 : 0;
}
