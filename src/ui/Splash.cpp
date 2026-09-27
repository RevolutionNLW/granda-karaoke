#include "ui/Splash.h"

#include "ui/Controls.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QEvent>
#include <QImage>
#include <QKeyEvent>
#include <QPainter>
#include <QRadialGradient>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace ui {

namespace {

constexpr int kBars = 31;

qreal clamp01(qreal value)
{
    return std::clamp<qreal>(value, 0.0, 1.0);
}

// Starts and ends gently.
qreal ease(qreal value)
{
    value = clamp01(value);
    return value * value * (3.0 - 2.0 * value);
}

// How far through [from, to] the time is, eased: 0 before, 1 after.
qreal between(qreal t, qreal from, qreal to)
{
    return to > from ? ease((t - from) / (to - from)) : (t >= to ? 1.0 : 0.0);
}

const QColor kTitle{0xf5, 0xf1, 0xea};
const QColor kMarkBackground{0x3d, 0x32, 0x23};

} // namespace

SplashOverlay::SplashOverlay(QWidget* window, qint64 msSinceLaunch, int lengthMs)
    : QWidget(window)
    , m_window(window)
    , m_timer(new QTimer(this))
{
    Q_ASSERT(window);
    setObjectName(QStringLiteral("splash"));
    setAccessibleName(QStringLiteral("Frankie's Karaoke Studio"));
    setFocusPolicy(Qt::NoFocus);
    // Opaque until it starts to dissolve, so nothing beneath is redrawn.
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_NoSystemBackground);
    // A slow start gets a shorter splash, so the program is always usable
    // soon after it is opened.
    const qint64 left = kLatestFromLaunchMs - std::max<qint64>(0, msSinceLaunch);
    m_plannedMs = int(std::clamp<qint64>(std::min<qint64>(lengthMs, left), kShortestMs, kLongestMs));
    setGeometry(window->rect());
    raise();
    // Follows the window's size, and skips at any key, click or wheel turn
    // in it.
    qApp->installEventFilter(this);

    m_quietTimer = new QTimer(this);
    m_quietTimer->setSingleShot(true);
    // Never early: a second click inside the double-click time is still
    // part of the same one.
    m_quietTimer->setTimerType(Qt::PreciseTimer);
    connect(m_quietTimer, &QTimer::timeout, this, [this] {
        m_pointerQuiet = true;
        finishIfDrained();
    });
    m_timer->setTimerType(Qt::PreciseTimer);
    m_timer->setInterval(16);
    connect(m_timer, &QTimer::timeout, this, &SplashOverlay::tick);
    m_clock.start();
    m_timer->start();
    show();
}

SplashOverlay::~SplashOverlay()
{
    if (qApp)
        qApp->removeEventFilter(this);
}

void SplashOverlay::dismiss()
{
    finish();
}

void SplashOverlay::skip(QEvent* event)
{
    // Gone from view at once; the rest of the same key press or click (its
    // repeats and release, a double-click's second click, a scroll's later
    // movement) is used up too, so none of it lands on the window now showing.
    m_timer->stop();
    hide();
    // Never kept for long, even if a release never arrives.
    QTimer::singleShot(kLongestDrainMs, this, &SplashOverlay::finish);
    if (event->type() == QEvent::KeyPress) {
        m_drain = Drain::Keyboard;
        m_keyHeld = true;
        m_heldKey = static_cast<QKeyEvent*>(event)->key();
    } else {
        m_drain = Drain::Pointer;
        m_mouseHeld = event->type() != QEvent::Wheel;
        pointerMoved();
    }
}

void SplashOverlay::pointerMoved()
{
    // The gesture is over once the pointer has been still for a
    // double-click's time.
    m_pointerQuiet = false;
    m_quietTimer->start(QApplication::doubleClickInterval());
}

void SplashOverlay::finishIfDrained()
{
    if ((m_drain == Drain::Keyboard && !m_keyHeld)
        || (m_drain == Drain::Pointer && !m_mouseHeld && m_pointerQuiet))
        finish();
}

void SplashOverlay::finish()
{
    if (m_done)
        return;
    m_done = true;
    m_timer->stop();
    qApp->removeEventFilter(this);
    hide();
    emit finished();
    deleteLater();
}

void SplashOverlay::tick()
{
    const qint64 now = m_clock.elapsed();
    if (now >= m_plannedMs) {
        finish();
        return;
    }
    // While it dissolves the window beneath shows through, so all of it is
    // redrawn, as it is while the glow comes up; otherwise only the part
    // that moves.
    if (now >= phase(kDissolveStart)) {
        setAttribute(Qt::WA_OpaquePaintEvent, false);
        update();
    } else if (now <= phase(kGlowInEnd) + 32) {
        update();
    } else {
        update(contentRect());
    }
}

bool SplashOverlay::eventFilter(QObject* watched, QEvent* event)
{
    if (m_done)
        return false;
    if (watched == m_window) {
        if (event->type() == QEvent::Resize)
            setGeometry(m_window->rect());
        else if (event->type() == QEvent::ChildAdded)
            QTimer::singleShot(0, this, [this] { raise(); });
    }
    if (event->type() == QEvent::ApplicationDeactivate && m_drain != Drain::None) {
        // A release may never come once another program has the keyboard.
        finish();
        return false;
    }
    auto* widget = qobject_cast<QWidget*>(watched);
    if (!widget || widget->window() != m_window)
        return false;
    const QEvent::Type type = event->type();
    // The menu a right-click or the Menu key asks for (sent after the press,
    // or on release on Windows) belongs to the input that skipped the splash.
    if (type == QEvent::ContextMenu)
        return true;
    const bool keyboard = type == QEvent::ShortcutOverride || type == QEvent::KeyPress
        || type == QEvent::KeyRelease;
    const bool pointer = type == QEvent::MouseButtonPress || type == QEvent::MouseButtonDblClick
        || type == QEvent::MouseButtonRelease || type == QEvent::Wheel;
    if (!keyboard && !pointer)
        return false;

    if (m_drain == Drain::None) {
        if (type == QEvent::ShortcutOverride) {
            // Claimed, so no shortcut acts on this key either.
            event->accept();
            return true;
        }
        if (type != QEvent::KeyRelease && type != QEvent::MouseButtonRelease)
            skip(event);
        return true;
    }

    // Skipped already: new input of the other kind is the user starting
    // something else, and goes through.
    const bool fresh = (m_drain == Drain::Keyboard && pointer && type != QEvent::MouseButtonRelease)
        || (m_drain == Drain::Pointer && keyboard && type != QEvent::KeyRelease);
    if (fresh) {
        finish();
        return false;
    }
    if (m_drain == Drain::Keyboard) {
        // Every key while the first is down is part of the same press
        // (e.g. a modifier and then its letter).
        if (type == QEvent::ShortcutOverride)
            event->accept();
        const auto* key = type == QEvent::KeyRelease ? static_cast<QKeyEvent*>(event) : nullptr;
        if (key && !key->isAutoRepeat() && key->key() == m_heldKey) {
            m_keyHeld = false;
            finishIfDrained();
        }
        return true;
    }
    if (type == QEvent::MouseButtonPress || type == QEvent::MouseButtonDblClick)
        m_mouseHeld = true;
    else if (type == QEvent::MouseButtonRelease)
        m_mouseHeld = false;
    if (pointer)
        pointerMoved();
    return true;
}

void SplashOverlay::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    rebuildCache();
}

qreal SplashOverlay::unit() const
{
    const qreal fit = std::min(width() / 1280.0, height() / 800.0);
    return std::clamp(fit, 0.6, 1.6) * theme::scalePercent() / 100.0;
}

QRect SplashOverlay::contentRect() const
{
    const qreal u = unit();
    return m_markRect.united(m_title.boundingRect()).united(m_waveRect)
        .adjusted(-8 * u, -16 * u, 8 * u, 16 * u).toAlignedRect();
}

void SplashOverlay::rebuildCache()
{
    if (width() <= 0 || height() <= 0)
        return;
    const qreal u = unit();
    const qreal ratio = devicePixelRatioF();
    const QPointF centre(width() / 2.0, height() / 2.0);

    // The background: charcoal with a soft warm glow behind the name.
    m_background = QPixmap((QSizeF(size()) * ratio).toSize());
    m_background.setDevicePixelRatio(ratio);
    m_background.fill(theme::color::window);
    {
        QPainter p(&m_background);
        p.setRenderHint(QPainter::Antialiasing);
        const QPointF glowCentre = centre - QPointF(0, 26 * u);
        QRadialGradient glow(glowCentre, 420 * u);
        QColor warm = theme::color::accent;
        warm.setAlpha(34);
        glow.setColorAt(0.0, warm);
        warm.setAlpha(14);
        glow.setColorAt(0.45, warm);
        warm.setAlpha(0);
        glow.setColorAt(1.0, warm);
        p.fillRect(QRectF(QPointF(0, 0), QSizeF(size())), glow);
        // A faint grain, so the glow fades smoothly instead of in rings.
        QImage grain(64, 64, QImage::Format_ARGB32_Premultiplied);
        quint32 seed = 0x2545F491u;
        for (int y = 0; y < grain.height(); ++y) {
            auto* line = reinterpret_cast<QRgb*>(grain.scanLine(y));
            for (int x = 0; x < grain.width(); ++x) {
                seed = seed * 1664525u + 1013904223u;
                const int alpha = int(seed >> 29);  // 0-7
                line[x] = (seed >> 28) & 1 ? qPremultiply(qRgba(255, 255, 255, alpha))
                                           : qPremultiply(qRgba(0, 0, 0, alpha));
            }
        }
        QPixmap tile = QPixmap::fromImage(grain);
        tile.setDevicePixelRatio(ratio);
        p.drawTiledPixmap(QRectF(QPointF(0, 0), QSizeF(size())), tile);
    }

    // The name, as a shape (so it can shimmer).
    QFont font = QApplication::font();
    font.setPixelSize(std::max(12, int(std::lround(30 * u))));
    font.setWeight(QFont::DemiBold);
    font.setLetterSpacing(QFont::AbsoluteSpacing, 5.5 * u);
    m_title = QPainterPath();
    // Overlapping strokes within a letter must not cancel each other out.
    m_title.setFillRule(Qt::WindingFill);
    m_title.addText(0, 0, font, QStringLiteral("FRANKIE’S KARAOKE STUDIO"));
    const QRectF titleBox = m_title.boundingRect();
    m_title.translate(centre.x() - titleBox.center().x(), centre.y() + 14 * u - titleBox.center().y());

    // The mark above it: the program's microphone in its gold-brown tile.
    const qreal markSide = 64 * u;
    m_markRect = QRectF(centre.x() - markSide / 2, m_title.boundingRect().top() - 34 * u - markSide,
                        markSide, markSide);
    m_mark = QPixmap((QSizeF(markSide, markSide) * ratio).toSize());
    m_mark.setDevicePixelRatio(ratio);
    m_mark.fill(Qt::transparent);
    {
        QPainter p(&m_mark);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(kMarkBackground);
        p.drawRoundedRect(QRectF(0, 0, markSide, markSide), 15 * u, 15 * u);
        drawGlyph(p, Glyph::App, QRectF(markSide * 0.14, markSide * 0.14, markSide * 0.72, markSide * 0.72),
                  theme::color::accent);
    }

    const qreal barPitch = 7 * u;
    const qreal waveWidth = barPitch * (kBars - 1) + 3 * u;
    m_waveRect = QRectF(centre.x() - waveWidth / 2, m_title.boundingRect().bottom() + 30 * u, waveWidth,
                        26 * u);
}

void SplashOverlay::paintEvent(QPaintEvent*)
{
    if (m_background.isNull())
        rebuildCache();
    const qreal t = m_clock.elapsed();
    const qreal dissolve = 1.0 - between(t, phase(kDissolveStart), phase(kDissolveEnd));
    if (dissolve <= 0.0)
        return;
    const qreal glow = between(t, 0, phase(kGlowInEnd));
    const qreal title = between(t, phase(kTitleInStart), phase(kTitleInEnd));
    const qreal wave = between(t, phase(kWaveInStart), phase(kWaveInEnd));
    const qreal u = unit();

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setOpacity(dissolve);
    // Plain charcoal first; the warm glow comes up over it.
    p.fillRect(rect(), theme::color::window);
    p.setOpacity(dissolve * glow);
    p.drawPixmap(0, 0, m_background);

    // The name and mark leave a little ahead of the background.
    const qreal leaving = dissolve * dissolve;
    p.setOpacity(leaving * glow);
    p.drawPixmap(m_markRect.topLeft() + QPointF(0, (1.0 - glow) * 6 * u), m_mark);

    // The name rises gently into place, then one slow highlight crosses it.
    p.save();
    p.setOpacity(leaving * title);
    p.translate(0, (1.0 - title) * 10 * u);
    const QRectF titleBox = m_title.boundingRect();
    const qreal sweep = (t - phase(kShimmerStart)) / qMax(1, phase(kShimmerEnd) - phase(kShimmerStart));
    if (sweep > 0.0 && sweep < 1.0) {
        const qreal at = -0.2 + ease(sweep) * 1.4;
        QLinearGradient shimmer(titleBox.topLeft(), titleBox.topRight());
        const QColor bright(0xff, 0xe2, 0xb0);
        shimmer.setColorAt(0.0, kTitle);
        shimmer.setColorAt(std::clamp(at - 0.14, 0.0, 1.0), kTitle);
        shimmer.setColorAt(std::clamp(at, 0.0, 1.0), bright);
        shimmer.setColorAt(std::clamp(at + 0.14, 0.0, 1.0), kTitle);
        shimmer.setColorAt(1.0, kTitle);
        p.fillPath(m_title, shimmer);
    } else {
        p.fillPath(m_title, kTitle);
    }
    p.restore();

    // A quiet waveform under the name, strongest in the middle, moving gently.
    p.setOpacity(leaving * wave);
    p.setPen(Qt::NoPen);
    const qreal barWidth = 3 * u;
    const qreal pitch = 7 * u;
    const qreal middle = (kBars - 1) / 2.0;
    for (int i = 0; i < kBars; ++i) {
        const qreal offset = (i - middle) / (kBars * 0.3);
        const qreal envelope = std::exp(-offset * offset);
        const qreal motion = 0.5 + 0.5 * std::sin(t * 0.0046 + i * 0.58) * std::sin(t * 0.0017 + i * 1.7);
        const qreal height = std::max(barWidth, m_waveRect.height() * envelope * (0.18 + 0.82 * motion * wave));
        QColor bar = theme::color::accent;
        bar.setAlphaF(0.25 + 0.6 * envelope);
        p.setBrush(bar);
        const QRectF rect(m_waveRect.left() + i * pitch, m_waveRect.center().y() - height / 2, barWidth, height);
        p.drawRoundedRect(rect, barWidth / 2, barWidth / 2);
    }
}

} // namespace ui
