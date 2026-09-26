#include "MainWindow.h"

#include "Logging.h"
#include "LibraryController.h"
#include "LibraryView.h"
#include "LyricsView.h"
#include "PlaylistView.h"
#include "SongPair.h"
#include "playlist/PlaylistPlayback.h"
#include "playlist/PlaylistStore.h"

#include <QApplication>
#include <QCloseEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
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
    button->setMinimumSize(115, 80);
    QFont font = button->font();
    font.setPointSize(22);
    font.setBold(true);
    button->setFont(font);
    return button;
}

QPushButton* makeSettingButton(const QString& text, QWidget* parent, int minimumWidth = 100)
{
    auto* button = new QPushButton(text, parent);
    button->setFocusPolicy(Qt::NoFocus);
    button->setMinimumSize(minimumWidth, 64);
    QFont font = button->font();
    font.setPointSize(24);
    font.setBold(true);
    button->setFont(font);
    return button;
}

QLabel* makeSettingLabel(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setAlignment(Qt::AlignCenter);
    label->setMinimumWidth(110);
    QFont font = label->font();
    font.setPointSize(26);
    font.setBold(true);
    label->setFont(font);
    return label;
}

bool isEnterKey(const QKeyEvent* event)
{
    return event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter;
}

} // namespace

MainWindow::MainWindow(KaraokePlayer* player, ISongSettingsStore* settingsStore,
                       LibraryController* libraryController,
                       PlaylistStore* playlistStore, QWidget* parent)
    : QWidget(parent)
    , m_player(player)
    , m_settingsStore(settingsStore)
    , m_libraryController(libraryController)
    , m_playlistStore(playlistStore)
    , m_playlistPlayback(new PlaylistPlayback(playlistStore, this))
{
    Q_ASSERT(m_player);
    Q_ASSERT(m_settingsStore);
    setWindowTitle(QStringLiteral("Frankie's Karaoke Studio"));

    setFocusPolicy(Qt::StrongFocus);
    m_pages = new QStackedWidget(this);
    m_controls = new QWidget(m_pages);
    m_lyrics = new LyricsView(m_pages);
    m_pages->addWidget(m_controls);
    m_pages->addWidget(m_lyrics);
    if (m_libraryController) {
        m_libraryPage = new QWidget(m_pages);
        m_library = new LibraryView(m_libraryController, m_libraryPage);
        m_playlistView = new PlaylistView(m_playlistStore, m_libraryController,
                                           m_playlistPlayback, m_libraryPage);
        auto* libraryLayout = new QHBoxLayout(m_libraryPage);
        libraryLayout->setContentsMargins(0, 0, 0, 0);
        libraryLayout->addWidget(m_library, 3);
        libraryLayout->addWidget(m_playlistView, 2);
        m_pages->addWidget(m_libraryPage);
    }
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

    m_findButton = makeButton(QStringLiteral("Find a Song"), m_controls);
    m_playlistsButton = makeButton(QStringLiteral("Playlists"), m_controls);
    m_openButton = makeButton(QStringLiteral("Open Song"), m_controls);
    m_playButton = makeButton(QStringLiteral("Play"), m_controls);
    m_pauseButton = makeButton(QStringLiteral("Pause"), m_controls);
    m_stopButton = makeButton(QStringLiteral("Stop"), m_controls);

    auto* keyTitle = makeSettingLabel(QStringLiteral("KEY"), m_controls);
    keyTitle->setMinimumWidth(150);
    m_keyDownButton = makeSettingButton(QStringLiteral("\u2212"), m_controls);
    m_keyValueLabel = makeSettingLabel(QStringLiteral("0"), m_controls);
    m_keyUpButton = makeSettingButton(QStringLiteral("+"), m_controls);
    m_keyResetButton = makeSettingButton(QStringLiteral("Reset"), m_controls, 150);
    auto* keyRow = new QHBoxLayout;
    keyRow->setSpacing(12);
    keyRow->addStretch();
    keyRow->addWidget(keyTitle);
    keyRow->addWidget(m_keyDownButton);
    keyRow->addWidget(m_keyValueLabel);
    keyRow->addWidget(m_keyUpButton);
    keyRow->addWidget(m_keyResetButton);
    keyRow->addStretch();

    auto* tempoTitle = makeSettingLabel(QStringLiteral("TEMPO"), m_controls);
    tempoTitle->setMinimumWidth(150);
    m_tempoDownButton = makeSettingButton(QStringLiteral("\u2212"), m_controls);
    m_tempoValueLabel = makeSettingLabel(QStringLiteral("100%"), m_controls);
    m_tempoUpButton = makeSettingButton(QStringLiteral("+"), m_controls);
    m_tempoResetButton = makeSettingButton(QStringLiteral("Reset"), m_controls, 150);
    auto* tempoRow = new QHBoxLayout;
    tempoRow->setSpacing(12);
    tempoRow->addStretch();
    tempoRow->addWidget(tempoTitle);
    tempoRow->addWidget(m_tempoDownButton);
    tempoRow->addWidget(m_tempoValueLabel);
    tempoRow->addWidget(m_tempoUpButton);
    tempoRow->addWidget(m_tempoResetButton);
    tempoRow->addStretch();

    m_exitButton = new QPushButton(QStringLiteral("Exit"), m_controls);
    m_exitButton->setFocusPolicy(Qt::NoFocus);
    m_exitButton->setMinimumSize(90, 44);
    auto* exitRow = new QHBoxLayout;
    exitRow->addStretch();
    exitRow->addWidget(m_exitButton);

    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(10);
    buttons->addWidget(m_findButton);
    buttons->addWidget(m_playlistsButton);
    buttons->addWidget(m_openButton);
    buttons->addWidget(m_playButton);
    buttons->addWidget(m_pauseButton);
    buttons->addWidget(m_stopButton);

    auto* layout = new QVBoxLayout(m_controls);
    layout->setContentsMargins(24, 16, 24, 16);
    layout->setSpacing(10);
    layout->addLayout(exitRow);
    layout->addStretch();
    layout->addWidget(m_songLabel);
    layout->addWidget(m_statusLabel);
    layout->addLayout(buttons);
    layout->addLayout(keyRow);
    layout->addLayout(tempoRow);
    layout->addWidget(m_hintLabel);
    layout->addStretch();

    connect(m_exitButton, &QPushButton::clicked, this, &QWidget::close);
    connect(m_findButton, &QPushButton::clicked, this, &MainWindow::showLibrary);
    connect(m_playlistsButton, &QPushButton::clicked, this, &MainWindow::showLibrary);
    connect(m_openButton, &QPushButton::clicked, this, &MainWindow::chooseSong);
    connect(m_playButton, &QPushButton::clicked, this, &MainWindow::onPlay);
    connect(m_pauseButton, &QPushButton::clicked, this, &MainWindow::onPause);
    connect(m_stopButton, &QPushButton::clicked, this, &MainWindow::onStop);
    connect(m_keyDownButton, &QPushButton::clicked, this, [this] { changeKey(-kKeyStep); });
    connect(m_keyUpButton, &QPushButton::clicked, this, [this] { changeKey(kKeyStep); });
    connect(m_keyResetButton, &QPushButton::clicked, this, [this] {
        m_player->setKeySemitones(0);
        persistCurrentSettings();
        updateControls();
    });
    connect(m_tempoDownButton, &QPushButton::clicked, this, [this] { changeTempo(-kTempoStep); });
    connect(m_tempoUpButton, &QPushButton::clicked, this, [this] { changeTempo(kTempoStep); });
    connect(m_tempoResetButton, &QPushButton::clicked, this, [this] {
        m_player->setTempoPercent(100);
        persistCurrentSettings();
        updateControls();
    });

    connect(m_player, &KaraokePlayer::stateChanged, this, &MainWindow::onStateChanged);
    connect(m_player, &KaraokePlayer::stateChanged,
            m_playlistPlayback, &PlaylistPlayback::onPlayerStateChanged);
    connect(m_player, &KaraokePlayer::errorOccurred, this, &MainWindow::onError);
    connect(m_player, &KaraokePlayer::positionChanged, this, &MainWindow::updateControls);
    connect(m_player, &KaraokePlayer::settingsChanged, this, &MainWindow::updateControls);
    connect(m_player, &KaraokePlayer::frameChanged, m_lyrics, &LyricsView::setFrame);
    connect(m_lyrics, &LyricsView::controlsRequested, this, &MainWindow::hideLyrics);
    if (m_library) {
        connect(m_library, &LibraryView::backRequested, this, &MainWindow::hideLyrics);
        connect(m_library, &LibraryView::singRequested, this, &MainWindow::singLibrarySong);
        connect(m_library, &LibraryView::addRequested,
                m_playlistView, &PlaylistView::addSong);
        connect(m_playlistView, &PlaylistView::displayedPlaylistChanged,
                m_library, &LibraryView::setPlaylistAvailable);
        connect(m_playlistView, &PlaylistView::playRequested,
                this, &MainWindow::playPlaylistItem);
        connect(m_playlistView, &PlaylistView::backRequested,
                this, &MainWindow::hideLyrics);
        connect(m_playlistPlayback, &PlaylistPlayback::autoplayRequested,
                this, [this](PlaylistEntry entry) { playPlaylistItem(entry, true); });
        m_library->setPlaylistAvailable(m_playlistView->displayedPlaylistId() != 0);
    }

    m_lyrics->setFrame(m_player->currentFrame());
    onStateChanged(m_player->state());
    setFocus(Qt::OtherFocusReason);
}

MainWindow::~MainWindow() = default;

bool MainWindow::lyricsVisible() const
{
    return m_pages->currentWidget() == m_lyrics;
}

bool MainWindow::libraryVisible() const
{
    return m_libraryPage && m_pages->currentWidget() == m_libraryPage;
}

void MainWindow::keyPressEvent(QKeyEvent* event)
{
    if (!QApplication::activeModalWidget()) {
        if (libraryVisible()) {
            event->ignore();
            return;
        } else if (lyricsVisible() && event->key() == Qt::Key_Escape) {
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
    m_playlistPlayback->cancelPendingAutoplay();
    m_player->stop();
    event->accept();
}

void MainWindow::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::ActivationChange && isActiveWindow()) {
        if (libraryVisible())
            m_library->searchBox()->setFocus(Qt::OtherFocusReason);
        else if (QApplication::focusWidget() != this)
            setFocus(Qt::OtherFocusReason);
    }
}

void MainWindow::showLibrary()
{
    if (!m_library)
        return;
    m_pages->setCurrentWidget(m_libraryPage);
    m_library->activate();
    if (m_playlistView)
        m_playlistView->activate();
}

void MainWindow::singLibrarySong(qint64 songId)
{
    m_playlistPlayback->cancelPendingAutoplay();
    if (!m_libraryController)
        return;
    if (!m_libraryController->isRootConnected()) {
        m_library->showMessage(QStringLiteral("This song's music drive is not connected."));
        return;
    }
    QString error;
    const PlaybackPaths paths = m_libraryController->playbackPathsFor(songId, &error);
    if (!paths.playable() || !QFileInfo::exists(paths.mp3Path)
        || !QFileInfo::exists(paths.graphicsPath)) {
        qCWarning(lcUi).noquote() << "Catalogue song is missing:" << error << paths.reason;
        m_library->showMessage(
            QStringLiteral("This song can't be found. The library will be checked again."));
        m_libraryController->requestRefreshScan();
        return;
    }
    if (loadSong(paths.mp3Path))
        m_playlistPlayback->clear();
}

void MainWindow::playPlaylistItem(PlaylistEntry entry, bool autoplay)
{
    if (!autoplay)
        m_playlistPlayback->cancelPendingAutoplay();
    const auto unavailable = [this] {
        const QString message = QStringLiteral("This song can't be found right now.");
        if (m_playlistView)
            m_playlistView->showMessage(message);
        m_errorText = message;
        updateControls();
    };
    if (!m_libraryController || !m_playlistStore) {
        unavailable();
        return;
    }
    QString error;
    const PlaylistSongResolution resolution =
        m_libraryController->resolvePlaylistSong(entry, &error);
    if (!resolution) {
        unavailable();
        return;
    }
    if (resolution.updateStoredSongId
        && !m_playlistStore->updateSongId(entry.itemId, resolution.songId, &error)) {
        qCWarning(lcUi).noquote()
            << "Could not cache resolved playlist song ID; continuing playback:" << error;
    }
    const PlaybackPaths paths = m_libraryController->playbackPathsForAny(
        resolution.songId, &error);
    if (!paths.playable() || !QFileInfo::exists(paths.mp3Path)
        || !QFileInfo::exists(paths.graphicsPath)) {
        unavailable();
        return;
    }
    if (!loadSong(paths.mp3Path)) {
        if (autoplay)
            unavailable();
        return;
    }
    if (m_player->state() == KaraokePlayer::State::Error
        || m_player->state() == KaraokePlayer::State::Empty) {
        m_playlistPlayback->clear();
        return;
    }
    m_playlistPlayback->startedFromPlaylist(entry.playlistId, entry.itemId);
    m_player->play();
}

void MainWindow::chooseSong()
{
    m_playlistPlayback->cancelPendingAutoplay();
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
    if (!loadSong(path))
        return false;
    m_playlistPlayback->clear();
    return true;
}

bool MainWindow::loadSong(const QString& path)
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
    const SongPair previousSong = m_player->song();
    const bool hadPreviousSong = m_player->hasSong();
    if (!m_player->load(result.pair)) {
        const bool previousSongKept = hadPreviousSong && m_player->hasSong()
            && m_player->song().mp3Path == previousSong.mp3Path
            && m_player->song().cdgPath == previousSong.cdgPath;
        if (!previousSongKept
            && (m_player->state() == KaraokePlayer::State::Error
                || m_player->state() == KaraokePlayer::State::Empty))
            m_playlistPlayback->clear();
        return false; // load() reports its own errors.
    }

    m_songIdentity = songIdentity(result.pair);
    m_identityWarningLogged = false;
    if (m_songIdentity.isEmpty()) {
        qCWarning(lcUi) << "Song settings cannot be saved because its files could not be fingerprinted";
        m_identityWarningLogged = true;
    }
    const SongSettings settings = m_settingsStore->settingsFor(m_songIdentity).clamped();
    m_player->setKeySemitones(settings.keySemitones);
    m_player->setTempoPercent(settings.tempoPercent);
    updateControls();
    return true;
}

void MainWindow::onPlay()
{
    m_playlistPlayback->cancelPendingAutoplay();
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
    m_playlistPlayback->cancelPendingAutoplay();
    m_player->stop();
    hideLyrics();
}

void MainWindow::changeKey(int delta)
{
    m_player->setKeySemitones(m_player->keySemitones() + delta);
    persistCurrentSettings();
    updateControls();
}

void MainWindow::changeTempo(int delta)
{
    m_player->setTempoPercent(m_player->tempoPercent() + delta);
    persistCurrentSettings();
    updateControls();
}

void MainWindow::persistCurrentSettings()
{
    if (!m_player->hasSong())
        return;
    if (m_songIdentity.isEmpty()) {
        if (!m_identityWarningLogged) {
            qCWarning(lcUi) << "Song settings cannot be saved because its identity is unavailable";
            m_identityWarningLogged = true;
        }
        return;
    }
    m_settingsStore->store(m_songIdentity,
        SongSettings{m_player->keySemitones(), m_player->tempoPercent()}, m_player->song());
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
    if (m_libraryController) {
        m_libraryController->setPlaybackActive(
            state == KaraokePlayer::State::Playing || state == KaraokePlayer::State::Paused);
    }
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

QString MainWindow::songText() const
{
    return m_songLabel->text();
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

    const int key = hasSong ? m_player->keySemitones() : 0;
    const int tempo = hasSong ? m_player->tempoPercent() : 100;
    m_keyValueLabel->setText(key > 0 ? QStringLiteral("+%1").arg(key) : QString::number(key));
    m_tempoValueLabel->setText(QStringLiteral("%1%").arg(tempo));
    m_keyDownButton->setEnabled(hasSong && key > kMinKey);
    m_keyUpButton->setEnabled(hasSong && key < kMaxKey);
    m_keyResetButton->setEnabled(hasSong);
    m_tempoDownButton->setEnabled(hasSong && tempo > kMinTempo);
    m_tempoUpButton->setEnabled(hasSong && tempo < kMaxTempo);
    m_tempoResetButton->setEnabled(hasSong);
}
