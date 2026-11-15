#ifndef QTIDM_UPDATE_DIALOG_H
#define QTIDM_UPDATE_DIALOG_H

#include "integration/UpdateChecker.h"

#include <QDialog>

class QCheckBox;
class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QTableWidget;

namespace qtidm {

class UpdateDialog : public QDialog {
    Q_OBJECT

public:
    explicit UpdateDialog(UpdateCheckResult result, UpdateChecker* checker, QWidget* parent = nullptr);
    ~UpdateDialog() override;

    void updateResult(const UpdateCheckResult& result);

private:
    void setupUi();
    void populateData();
    void startUpdate(const QString& dependencyName);
    void updateAllDependencies();

    UpdateCheckResult result_;
    UpdateChecker* checker_;

    QLabel* appStatusLabel_ = nullptr;
    QPushButton* appUpdateButton_ = nullptr;
    QPlainTextEdit* releaseNotesText_ = nullptr;
    QTableWidget* dependenciesTable_ = nullptr;
    QPushButton* updateAllDepsButton_ = nullptr;
    QPushButton* checkAgainButton_ = nullptr;
    QProgressBar* progressBar_ = nullptr;
    QPlainTextEdit* logView_ = nullptr;
    QCheckBox* startupCheckCheckbox_ = nullptr;
};

} // namespace qtidm

#endif // QTIDM_UPDATE_DIALOG_H
