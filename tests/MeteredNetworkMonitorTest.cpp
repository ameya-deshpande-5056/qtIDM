#include "integration/MeteredNetworkMonitor.h"

#include <QtTest/QtTest>

class MeteredNetworkMonitorTest final : public QObject {
    Q_OBJECT
private slots:
    void mapsNetworkManagerMeteredValues()
    {
        QVERIFY(!qtidm::MeteredNetworkMonitor::valueMeansMetered(0));
        QVERIFY(qtidm::MeteredNetworkMonitor::valueMeansMetered(1));
        QVERIFY(!qtidm::MeteredNetworkMonitor::valueMeansMetered(2));
        QVERIFY(qtidm::MeteredNetworkMonitor::valueMeansMetered(3));
        QVERIFY(!qtidm::MeteredNetworkMonitor::valueMeansMetered(4));
    }
};

QTEST_MAIN(MeteredNetworkMonitorTest)
#include "MeteredNetworkMonitorTest.moc"
