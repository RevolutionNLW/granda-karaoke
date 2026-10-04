#include "SongKeyPicker.h"

#include "library/SongKeys.h"
#include "ui/ElidedLabel.h"
#include "ui/Theme.h"

#include <QGridLayout>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QVBoxLayout>

namespace {

QLabel* makeLabel(const QString& text, const char* name, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setObjectName(QString::fromLatin1(name));
    return label;
}

} // namespace

SongKeyPicker::SongKeyPicker(const QString& songText, int manualKeyIndex, int detectedKeyIndex,
                             QWidget* parent)
    : QFrame(parent, Qt::Popup)
{
    setObjectName(QStringLiteral("keyPicker"));
    setAttribute(Qt::WA_DeleteOnClose);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(6);
    layout->addWidget(makeLabel(QStringLiteral("Set Original Song Key"), "keyPickerTitle", this));
    if (!songText.isEmpty()) {
        auto* song = new ElidedLabel(songText, this);
        song->setObjectName(QStringLiteral("keyPickerSong"));
        layout->addWidget(song);
    }
    auto* hint = makeLabel(QStringLiteral("Choose the key this backing track is recorded in "
                                          "before any Key +/- adjustment."),
                           "keyPickerHint", this);
    hint->setWordWrap(true);
    layout->addWidget(hint);

    for (const bool minor : {false, true}) {
        theme::addSpacing(layout, 4);
        layout->addWidget(makeLabel(minor ? QStringLiteral("Minor") : QStringLiteral("Major"),
                                    "keyPickerCaption", this));
        auto* grid = new QGridLayout;
        grid->setHorizontalSpacing(6);
        grid->setVerticalSpacing(6);
        for (int tonic = 0; tonic < 12; ++tonic) {
            const int index = tonic + (minor ? 12 : 0);
            auto* button = new QPushButton(songKeyName(index), this);
            button->setObjectName(QStringLiteral("keyChoice"));
            button->setFocusPolicy(Qt::NoFocus);
            button->setCheckable(true);
            button->setChecked(index == manualKeyIndex);
            // The key heard in the music, when nobody has chosen one: marked
            // quietly so it is easy to confirm, but not shown as chosen.
            button->setProperty("detected", index == detectedKeyIndex && manualKeyIndex < 0);
            theme::setFixedSize(button, 50, 34);
            connect(button, &QPushButton::clicked, this, [this, index] {
                emit keyChosen(index);
                close();
            });
            grid->addWidget(button, tonic / 6, tonic % 6);
            m_keys.append(button);
        }
        layout->addLayout(grid);
    }

    theme::addSpacing(layout, 4);
    auto* footer = new QHBoxLayout;
    footer->setSpacing(10);
    m_detected = makeLabel(detectedKeyIndex >= 0
                               ? QStringLiteral("Detected from the music: %1").arg(songKeyName(detectedKeyIndex))
                               : QString(),
                           "keyPickerHint", this);
    footer->addWidget(m_detected, 1);
    m_clear = new QPushButton(QStringLiteral("Clear Manual Key"), this);
    m_clear->setObjectName(QStringLiteral("ghostButton"));
    m_clear->setFocusPolicy(Qt::NoFocus);
    m_clear->setEnabled(manualKeyIndex >= 0);
    m_clear->setToolTip(QStringLiteral("Go back to the key detected from the music (blank if none)"));
    connect(m_clear, &QPushButton::clicked, this, [this] {
        emit clearRequested();
        close();
    });
    footer->addWidget(m_clear);
    layout->addLayout(footer);

    theme::rescale(this);
}

QPushButton* SongKeyPicker::keyButton(int keyIndex) const
{
    return keyIndex >= 0 && keyIndex < m_keys.size() ? m_keys.at(keyIndex) : nullptr;
}

void SongKeyPicker::popUpAt(QWidget* anchor)
{
    adjustSize();
    const QSize size = sizeHint().expandedTo(minimumSizeHint());
    resize(size);
    QPoint at = anchor->mapToGlobal(QPoint(anchor->width() - size.width(), -size.height() - theme::px(6)));
    QScreen* screen = anchor->screen() ? anchor->screen() : QGuiApplication::primaryScreen();
    if (screen) {
        const QRect room = screen->availableGeometry();
        if (at.y() < room.top())
            at.setY(anchor->mapToGlobal(QPoint(0, anchor->height() + theme::px(6))).y());
        at.setX(std::clamp(at.x(), room.left(), std::max(room.left(), room.right() - size.width())));
        at.setY(std::clamp(at.y(), room.top(), std::max(room.top(), room.bottom() - size.height())));
    }
    move(at);
    show();
}

void SongKeyPicker::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape) {
        close();
        return;
    }
    QFrame::keyPressEvent(event);
}
