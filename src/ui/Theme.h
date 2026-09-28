#pragma once

#include <QColor>
#include <QObject>
#include <QString>

class QAbstractButton;
class QApplication;
class QBoxLayout;
class QPainter;
class QRect;
class QWidget;

// The application's look: a dark charcoal desktop theme with a restrained
// warm gold accent. One palette and one style sheet, applied once at start-up.
// Widgets pick their role with an object name (e.g. "playerBar", "caption")
// rather than setting colours or fonts themselves.
namespace theme {

namespace color {
inline const QColor window{0x17, 0x18, 0x1b};
inline const QColor panel{0x1f, 0x22, 0x26};
inline const QColor text{0xec, 0xe8, 0xe1};
inline const QColor textMuted{0x9a, 0x96, 0x8f};
inline const QColor accent{0xea, 0xa2, 0x44};
inline const QColor onAccent{0x22, 0x18, 0x08};
inline const QColor danger{0xe8, 0x6a, 0x5f};
// Rows: the selection in the pane being used, the selection kept in the
// other pane, and the separate "now playing" marker.
inline const QColor selection{0x52, 0x43, 0x30};
inline const QColor selectionText{0xef, 0xab, 0x52};
inline const QColor selectionIdle{0x34, 0x38, 0x3e};
inline const QColor rowBase{0x1f, 0x22, 0x26};
inline const QColor rowAlternate{0x25, 0x28, 0x2c};
inline const QColor secondaryText{0xa9, 0xa5, 0x9e};
} // namespace color

// The CSS colour used for problem messages ("color: ...;").
QString dangerStyle();

void apply(QApplication& app);

// The application's arrow: a thin chevron centred in `rect`.
void paintChevron(QPainter* painter, const QRect& rect, Qt::ArrowType direction, bool enabled);

// ---- Interface scale -----------------------------------------------------
// The whole interface (text, rows, buttons, icons, spacing) can be scaled
// from 80% to 150%, live. Code states sizes at 100%; px() converts one, and
// the helpers below remember a widget's 100% size so rescale() can redo it.
// The karaoke graphics are not affected: they always fill their page.
inline constexpr int kMinScalePercent = 80;
inline constexpr int kMaxScalePercent = 150;
inline constexpr int kScaleStepPercent = 5;

// The size in use: the chosen one, unless the screen cannot hold it.
int scalePercent();
// Applies at once: the style sheet, fonts and every open window are redone.
void setScalePercent(int percent);
// The largest size the screen can hold (the window works it out; Windows
// display scaling enlarges everything as well). The chosen size is kept and
// comes back when the screen allows it again.
int scaleLimitPercent();
void setScaleLimitPercent(int percent);
// The size chosen in Settings (scalePercent() may be smaller).
int chosenScalePercent();
// A 100% length at the current scale. Hairlines (0 and 1) stay as they are.
int px(int base);

// Library and playlist rows: compact or comfortable, plain or striped.
bool compactRows();
void setCompactRows(bool compact);
bool alternateRows();
void setAlternateRows(bool alternate);
// Height added to a row's text line.
int rowPadding();

// Emits changed() after the scale or the row presentation changes.
class Notifier : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
signals:
    void changed();
};
Notifier* notifier();

// Sizes stated at 100% and kept at scale by rescale().
void setFixedSize(QWidget* widget, int width, int height);
void setFixedWidth(QWidget* widget, int width);
void setFixedHeight(QWidget* widget, int height);
void setMinimumWidth(QWidget* widget, int width);
void setMinimumHeight(QWidget* widget, int height);
void setIconSize(QAbstractButton* button, int size);
void setFontPixelSize(QWidget* widget, int pixelSize);
// Space inside a widget, around its text.
void setContentsMargins(QWidget* widget, int left, int top, int right, int bottom);
// A fixed gap in a row or column of widgets.
void addSpacing(QBoxLayout* layout, int base);
// Brings every layout margin and spacing, and the sizes set above, under
// `root` to the current scale (their first-seen values are the 100% ones).
void rescale(QWidget* root);

} // namespace theme
