#pragma once

#include <QColor>
#include <QIcon>
#include <QPushButton>

// Small drawing helpers and custom controls for the application's look.
// Icons are drawn as vectors, so they stay sharp at any display scale and
// need no image files or symbol fonts.
namespace ui {

enum class Glyph { App, Play, Pause, Stop, Search, Gear, Lyrics };

// An icon drawn in the given colour (a dimmer one when disabled).
QIcon glyphIcon(Glyph glyph, const QColor& color);

// A button that shows only its icon. Its text stays set, for tooltips,
// accessibility and tests, but is not painted.
class IconButton : public QPushButton {
    Q_OBJECT

public:
    IconButton(Glyph glyph, const QString& text, QWidget* parent = nullptr);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent* event) override;
};

// A checkable on/off switch. The text stays set (e.g. "Autoplay: On") but only
// the track and knob are painted; put a label beside it.
class ToggleSwitch : public QPushButton {
    Q_OBJECT

public:
    explicit ToggleSwitch(QWidget* parent = nullptr);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent* event) override;
};

} // namespace ui
