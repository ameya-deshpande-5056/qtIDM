#pragma once

#include <QHash>
#include <QObject>
#include <QSharedPointer>

class QProcess;
class QTemporaryDir;

namespace qtidm {

class ArchiveExtractor final : public QObject {
    Q_OBJECT
public:
    explicit ArchiveExtractor(QObject* parent = nullptr);

    static bool supports(const QString& path);
    static bool listingIsSafe(const QByteArray& listing, QString* error = nullptr);
    bool isAvailable() const;
    void extract(const QString& id, const QString& archivePath, const QString& destination,
        bool deleteArchiveAfterExtraction);

signals:
    void extractionStarted(QString id, QString archivePath, QString destination);
    void extractionFinished(QString id, QString archivePath, QString destination);
    void extractionFailed(QString id, QString message);

private:
    struct Job {
        QString id;
        QString archivePath;
        QString destination;
        QSharedPointer<QTemporaryDir> stagingDirectory;
        bool deleteArchive = false;
        bool listing = true;
    };

    QString executable() const;
    void finishProcess(QProcess* process, int exitCode, int exitStatus);

    QHash<QProcess*, Job> jobs_;
};

}
