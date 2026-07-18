#pragma once

#include <QObject>
#include <QTimer>

namespace qtidm {

class MeteredNetworkMonitor final : public QObject {
    Q_OBJECT
public:
    explicit MeteredNetworkMonitor(QObject* parent = nullptr);

    bool isMetered() const;
    static bool valueMeansMetered(uint value);

public slots:
    void refresh();

signals:
    void meteredChanged(bool metered);

private:
    bool metered_ = false;
    QTimer timer_;
};

}
