#pragma once

#include <QObject>
#include <QString>

class QFileSystemWatcher;
class QTimer;

namespace qtidm {

enum class ThemeMode {
    Light,
    Dark
};

class ThemeManager final : public QObject {
    Q_OBJECT
public:
    explicit ThemeManager(QObject* parent = nullptr);
    ThemeMode mode() const;
    QString styleSheet() const;
    static ThemeMode detectSystemTheme();

public slots:
#ifndef Q_OS_WIN
    void refreshFromPortal();
#endif
    void refreshFromSystemTheme();

signals:
    void themeChanged();

private:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void setupThemeWatchers();
    void scheduleThemeRefresh();
    void refreshTheme();

    ThemeMode mode_ = ThemeMode::Light;
    QFileSystemWatcher* watcher_ = nullptr;
    QTimer* refreshTimer_ = nullptr;
};

}