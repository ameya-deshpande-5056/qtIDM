#pragma once

#include <QObject>
#include <QString>

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

public slots:
    void refreshFromPortal();

signals:
    void themeChanged();

private:
    ThemeMode mode_ = ThemeMode::Light;
};

}
