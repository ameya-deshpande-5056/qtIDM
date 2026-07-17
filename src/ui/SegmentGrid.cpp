#include "ui/SegmentGrid.h"

#include <QPainter>

namespace qtidm {

SegmentGrid::SegmentGrid(QWidget* parent)
    : QWidget(parent)
{
    setMinimumHeight(28);
}

void SegmentGrid::setSegments(QVector<SegmentInfo> segments)
{
    segments_ = std::move(segments);
    update();
}

void SegmentGrid::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.fillRect(rect(), palette().base());
    const int count = qMax(1, segments_.size());
    const int cellWidth = qMax(1, width() / count);
    for (int i = 0; i < count; ++i) {
        const QRect cell(i * cellWidth, 0, i == count - 1 ? width() - i * cellWidth : cellWidth - 1, height());
        const auto status = i < segments_.size() ? segments_[i].status : DownloadStatus::Queued;
        QColor color = palette().mid().color();
        if (status == DownloadStatus::Downloading) color = QColor(49, 106, 197);
        if (status == DownloadStatus::Completed) color = QColor(28, 128, 56);
        if (status == DownloadStatus::Failed) color = QColor(176, 40, 40);
        painter.fillRect(cell.adjusted(1, 4, -1, -4), color);
    }
}

QSize SegmentGrid::sizeHint() const
{
    return {240, 28};
}

}
