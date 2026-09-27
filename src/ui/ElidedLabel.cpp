#include "ui/ElidedLabel.h"

#include <QPainter>
#include <QStyle>

QSize ElidedLabel::minimumSizeHint() const
{
    return QSize(0, QLabel::minimumSizeHint().height());
}

void ElidedLabel::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    const QRect area = contentsRect();
    const QString shown = fontMetrics().elidedText(text(), Qt::ElideRight, area.width());
    style()->drawItemText(&painter, area, int(alignment()), palette(), isEnabled(), shown,
                          foregroundRole());
}
