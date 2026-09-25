#include "LyricsView.h"

#include <QMouseEvent>
#include <QPainter>

LyricsView::LyricsView(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_OpaquePaintEvent);
    setCursor(Qt::BlankCursor);
    setFocusPolicy(Qt::NoFocus);
}

void LyricsView::setFrame(const QImage& frame)
{
    m_frame = frame;
    if (isVisible())
        update();
}

void LyricsView::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.fillRect(rect(), Qt::black);
    if (m_frame.isNull())
        return;

    // Scale to fit while keeping the CDG aspect ratio. Nearest-neighbour
    // scaling keeps the blocky CDG text sharp.
    const QSize target = m_frame.size().scaled(size(), Qt::KeepAspectRatio);
    const QRect area(QPoint((width() - target.width()) / 2, (height() - target.height()) / 2), target);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    painter.drawImage(area, m_frame);
}

void LyricsView::mousePressEvent(QMouseEvent* event)
{
    event->accept();
    emit controlsRequested();
}
