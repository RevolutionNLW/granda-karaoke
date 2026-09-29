#include "ui/Theme.h"

#include <QAbstractButton>
#include <QApplication>
#include <QBoxLayout>
#include <QFont>
#include <QLayout>
#include <QPainter>
#include <QPalette>
#include <QProxyStyle>
#include <QRegularExpression>
#include <QStyleFactory>
#include <QStyleOption>
#include <QWidget>

#include <algorithm>
#include <cmath>

namespace theme {

namespace {

// Colour tokens used by the style sheet below.
//   window   #17181b  main background
//   panel    #1f2226  player bar, library and playlist panes
//   sunken   #141518  now-playing well, search field
//   raised   #26292e  buttons
//   border   #2c2f35  soft separators
//   text     #ece8e1  off-white
//   muted    #8d8982  secondary text and captions
//   accent   #eaa244  warm gold (play button, switch, active tab)
const char* const kStyleSheet = R"(
QToolTip {
    background: #26292e; color: #ece8e1;
    border: 1px solid #3a3d44; padding: 4px 6px;
}

/* ---- Player bar ---------------------------------------------------- */
#playerBar {
    background: #1f2226;
    border-bottom: 1px solid #2a2d32;
}
#brandMark { background: #3d3223; border-radius: 8px; }
#appTitle { font-size: 17px; font-weight: 700; color: #f5f1ea; }
#nowPlaying {
    background: #141518;
    border: 1px solid #2a2d32;
    border-radius: 8px;
}
#nowPlayingCaption { color: #8d8982; font-size: 12px; font-weight: 600; }
#nowPlayingSong { color: #eaa244; font-size: 15px; font-weight: 600; }
#playerStatus { color: #8d8982; font-size: 12px; }
QLabel#controlCaption { color: #b9b5ae; font-size: 14px; }
QLabel#settingValue {
    color: #f5f1ea; font-size: 14px; font-weight: 700;
}
QFrame#barSeparator { background: #2c2f35; max-width: 1px; min-width: 1px; }

/* ---- Buttons ------------------------------------------------------- */
QPushButton, QToolButton {
    background: #26292e;
    color: #ece8e1;
    border: 1px solid #34373d;
    border-radius: 6px;
    padding: 5px 12px;
}
QPushButton:hover, QToolButton:hover { background: #2e3137; border-color: #43474e; }
QPushButton:pressed, QToolButton:pressed { background: #202226; }
QPushButton:disabled, QToolButton:disabled {
    color: #5c5a56; background: #1f2125; border-color: #292b30;
}
QPushButton#transportButton { border-radius: 8px; padding: 0px; }
QPushButton#playPauseButton {
    background: #eaa244; border: 1px solid #f0b05a; border-radius: 8px; padding: 0px;
}
QPushButton#playPauseButton:hover { background: #f0ae55; }
QPushButton#playPauseButton:pressed { background: #d99539; }
QPushButton#playPauseButton:disabled { background: #3a3329; border-color: #3a3329; }
QPushButton#stepButton {
    padding: 0px; min-width: 30px; max-width: 30px; min-height: 28px; max-height: 28px;
    font-size: 16px; font-weight: 600; border-radius: 6px;
}
QPushButton#linkButton {
    background: transparent; border: none; color: #8d8982; padding: 4px 6px;
}
QPushButton#linkButton:hover { color: #ece8e1; }
QPushButton#linkButton:disabled { color: #4c4a47; background: transparent; }
QPushButton#ghostButton, QToolButton#ghostButton {
    background: transparent; border: 1px solid #2f3238; color: #c9c5be;
}
QPushButton#ghostButton:hover, QToolButton#ghostButton:hover {
    background: #26292e; color: #ece8e1;
}
QPushButton#ghostButton:disabled { color: #4c4a47; border-color: #25272c; background: transparent; }
QToolButton#settingsButton { padding: 4px 7px; }
QToolButton#settingsButton::menu-indicator { image: none; width: 0px; }

/* ---- Search / sort bar -------------------------------------------- */
QLineEdit {
    background: #141518;
    color: #ece8e1;
    border: 1px solid #2c2f35;
    border-radius: 8px;
    padding: 5px 9px;
    selection-background-color: #5e4c34;
    selection-color: #fff6e6;
}
QLineEdit:focus { border-color: #5b4c38; }
QLineEdit#librarySearchBox { font-size: 15px; padding: 8px 10px; }
QLineEdit#librarySearchBox:disabled { color: #5c5a56; }

QComboBox {
    background: #26292e;
    color: #ece8e1;
    border: 1px solid #34373d;
    border-radius: 6px;
    padding: 5px 10px;
}
QComboBox:hover { border-color: #43474e; }
QComboBox:disabled { color: #5c5a56; background: #1f2125; border-color: #292b30; }
/* The arrow is a chevron painted by ui::ComboBox. */
QComboBox::drop-down { width: 26px; border: none; background: transparent; }
QComboBox::down-arrow { image: none; }

QComboBox QAbstractItemView {
    background: #212328;
    color: #ece8e1;
    border: 1px solid #34373d;
    selection-background-color: #524330;
    selection-color: #efab52;
    outline: 0;
}
QLabel#caption { color: #8d8982; font-size: 13px; }

/* ---- Library and playlist panes ----------------------------------- */
#pane {
    background: #1f2226;
    border: 1px solid #2a2d32;
    border-radius: 10px;
}
#paneHeader { border-bottom: 1px solid #2a2d32; }
#paneFooter { border-top: 1px solid #2a2d32; }
#paneTitle { font-size: 15px; font-weight: 700; color: #f5f1ea; }
#paneCount { font-size: 15px; color: #8d8982; }
#paneStatus { color: #8d8982; font-size: 12px; }
#paneHint { color: #75726c; font-size: 12px; }

QListView, QListWidget, QTreeView {
    background: #1f2226;
    alternate-background-color: #25282c;
    color: #ece8e1;
    border: none;
    outline: 0;
}
QHeaderView { background: #1f2226; border: none; }
QHeaderView::section {
    background: #1f2226;
    color: #8d8982;
    border: none;
    border-bottom: 1px solid #2a2d32;
    padding: 8px 12px;
    font-size: 11px;
    font-weight: 700;
}

QTabBar { background: transparent; }
QTabBar::tab {
    background: transparent;
    color: #a9a59e;
    border: 1px solid transparent;
    border-radius: 6px;
    padding: 5px 13px;
    margin-right: 4px;
}
QTabBar::tab:hover { background: #24272c; color: #ece8e1; }
QTabBar::tab:selected {
    background: #3a3226; color: #eaa244; border-color: #5e4c33;
}
QTabBar::scroller { width: 52px; }
QTabBar QToolButton {
    background: #26292e; border: 1px solid #34373d; border-radius: 6px; padding: 0px; margin: 2px 1px;
}
QTabBar QToolButton:disabled { background: #1f2125; border-color: #292b30; }

/* Continues a table's column-header strip over its scroll bar. */
QFrame#headerCorner { background: #1f2226; border: none; border-bottom: 1px solid #2a2d32; }

QSplitter::handle { background: transparent; }
QSplitter::handle:hover { background: #2a2d32; }

/* ---- Scroll bars --------------------------------------------------- */
QScrollBar:vertical { background: transparent; width: 11px; margin: 2px 2px 2px 0px; }
QScrollBar:horizontal { background: transparent; height: 11px; margin: 0px 2px 2px 2px; }
QScrollBar::handle:vertical { background: #34373e; border-radius: 4px; min-height: 32px; }
QScrollBar::handle:horizontal { background: #34373e; border-radius: 4px; min-width: 32px; }
QScrollBar::handle:hover { background: #4a4e56; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0px; height: 0px; }
QScrollBar::add-page, QScrollBar::sub-page { background: none; }

/* ---- Menus --------------------------------------------------------- */
QMenu {
    background: #212328;
    color: #ece8e1;
    border: 1px solid #34373d;
    padding: 4px;
}
QMenu::item { padding: 6px 22px 6px 14px; border-radius: 4px; }
QMenu::item:selected { background: #524330; color: #efab52; }
QMenu::item:disabled { color: #5c5a56; }
QMenu::separator { height: 1px; background: #2c2f35; margin: 4px 6px; }

/* ---- Settings ------------------------------------------------------ */
QListWidget#settingsNav {
    background: #1b1d21;
    border: none;
    border-right: 1px solid #2a2d32;
    padding: 12px 8px;
    font-size: 14px;
}
QListWidget#settingsNav::item { padding: 8px 12px; border-radius: 6px; margin: 1px 0px; color: #c9c5be; }
QListWidget#settingsNav::item:hover { background: #26292e; color: #ece8e1; }
QListWidget#settingsNav::item:selected { background: #3a3226; color: #eaa244; }
#settingsTitle { font-size: 20px; font-weight: 700; color: #f5f1ea; }
#settingsSection { font-size: 12px; font-weight: 700; color: #eaa244; }
#settingsHint { color: #8d8982; font-size: 12px; }
#settingsPath { color: #c9c5be; font-size: 12px; background: #16171a; border: 1px solid #2a2d32;
                border-radius: 6px; padding: 6px 8px; }
#aboutTitle { font-size: 22px; font-weight: 700; color: #f5f1ea; }
QScrollArea#settingsPage, QWidget#settingsPageBody { background: transparent; }
QCheckBox, QRadioButton { spacing: 8px; padding: 2px 0px; }
QPushButton#dangerButton { background: transparent; color: #e8847a; border: 1px solid #5a2f2b; }
QPushButton#dangerButton:hover { background: #2e1f1e; }
QTreeWidget#shortcutList { border: 1px solid #2a2d32; border-radius: 8px; }
QTreeWidget#shortcutList::item { padding: 6px 8px; }
QTreeWidget#shortcutList::item:selected { background: #524330; color: #efab52; }
QKeySequenceEdit QLineEdit { padding: 6px 9px; }
QSlider::groove:horizontal { height: 4px; background: #34373d; border-radius: 2px; }
QSlider::sub-page:horizontal { background: #eaa244; border-radius: 2px; }
QSlider::handle:horizontal {
    background: #f5f1ea; width: 16px; height: 16px; margin: -6px 0px; border-radius: 8px;
}
)";

int g_scalePercent = 100;
int g_chosenScalePercent = 100;
int g_scaleLimitPercent = kMaxScalePercent;

} // namespace

void paintChevron(QPainter* painter, const QRect& rect, Qt::ArrowType direction, bool enabled)
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    const qreal side = qMin<qreal>(qMin(rect.width(), rect.height()), px(10));
    const QPointF c = QRectF(rect).center();
    const qreal a = side / 2;
    const qreal b = side / 4;
    // The three points of a '>' pointing right, turned to face `direction`.
    QPointF points[3] = {{-b, -a}, {b, 0}, {-b, a}};
    for (QPointF& point : points) {
        switch (direction) {
        case Qt::DownArrow: point = QPointF(point.y(), point.x()); break;
        case Qt::UpArrow: point = QPointF(point.y(), -point.x()); break;
        case Qt::LeftArrow: point = QPointF(-point.x(), point.y()); break;
        default: break;
        }
        point += c;
    }
    painter->setPen(QPen(enabled ? QColor(0xb9, 0xb5, 0xae) : QColor(0x4c, 0x4a, 0x47),
                         qMax<qreal>(1.4, side * 0.16), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter->setBrush(Qt::NoBrush);
    painter->drawPolyline(points, 3);
    painter->restore();
}

namespace {

// Fusion, with the standard icon and check-box sizes kept at the interface
// scale (the style sheet scales everything else).
class ScaledStyle final : public QProxyStyle {
public:
    ScaledStyle() : QProxyStyle(QStyleFactory::create(QStringLiteral("Fusion"))) {}

    int pixelMetric(PixelMetric metric, const QStyleOption* option,
                    const QWidget* widget) const override
    {
        const int value = QProxyStyle::pixelMetric(metric, option, widget);
        switch (metric) {
        case PM_SmallIconSize:
        case PM_ButtonIconSize:
        case PM_ToolBarIconSize:
        case PM_ListViewIconSize:
        case PM_TabBarIconSize:
        case PM_IndicatorWidth:
        case PM_IndicatorHeight:
        case PM_ExclusiveIndicatorWidth:
        case PM_ExclusiveIndicatorHeight:
        case PM_TabBarScrollButtonWidth:
            return px(value);
        default:
            return value;
        }
    }

    // Check boxes, radio buttons and arrows in the application's own look,
    // drawn as vectors so they stay sharp at every scale.
    void drawPrimitive(PrimitiveElement element, const QStyleOption* option, QPainter* painter,
                       const QWidget* widget) const override
    {
        const bool enabled = option->state & State_Enabled;
        switch (element) {
        case PE_IndicatorCheckBox:
        case PE_IndicatorRadioButton:
            drawIndicator(element == PE_IndicatorRadioButton, option, painter);
            return;
        case PE_IndicatorArrowDown:
            paintChevron(painter, option->rect, Qt::DownArrow, enabled);
            return;
        case PE_IndicatorArrowUp:
            paintChevron(painter, option->rect, Qt::UpArrow, enabled);
            return;
        case PE_IndicatorArrowLeft:
            paintChevron(painter, option->rect, Qt::LeftArrow, enabled);
            return;
        case PE_IndicatorArrowRight:
            paintChevron(painter, option->rect, Qt::RightArrow, enabled);
            return;
        default:
            QProxyStyle::drawPrimitive(element, option, painter, widget);
        }
    }

private:
    static void drawIndicator(bool radio, const QStyleOption* option, QPainter* painter)
    {
        const bool enabled = option->state & State_Enabled;
        const bool on = option->state & (State_On | State_NoChange);
        const bool hover = enabled && (option->state & State_MouseOver);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const qreal side = qMin(option->rect.width(), option->rect.height());
        QRectF box(0, 0, side, side);
        box.moveCenter(QRectF(option->rect).center());
        box.adjust(0.5, 0.5, -0.5, -0.5);
        QColor fill = on ? color::accent : QColor(0x14, 0x15, 0x18);
        QColor edge = on ? color::accent : (hover ? QColor(0x5a, 0x5e, 0x66) : QColor(0x46, 0x4a, 0x52));
        if (!enabled) {
            fill = on ? QColor(0x4a, 0x40, 0x31) : QColor(0x1a, 0x1b, 0x1e);
            edge = on ? QColor(0x4a, 0x40, 0x31) : QColor(0x2e, 0x30, 0x35);
        }
        const QColor mark = enabled ? color::onAccent : QColor(0x8a, 0x86, 0x7f);
        painter->setPen(QPen(edge, 1.0));
        painter->setBrush(fill);
        if (radio) {
            painter->drawEllipse(box);
            if (on) {
                painter->setPen(Qt::NoPen);
                painter->setBrush(mark);
                const qreal dot = side * 0.18;
                painter->drawEllipse(box.center(), dot, dot);
            }
        } else {
            painter->drawRoundedRect(box, side * 0.22, side * 0.22);
            const QPen stroke(mark, qMax<qreal>(1.5, side * 0.13), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            if (option->state & State_NoChange) {
                painter->setPen(stroke);
                painter->drawLine(QPointF(box.left() + side * 0.28, box.center().y()),
                                  QPointF(box.right() - side * 0.28, box.center().y()));
            } else if (on) {
                painter->setPen(stroke);
                painter->setBrush(Qt::NoBrush);
                const QPointF points[] = {
                    {box.left() + side * 0.26, box.top() + side * 0.52},
                    {box.left() + side * 0.43, box.top() + side * 0.68},
                    {box.left() + side * 0.74, box.top() + side * 0.34},
                };
                painter->drawPolyline(points, 3);
            }
        }
        painter->restore();
    }
};
bool g_compactRows = false;
bool g_alternateRows = true;

// The style sheet with every length of 2px or more at the current scale.
QString scaledStyleSheet()
{
    QString sheet = QString::fromUtf8(kStyleSheet);
    static const QRegularExpression length(QStringLiteral("(\\d+)px"));
    QString result;
    result.reserve(sheet.size());
    qsizetype last = 0;
    auto it = length.globalMatch(sheet);
    while (it.hasNext()) {
        const auto match = it.next();
        result += sheet.mid(last, match.capturedStart() - last);
        result += QString::number(px(match.captured(1).toInt())) + QStringLiteral("px");
        last = match.capturedEnd();
    }
    result += sheet.mid(last);
    return result;
}

void applyFontAndStyleSheet(QApplication& app)
{
    QFont font = app.font();
    font.setPixelSize(px(14));
    app.setFont(font);
    app.setStyleSheet(scaledStyleSheet());
}

const char* const kFixed = "fksFixedSize";
const char* const kMinimum = "fksMinimumSize";
const char* const kFontPx = "fksFontPixelSize";
const char* const kIcon = "fksIconSize";
const char* const kMargins = "fksLayoutMargins";
const char* const kSpacing = "fksLayoutSpacing";
const char* const kWidgetMargins = "fksWidgetMargins";

QSize scaled(const QSize& base)
{
    return QSize(base.width() < 0 ? -1 : px(base.width()),
                 base.height() < 0 ? -1 : px(base.height()));
}

void applyFixed(QWidget* widget)
{
    const QSize size = scaled(widget->property(kFixed).toSize());
    if (size.width() >= 0)
        widget->setFixedWidth(size.width());
    if (size.height() >= 0)
        widget->setFixedHeight(size.height());
}

void mergeSize(QWidget* widget, const char* name, int width, int height)
{
    QSize size = widget->property(name).isValid() ? widget->property(name).toSize() : QSize(-1, -1);
    if (width >= 0)
        size.setWidth(width);
    if (height >= 0)
        size.setHeight(height);
    widget->setProperty(name, size);
}

void rescaleLayout(QLayout* layout)
{
    if (!layout->property(kMargins).isValid()) {
        layout->setProperty(kMargins, QVariant::fromValue(layout->contentsMargins()));
        layout->setProperty(kSpacing, layout->spacing());
    }
    const QMargins base = layout->property(kMargins).value<QMargins>();
    layout->setContentsMargins(px(base.left()), px(base.top()), px(base.right()), px(base.bottom()));
    const int spacing = layout->property(kSpacing).toInt();
    if (spacing >= 0)
        layout->setSpacing(px(spacing));
}

} // namespace

int scalePercent()
{
    return g_scalePercent;
}

int px(int base)
{
    if (base <= 1 || g_scalePercent == 100)
        return base;
    return int(std::lround(base * g_scalePercent / 100.0));
}

Notifier* notifier()
{
    static Notifier* instance = new Notifier(qApp);
    return instance;
}

int chosenScalePercent()
{
    return g_chosenScalePercent;
}

int scaleLimitPercent()
{
    return g_scaleLimitPercent;
}

void setScaleLimitPercent(int percent)
{
    percent = std::clamp(percent, kMinScalePercent, kMaxScalePercent);
    if (percent == g_scaleLimitPercent)
        return;
    const int before = g_scalePercent;
    g_scaleLimitPercent = percent;
    setScalePercent(g_chosenScalePercent);
    if (g_scalePercent == before)
        emit notifier()->changed();  // the size is the same, but Settings says why
}

void setScalePercent(int percent)
{
    g_chosenScalePercent = std::clamp(percent, kMinScalePercent, kMaxScalePercent);
    percent = std::min(g_chosenScalePercent, g_scaleLimitPercent);
    if (percent == g_scalePercent)
        return;
    g_scalePercent = percent;
    if (auto* app = qobject_cast<QApplication*>(QCoreApplication::instance())) {
        applyFontAndStyleSheet(*app);
        for (QWidget* window : QApplication::topLevelWidgets()) {
            rescale(window);
            // Settle the new sizes now, so the window's smallest size is
            // right straight away.
            if (window->layout())
                window->layout()->activate();
        }
    }
    emit notifier()->changed();
}

bool compactRows()
{
    return g_compactRows;
}

void setCompactRows(bool compact)
{
    if (compact == g_compactRows)
        return;
    g_compactRows = compact;
    emit notifier()->changed();
}

bool alternateRows()
{
    return g_alternateRows;
}

void setAlternateRows(bool alternate)
{
    if (alternate == g_alternateRows)
        return;
    g_alternateRows = alternate;
    emit notifier()->changed();
}

int rowPadding()
{
    return px(g_compactRows ? 10 : 17);
}

void setFixedSize(QWidget* widget, int width, int height)
{
    mergeSize(widget, kFixed, width, height);
    applyFixed(widget);
}

void setFixedWidth(QWidget* widget, int width)
{
    setFixedSize(widget, width, -1);
}

void setFixedHeight(QWidget* widget, int height)
{
    setFixedSize(widget, -1, height);
}

void setMinimumWidth(QWidget* widget, int width)
{
    mergeSize(widget, kMinimum, width, -1);
    widget->setMinimumWidth(px(width));
}

void setMinimumHeight(QWidget* widget, int height)
{
    mergeSize(widget, kMinimum, -1, height);
    widget->setMinimumHeight(px(height));
}

void setIconSize(QAbstractButton* button, int size)
{
    button->setProperty(kIcon, size);
    button->setIconSize(QSize(px(size), px(size)));
}

void setFontPixelSize(QWidget* widget, int pixelSize)
{
    widget->setProperty(kFontPx, pixelSize);
    QFont font = widget->font();
    font.setPixelSize(px(pixelSize));
    widget->setFont(font);
}

void setContentsMargins(QWidget* widget, int left, int top, int right, int bottom)
{
    widget->setProperty(kWidgetMargins, QVariant::fromValue(QMargins(left, top, right, bottom)));
    widget->setContentsMargins(px(left), px(top), px(right), px(bottom));
}

void addSpacing(QBoxLayout* layout, int base)
{
    auto* gap = new QWidget;
    const bool across = layout->direction() == QBoxLayout::LeftToRight
        || layout->direction() == QBoxLayout::RightToLeft;
    layout->addWidget(gap);
    if (across)
        setFixedSize(gap, base, 0);
    else
        setFixedSize(gap, 0, base);
}

void rescale(QWidget* root)
{
    QList<QWidget*> widgets = root->findChildren<QWidget*>();
    widgets.prepend(root);
    for (QWidget* widget : std::as_const(widgets)) {
        if (QLayout* layout = widget->layout()) {
            rescaleLayout(layout);
            for (QLayout* inner : layout->findChildren<QLayout*>())
                rescaleLayout(inner);
        }
        if (widget->property(kFixed).isValid())
            applyFixed(widget);
        if (widget->property(kMinimum).isValid()) {
            const QSize size = scaled(widget->property(kMinimum).toSize());
            if (size.width() >= 0)
                widget->setMinimumWidth(size.width());
            if (size.height() >= 0)
                widget->setMinimumHeight(size.height());
        }
        if (widget->property(kWidgetMargins).isValid()) {
            const QMargins base = widget->property(kWidgetMargins).value<QMargins>();
            widget->setContentsMargins(px(base.left()), px(base.top()), px(base.right()),
                                       px(base.bottom()));
        }
        if (widget->property(kFontPx).isValid()) {
            QFont font = widget->font();
            font.setPixelSize(px(widget->property(kFontPx).toInt()));
            widget->setFont(font);
        }
        if (widget->property(kIcon).isValid()) {
            if (auto* button = qobject_cast<QAbstractButton*>(widget)) {
                const int size = px(button->property(kIcon).toInt());
                button->setIconSize(QSize(size, size));
            }
        }
    }
}

QString dangerStyle()
{
    return QStringLiteral("color: %1;").arg(color::danger.name());
}

void apply(QApplication& app)
{
    // Fusion draws the same on Windows and macOS and follows the palette and
    // style sheet faithfully; the native styles do not.
    app.setStyle(new ScaledStyle);

    const QColor raised(0x26, 0x29, 0x2e);
    const QColor base = color::rowBase;
    const QColor disabled(0x5c, 0x5a, 0x56);
    QPalette palette;
    palette.setColor(QPalette::Window, color::window);
    palette.setColor(QPalette::WindowText, color::text);
    palette.setColor(QPalette::Base, base);
    palette.setColor(QPalette::AlternateBase, color::rowAlternate);
    palette.setColor(QPalette::Text, color::text);
    palette.setColor(QPalette::PlaceholderText, QColor(0x77, 0x74, 0x6e));
    palette.setColor(QPalette::Button, raised);
    palette.setColor(QPalette::ButtonText, color::text);
    palette.setColor(QPalette::BrightText, Qt::white);
    palette.setColor(QPalette::Highlight, color::selection);
    palette.setColor(QPalette::HighlightedText, color::selectionText);
    palette.setColor(QPalette::Mid, color::textMuted);
    palette.setColor(QPalette::Dark, QColor(0x12, 0x13, 0x15));
    palette.setColor(QPalette::Light, QColor(0x3a, 0x3d, 0x44));
    palette.setColor(QPalette::Shadow, Qt::black);
    palette.setColor(QPalette::ToolTipBase, raised);
    palette.setColor(QPalette::ToolTipText, color::text);
    palette.setColor(QPalette::Link, color::accent);
    palette.setColor(QPalette::Accent, color::accent);
    for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        palette.setColor(QPalette::Disabled, role, disabled);
    app.setPalette(palette);

    applyFontAndStyleSheet(app);
}

} // namespace theme
