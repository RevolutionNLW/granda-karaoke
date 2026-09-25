#include "MainWindow.h"

#include "Logging.h"
#include "LyricsView.h"
#include "SongPair.h"

#include <QApplication>
#include <QCloseEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace {

QString formatTime(qint64 ms)
{
    const qint64 seconds = qMax<qint64>(0, ms) / 1000;
    return QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

QPushButton* makeButton(const QString& text, QWidget* parent)
{
    auto* button = new QPushButton(text, parent);
    button->setFocusPolicy(Qt::NoFocus);
    button->setMinimumSize(170, 80);
    QFont font = button->font();
    font.setPointSize(22);
    font.setBold(true);
    button->setFont(font);
    return button;
}

bool isEnterKey(const QKeyEvent* event)
{
    return event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter;
}

} // namespace

MainWindow::MainWindow(KaraokePlayer* player, QWidget* parent)
    : QWidget(parent)
    , m_player(player)
{
    setWindowTitle(QStringLiteral("Frankie's Karaoke Studio"));

    setFocusPolicy(Qt::StrongFocus);
    m_pages = new QStackedWidget(this);
    m_controls = new QWidget(m_pages);
    m_lyrics = new LyricsView(m_pages);
    m_pages->addWidget(m_controls);
    m_pages->addWidget(m_lyrics);
    auto* windowLayout = new QVBoxLayout(this);
    windowLayout->setContentsMargins(0, 0, 0, 0);
    windowLayout->addWidget(m_pages);

    m_songLabel = new QLabel(m_controls);
    m_songLabel->setWordWrap(true);
    m_songLabel->setAlignment(Qt::AlignCenter);
    QFont songFont = m_songLabel->font();
    songFont.setPointSize(26);
    songFont.setBold(true);
    m_songLabel->setFont(songFont);

    m_statusLabel = new QLabel(m_controls);
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setAlignment(Qt::AlignCenter);
    QFont statusFont = m_statusLabel->font();
    statusFont.setPointSize(22);
    m_statusLabel->setFont(statusFont);

    m_hintLabel = new QLabel(m_controls);
    m_hintLabel->setAlignment(Qt::AlignCenter);
    QFont hintFont = m_hintLabel->font();
    hintFont.setPointSize(18);
    m_hintLabel->setFont(hintFont);

    m_openButton = makeButton(QStringLiteral("Open Song"), m_controls);
    m_playButton = makeButton(QStringLiteral("Play"), m_controls);
    m_pauseButton = makeButton(QStringLiteral("Pause"), m_controls);
    m_stopButton = makeButton(QStringLiteral("Stop"), m_controls);

    m_exitButton = new QPushButton(QStringLiteral("Exit"), m_controls);
    m_exitButton->setFocusPolicy(Qt::NoFocus);
    m_exitButton->setMinimumSize(90, 44);
    auto* exitRow = new QHBoxLayout;
    exitRow->addStretch();
    exitRow->addWidget(m_exitButton);

    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(16);
    buttons->addWidget(m_openButton);
    buttons->addWidget(m_playButton);
    buttons->addWidget(m_pauseButton);
    buttons->addWidget(m_stopButton);

    auto* layout = new QVBoxLayout(m_controls);
    layout->setContentsMargins(32, 32, 32, 32);
    layout->setSpacing(20);
    layout->addLayout(exitRow);
    layout->addStretch();
    layout->addWidget(m_songLabel);
    layout->addWidget(m_statusLabel);
    layout->addLayout(buttons);
    layout->addWidget(m_hintLabel);
    layout->addStretch();

    connect(m_exitButton, &QPushButton::clicked, this, &QWidget::close);
    connect(m_openButton, &QPushButton::clicked, this, &MainWindow::chooseSong);
    connect(m_playButton, &QPushButton::clicked, this, &MainWindow::onPlay);
    connect(m_pauseButton, &QPushButton::clicked, this, &MainWindow::onPause);
    connect(m_stopButton, &QPushButton::clicked, this, &MainWindow::onStop);

    connect(m_player, &KaraokePlayer::stateChanged, this, &MainWindow::onStateChanged);
    connect(m_player, &KaraokePlayer::errorOccurred, this, &MainWindow::onError);
    connect(m_player, &KaraokePlayer::positionChanged, this, &MainWindow::updateControls);
    connect(m_player, &KaraokePlayer::frameChanged, m_lyrics, &LyricsView::setFrame);
    connect(m_lyrics, &LyricsView::controlsRequested, this, &MainWindow::hideLyrics);

    m_lyrics->setFrame(m_player->currentFrame());
    onStateChanged(m_player->state());
    setFocus(Qt::OtherFocusReason);
}

MainWindow::~MainWindow() = default;

bool MainWindow::lyricsVisible() const
{
    return m_pages->currentWidget() == m_lyrics;
}

void MainWindow::keyPressEvent(QKeyEvent* event)
{
    if (!QApplication::activeModalWidget()) {
        if (lyricsVisible() && event->key() == Qt::Key_Escape) {
            hideLyrics();
        } else if (!lyricsVisible() && isEnterKey(event)) {
            const auto state = m_player->state();
            if (state == KaraokePlayer::State::Playing || state == KaraokePlayer::State::Paused)
                showLyrics();
        }
    }
    // No other key, including Space, performs an action.
    event->accept();
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    m_player->stop();
    event->accept();
}

void MainWindow::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::ActivationChange && isActiveWindow()
        && QApplication::focusWidget() != this)
        setFocus(Qt::OtherFocusReason);
}

void MainWindow::chooseSong()
{
    QSettings settings;
    QString startDir = settings.value(QStringLiteral("lastSongFolder")).toString();
    if (startDir.isEmpty() || !QFileInfo(startDir).isDir())
        startDir = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);

    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Open Song"), startDir,
        QStringLiteral("Karaoke songs (*.mp3 *.cdg *.MP3 *.CDG)"));
    setFocus(Qt::OtherFocusReason);
    if (path.isEmpty())
        return;  // Cancelled: whatever was playing carries on.

    settings.setValue(QStringLiteral("lastSongFolder"), QFileInfo(path).absolutePath());
    openSong(path);
}

bool MainWindow::openSong(const QString& path)
{
    qCInfo(lcUi) << "Opening" << path;
    hideLyrics();
    m_errorText.clear();

    const SongPairResult result = resolveSongPair(path);
    if (!result.pair.isValid()) {
        qCWarning(lcUi) << "Cannot open song:" << result.error;
        showError(result.error);
        updateControls();
        return false;
    }
    return m_player->load(result.pair);  // load() reports its own errors.
}

void MainWindow::onPlay()
{
    m_errorText.clear();
    m_player->play();
    updateControls();
}

void MainWindow::onPause()
{
    m_player->pause();
}

void MainWindow::onStop()
{
    m_player->stop();
    hideLyrics();
}

void MainWindow::showLyrics()
{
    m_lyrics->setFrame(m_player->currentFrame());
    // Re-entering fullscreen after the user leaves it is the one intentional
    // window-state change: Play/Enter must always fill the screen.
    if (!isFullScreen())
        showFullScreen();
    m_pages->setCurrentWidget(m_lyrics);
    setFocus(Qt::OtherFocusReason);
}

void MainWindow::hideLyrics()
{
    m_pages->setCurrentWidget(m_controls);
    setFocus(Qt::OtherFocusReason);
}

void MainWindow::onStateChanged(KaraokePlayer::State state)
{
    m_errorText.clear();
    m_displaySleepBlocker.setActive(state == KaraokePlayer::State::Playing);
    switch (state) {
    case KaraokePlayer::State::Playing:
        showLyrics();
        break;
    case KaraokePlayer::State::Stopped:
    case KaraokePlayer::State::Finished:
    case KaraokePlayer::State::Error:
    case KaraokePlayer::State::Empty:
        hideLyrics();
        break;
    default:
        break;
    }
    updateControls();
}

void MainWindow::onError(const QString& message)
{
    hideLyrics();
    showError(message);
    updateControls();
}

void MainWindow::showError(const QString& message)
{
    m_errorText = message;
    if (!m_showErrorDialogs)
        return;
    // Non-blocking so playback timers are never run from a nested event loop.
    auto* box = new QMessageBox(QMessageBox::Warning, QStringLiteral("Frankie's Karaoke Studio"),
                                message, QMessageBox::Ok, this);
    box->setAttribute(Qt::WA_DeleteOnClose);
    connect(box, &QDialog::finished, this, [this] { setFocus(Qt::OtherFocusReason); });
    box->open();
}

QString MainWindow::statusText() const
{
    return m_statusLabel->text();
}

void MainWindow::updateControls()
{
    using State = KaraokePlayer::State;
    const State state = m_player->state();
    const bool hasSong = m_player->hasSong();

    m_songLabel->setText(hasSong ? m_player->song().displayName() : QStringLiteral("No song loaded"));

    const QString time = formatTime(m_player->positionMs());
    const QString total = m_player->durationMs() > 0
        ? QStringLiteral(" of %1").arg(formatTime(m_player->durationMs()))
        : QString();

    QString status;
    QString hint;
    switch (state) {
    case State::Empty:
        status = QStringLiteral("Press Open Song to choose a song.");
        break;
    case State::Ready:
        status = QStringLiteral("Ready. Press Play to start.");
        break;
    case State::Playing:
        status = QStringLiteral("Playing  %1%2").arg(time, total);
        hint = QStringLiteral("Press Enter to show the lyrics.");
        break;
    case State::Paused:
        status = QStringLiteral("Paused at %1. Press Resume to carry on.").arg(time);
        hint = QStringLiteral("Press Enter to show the lyrics.");
        break;
    case State::Stopped:
        status = QStringLiteral("Stopped. Press Play to start from the beginning.");
        break;
    case State::Finished:
        status = QStringLiteral("Finished. Press Play to sing it again.");
        break;
    case State::Error:
        status = QStringLiteral("Sorry, this song could not be played.");
        break;
    }
    if (!m_errorText.isEmpty())
        status = m_errorText;

    m_statusLabel->setText(status);
    m_statusLabel->setStyleSheet(m_errorText.isEmpty() && state != State::Error
                                     ? QString()
                                     : QStringLiteral("color: #b00020;"));
    m_hintLabel->setText(hint);

    m_playButton->setText(state == State::Paused ? QStringLiteral("Resume") : QStringLiteral("Play"));
    m_playButton->setEnabled(hasSong && state != State::Playing);
    m_pauseButton->setEnabled(state == State::Playing);
    m_stopButton->setEnabled(state == State::Playing || state == State::Paused);
}
