#include "ui/Controls.h"

#include "ui/Theme.h"

#include <QEvent>
#include <QFrame>
#include <QHeaderView>
#include <QIconEngine>
#include <QPainter>
#include <QPainterPath>
#include <QStyleOptionButton>
#include <QStyleOptionComboBox>
#include <QStylePainter>
#include <QTreeView>

namespace ui {

void drawGlyph(QPainter& p, Glyph glyph, const QRectF& box, const QColor& color)
{
    p.setRenderHint(QPainter::Antialiasing);
    const qreal s = qMin(box.width(), box.height());
    const QRectF r(box.center().x() - s / 2, box.center().y() - s / 2, s, s);
    QPen pen(color, qMax<qreal>(1.2, s * 0.09), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    const auto at = [&r](qreal x, qreal y) { return QPointF(r.left() + x * r.width(), r.top() + y * r.height()); };

    switch (glyph) {
    case Glyph::App: {
        // A stage microphone: a solid head in its cradle, on a short stand.
        p.setBrush(color);
        p.drawRoundedRect(QRectF(at(0.37, 0.08), at(0.63, 0.52)), s * 0.13, s * 0.13);
        p.setBrush(Qt::NoBrush);
        p.drawArc(QRectF(at(0.25, 0.16), at(0.75, 0.66)), 180 * 16, 180 * 16);
        p.drawLine(at(0.50, 0.66), at(0.50, 0.84));
        p.drawLine(at(0.34, 0.86), at(0.66, 0.86));
        break;
    }
    case Glyph::Play: {
        QPainterPath path;
        path.moveTo(at(0.30, 0.20));
        path.lineTo(at(0.80, 0.50));
        path.lineTo(at(0.30, 0.80));
        path.closeSubpath();
        p.setBrush(color);
        p.drawPath(path);
        break;
    }
    case Glyph::Pause:
        p.drawRoundedRect(QRectF(at(0.26, 0.20), at(0.42, 0.80)), s * 0.04, s * 0.04);
        p.drawRoundedRect(QRectF(at(0.58, 0.20), at(0.74, 0.80)), s * 0.04, s * 0.04);
        break;
    case Glyph::Stop:
        p.drawRoundedRect(QRectF(at(0.25, 0.25), at(0.75, 0.75)), s * 0.05, s * 0.05);
        break;
    case Glyph::Search:
        p.drawEllipse(QRectF(at(0.16, 0.16), at(0.66, 0.66)));
        p.drawLine(at(0.60, 0.60), at(0.84, 0.84));
        break;
    case Glyph::Gear: {
        const QPointF c = r.center();
        for (int i = 0; i < 8; ++i) {
            p.save();
            p.translate(c);
            p.rotate(i * 45.0);
            p.drawLine(QPointF(0, -s * 0.26), QPointF(0, -s * 0.38));
            p.restore();
        }
        p.drawEllipse(c, s * 0.25, s * 0.25);
        p.drawEllipse(c, s * 0.09, s * 0.09);
        break;
    }
    case Glyph::Lyrics:
        // A screen with lines of text.
        p.drawRoundedRect(QRectF(at(0.14, 0.20), at(0.86, 0.80)), s * 0.06, s * 0.06);
        p.drawLine(at(0.30, 0.42), at(0.70, 0.42));
        p.drawLine(at(0.36, 0.58), at(0.64, 0.58));
        break;
    }
}

namespace {

class GlyphEngine final : public QIconEngine {
public:
    GlyphEngine(Glyph glyph, QColor color) : m_glyph(glyph), m_color(std::move(color)) {}

    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State) override
    {
        QColor color = m_color;
        if (mode == QIcon::Disabled)
            color.setAlphaF(0.35);
        painter->save();
        drawGlyph(*painter, m_glyph, rect, color);
        painter->restore();
    }

    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override
    {
        return scaledPixmap(size, mode, state, 1.0);
    }

    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state,
                         qreal scale) override
    {
        QPixmap pixmap(size * scale);
        pixmap.setDevicePixelRatio(scale);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        paint(&painter, QRect(QPoint(0, 0), size), mode, state);
        return pixmap;
    }

    QIconEngine* clone() const override { return new GlyphEngine(m_glyph, m_color); }

private:
    Glyph m_glyph;
    QColor m_color;
};

// The piece of header strip above a table's vertical scroll bar; it keeps
// the header's height.
class HeaderCorner final : public QFrame {
public:
    explicit HeaderCorner(QHeaderView* header)
        : m_header(header)
    {
        setObjectName(QStringLiteral("headerCorner"));
        setFixedHeight(header->sizeHint().height());
        header->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (watched == m_header && (event->type() == QEvent::Resize || event->type() == QEvent::Show))
            setFixedHeight(m_header->isVisible() ? m_header->height() : 0);
        return false;
    }

private:
    QHeaderView* m_header;
};

} // namespace

void ComboBox::paintEvent(QPaintEvent* event)
{
    QComboBox::paintEvent(event);
    QStyleOptionComboBox option;
    initStyleOption(&option);
    QPainter painter(this);
    theme::paintChevron(&painter,
                        style()->subControlRect(QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxArrow, this),
                        Qt::DownArrow, isEnabled());
}

void extendHeaderOverScrollBar(QTreeView* view)
{
    view->addScrollBarWidget(new HeaderCorner(view->header()), Qt::AlignTop);
}

QIcon glyphIcon(Glyph glyph, const QColor& color)
{
    return QIcon(new GlyphEngine(glyph, color));
}

IconButton::IconButton(Glyph glyph, const QString& text, QWidget* parent)
    : QPushButton(text, parent)
{
    setFocusPolicy(Qt::NoFocus);
    setToolTip(text);
    setIcon(glyphIcon(glyph, theme::color::text));
    theme::setIconSize(this, 18);
}

QSize IconButton::sizeHint() const
{
    return QSize(theme::px(44), theme::px(34));
}

void IconButton::paintEvent(QPaintEvent*)
{
    QStylePainter painter(this);
    QStyleOptionButton option;
    initStyleOption(&option);
    option.text.clear();
    painter.drawControl(QStyle::CE_PushButton, option);
}

ToggleSwitch::ToggleSwitch(QWidget* parent)
    : QPushButton(parent)
{
    setCheckable(true);
    setFocusPolicy(Qt::NoFocus);
    setCursor(Qt::PointingHandCursor);
    connect(theme::notifier(), &theme::Notifier::changed, this, [this] { updateGeometry(); });
}

QSize ToggleSwitch::sizeHint() const
{
    return QSize(theme::px(40), theme::px(22));
}

void ToggleSwitch::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF track = QRectF(rect()).adjusted(1, 2, -1, -2);
    const qreal radius = track.height() / 2;
    QColor fill = isChecked() ? theme::color::accent : QColor(0x3a, 0x3d, 0x44);
    if (!isEnabled())
        fill.setAlphaF(0.4);
    painter.setPen(Qt::NoPen);
    painter.setBrush(fill);
    painter.drawRoundedRect(track, radius, radius);
    const qreal knob = track.height() - 4;
    const qreal x = isChecked() ? track.right() - 2 - knob : track.left() + 2;
    painter.setBrush(isEnabled() ? QColor(0xf6, 0xf2, 0xea) : QColor(0x8a, 0x86, 0x7f));
    painter.drawEllipse(QRectF(x, track.top() + 2, knob, knob));
}

} // namespace ui
