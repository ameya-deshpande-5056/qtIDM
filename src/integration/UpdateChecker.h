#ifndef QTIDM_UPDATE_CHECKER_H
#define QTIDM_UPDATE_CHECKER_H

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QString>
#include <QUrl>

namespace qtidm {

struct DependencyUpdateInfo {
    QString name;               // "yt-dlp", "Deno", "FFmpeg", "7-Zip"
    QString executablePath;     // Path to resolved binary
    QString installedVersion;   // e.g. "2026.10.26"
    QString latestVersion;      // e.g. "2026.11.01"
    bool installed = false;
    bool updateAvailable = false;
    bool canAutoUpdate = false;
    QString updateCommand;
    QString installCommand;
    QString statusText;
};

struct AppUpdateInfo {
    QString currentVersion;     // e.g. "0.3.3"
    QString latestVersion;      // e.g. "0.3.4"
    QString releaseTitle;       // e.g. "qtIDM 0.3.4"
    QString releaseTag;         // e.g. "v0.3.4"
    QString releaseUrl;         // GitHub release URL
    QString releaseNotes;       // Markdown changelog
    QString publishedDate;      // ISO string or formatted date
    bool updateAvailable = false;
    bool checkSucceeded = false;
    QString errorMessage;
};

struct UpdateCheckResult {
    AppUpdateInfo app;
    QList<DependencyUpdateInfo> dependencies;
    bool checkSucceeded = false;
    QString errorMessage;

    bool hasAnyUpdates() const {
        if (app.updateAvailable) return true;
        for (const auto& dep : dependencies) {
            if (dep.updateAvailable) return true;
        }
        return false;
    }

    QString summary() const {
        QStringList items;
        if (app.updateAvailable) {
            items.append(QStringLiteral("qtIDM %1 is available (current: %2).")
                             .arg(app.latestVersion, app.currentVersion));
        }
        for (const auto& dep : dependencies) {
            if (dep.updateAvailable) {
                items.append(QStringLiteral("%1 %2 is available (installed: %3).")
                                 .arg(dep.name, dep.latestVersion, dep.installedVersion));
            }
        }
        return items.join(QStringLiteral("\n"));
    }
};

class UpdateChecker : public QObject {
    Q_OBJECT

public:
    explicit UpdateChecker(QObject* parent = nullptr);
    ~UpdateChecker() override;

    // Asynchronously check for updates in a worker thread.
    void checkForUpdates(bool silentIfUpToDate = false);

    // Synchronously check for updates.
    UpdateCheckResult checkSync();

    // Run dependency update via QProcess.
    void runDependencyUpdate(const QString& dependencyName);

    // Exposed static helpers for version logic and testing.
    static bool isVersionNewer(const QString& latest, const QString& current);
    static QList<int> parseVersionNumbers(const QString& versionStr);
    static QString cleanVersionTag(const QString& rawTag);

signals:
    void checkStarted();
    void checkFinished(const qtidm::UpdateCheckResult& result, bool silentIfUpToDate);
    void checkFailed(const QString& errorMessage, bool silentIfUpToDate);

    void dependencyUpdateStarted(const QString& dependencyName);
    void dependencyUpdateOutput(const QString& dependencyName, const QString& text);
    void dependencyUpdateFinished(const QString& dependencyName, bool success, const QString& message);

private:
    QByteArray httpGet(const QString& url, QString* effectiveUrl = nullptr, QString* error = nullptr) const;

    AppUpdateInfo checkAppUpdate() const;
    DependencyUpdateInfo checkYtDlp() const;
    DependencyUpdateInfo checkDeno() const;
    DependencyUpdateInfo checkFfmpeg() const;
    DependencyUpdateInfo check7Zip() const;

    bool isChecking_ = false;
};

} // namespace qtidm

Q_DECLARE_METATYPE(qtidm::UpdateCheckResult)

#endif // QTIDM_UPDATE_CHECKER_H
