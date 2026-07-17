#pragma once

#include "core/DownloadTypes.h"

#include <QWidget>

namespace qtidm {

class SegmentGrid final : public QWidget {
    Q_OBJECT
public:
    explicit SegmentGrid(QWidget* parent = nullptr);
    void setSegments(QVector<SegmentInfo> segments);

protected:
    void paintEvent(QPaintEvent* event) override;
    QSize sizeHint() const override;

private:
    QVector<SegmentInfo> segments_;
};

}
