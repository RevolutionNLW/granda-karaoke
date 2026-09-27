#pragma once

#include <QElapsedTimer>
#include <QPainterPath>
#include <QPixmap>
#include <QWidget>

class QTimer;

namespace ui {

// The start-up splash: the program's name on the dark background with a warm
// glow and a small moving waveform, covering the main window while it first
// appears, then dissolving into it.
//
// It never holds the program up: the window underneath is already built and
// working, and the splash only sits on top of it. It plays for about 3.5
// seconds (glow and microphone, then the name, a slow shimmer, and a
// dissolve), is cut shorter after a slow start so it ends by
// kLatestFromLaunchMs after the program was started, and never lasts longer
// than kLongestMs. Any key, click or wheel turn removes it at once (that input
// is used up, so it never also acts on the window). It is a child of the
// window, so it goes whenever the window does.
class SplashOverlay : public QWidget {
    Q_OBJECT

public:
    static constexpr int kNormalMs = 3500;
    static constexpr int kShortestMs = 1500;
    static constexpr int kLongestMs = 4000;
    static constexpr int kLatestFromLaunchMs = 4500;
    // After a skip, the longest the rest of that key press or click is held
    // back from the window.
    static constexpr int kLongestDrainMs = 2000;

    // `msSinceLaunch`: how long the program took to get this far. `lengthMs`:
    // how long it plays when nothing cuts it short (kShortestMs-kLongestMs).
    explicit SplashOverlay(QWidget* window, qint64 msSinceLaunch = 0, int lengthMs = kNormalMs);
    ~SplashOverlay() override;

    // How long it shows for, dissolve included.
    int plannedMs() const { return m_plannedMs; }
    // When the name is fully shown and the dissolve begins (ms from the start).
    int titleShownMs() const { return phase(kTitleInEnd); }
    int dissolveStartMs() const { return phase(kDissolveStart); }

public slots:
    // Goes at once.
    void dismiss();

signals:
    void finished();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    // The timeline at kNormalMs; a longer or shorter splash keeps its shape.
    static constexpr int kGlowInEnd = 500;
    static constexpr int kTitleInStart = 400;
    static constexpr int kTitleInEnd = 1200;
    static constexpr int kWaveInStart = 500;
    static constexpr int kWaveInEnd = 1300;
    static constexpr int kShimmerStart = 1000;
    static constexpr int kShimmerEnd = 2600;
    static constexpr int kDissolveStart = 2700;
    static constexpr int kDissolveEnd = 3450;
    int phase(int atNormal) const { return int(qint64(atNormal) * m_plannedMs / kNormalMs); }

    void tick();
    void skip(QEvent* event);
    void pointerMoved();
    void finishIfDrained();
    void finish();
    qreal unit() const;
    void rebuildCache();
    QRect contentRect() const;

    QWidget* m_window;
    QTimer* m_timer;
    QElapsedTimer m_clock;
    int m_plannedMs;
    bool m_done = false;
    // Hidden by a key, click or scroll whose rest is still being used up.
    enum class Drain { None, Keyboard, Pointer };
    Drain m_drain = Drain::None;
    bool m_keyHeld = false;
    int m_heldKey = 0;
    bool m_mouseHeld = false;
    bool m_pointerQuiet = false;
    QTimer* m_quietTimer = nullptr;
    QPixmap m_background;
    QPixmap m_mark;
    QPainterPath m_title;
    QRectF m_markRect;
    QRectF m_waveRect;
};

} // namespace ui
