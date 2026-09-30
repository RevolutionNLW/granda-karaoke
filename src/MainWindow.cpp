#include "MainWindow.h"

#include "Logging.h"
#include "LibraryController.h"
#include "LibraryResultsModel.h"
#include "LibraryView.h"
#include "MetadataReviewDialog.h"
#include "LyricsView.h"
#include "PlaylistView.h"
#include "SongPair.h"
#include "playlist/PlaylistPlayback.h"
#include "playlist/PlaylistStore.h"
#include "AppPreferences.h"
#include "SettingsDialog.h"
#include "library/SongKeys.h"
#include "ui/Controls.h"
#include "ui/Shortcuts.h"
#include "ui/ElidedLabel.h"
#include "ui/Theme.h"

#include <QAbstractButton>
#include <QApplication>
#include <QElapsedTimer>
#include <QCloseEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QAction>
#include <QComboBox>
#include <QListWidget>
#include <QTreeView>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QStandardPaths>
#include <QStackedWidget>
#include <QTimer>
#include <QScopedValueRollback>

#include <algorithm>
#include <QScreen>
#include <QWindow>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

QString formatTime(qint64 ms)
{
    const qint64 seconds = qMax<qint64>(0, ms) / 1000;
    return QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

QPushButton* makeButton(const QString& text, QWidget* parent,
                        const QString& objectName = {})
{
    auto* button = new QPushButton(text, parent);
    button->setFocusPolicy(Qt::NoFocus);
    if (!objectName.isEmpty())
        button->setObjectName(objectName);
    return button;
}

QLabel* makeLabel(const QString& text, QWidget* parent, const QString& objectName)
{
    auto* label = new QLabel(text, parent);
    label->setObjectName(objectName);
    return label;
}

QLabel* makeElidedLabel(QWidget* parent, const QString& objectName)
{
    auto* label = new ElidedLabel(parent);
    label->setObjectName(objectName);
    return label;
}

QFrame* makeSeparator(QWidget* parent)
{
    auto* line = new QFrame(parent);
    line->setObjectName(QStringLiteral("barSeparator"));
    line->setFixedWidth(1);
    theme::setMinimumHeight(line, 22);
    return line;
}

bool isEnterKey(const QKeyEvent* event)
{
    return event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter;
}

} // namespace

MainWindow::MainWindow(KaraokePlayer* player, ISongSettingsStore* settingsStore,
                       LibraryController* libraryController,
                       PlaylistStore* playlistStore, AppPreferences* preferences,
                       QWidget* parent)
    : QWidget(parent)
    , m_ownPreferences(preferences ? nullptr : std::make_unique<AppPreferences>())
    , m_preferences(preferences ? preferences : m_ownPreferences.get())
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
    m_home = new QWidget(m_pages);
    m_home->setObjectName(QStringLiteral("home"));
    m_lyrics = new LyricsView(m_pages);
    m_pages->addWidget(m_home);
    m_pages->addWidget(m_lyrics);
    auto* windowLayout = new QVBoxLayout(this);
    windowLayout->setContentsMargins(0, 0, 0, 0);
    windowLayout->addWidget(m_pages);

    if (m_libraryController) {
        m_library = new LibraryView(m_libraryController, m_home);
        m_playlistView = new PlaylistView(m_playlistStore, m_libraryController,
                                          m_playlistPlayback, m_home);
    }

    auto* homeLayout = new QVBoxLayout(m_home);
    homeLayout->setContentsMargins(0, 0, 0, 0);
    homeLayout->setSpacing(0);
    homeLayout->addWidget(buildPlayerBar());
    if (m_library) {
        auto* searchRow = new QHBoxLayout;
        searchRow->setContentsMargins(16, 12, 16, 12);
        searchRow->addWidget(m_library->takeSearchBar());
        homeLayout->addLayout(searchRow);

        // Library and playlists side by side (about 60/40); the divider can
        // be dragged, and each side scrolls on its own.
        auto* workspace = new QSplitter(Qt::Horizontal, m_home);
        workspace->setObjectName(QStringLiteral("workspace"));
        workspace->setChildrenCollapsible(false);
        workspace->setHandleWidth(theme::px(12));
        theme::setMinimumWidth(m_library, 380);
        workspace->addWidget(m_library);
        workspace->addWidget(m_playlistView);
        workspace->setStretchFactor(0, 3);
        workspace->setStretchFactor(1, 2);
        workspace->setSizes({600, 400});
        // Enough room for several rows on each side at the smallest size.
        theme::setMinimumHeight(workspace, 270);
        connect(theme::notifier(), &theme::Notifier::changed, workspace, [workspace] {
            workspace->setHandleWidth(theme::px(12));
        });
        auto* workspaceRow = new QHBoxLayout;
        workspaceRow->setContentsMargins(16, 0, 16, 16);
        workspaceRow->addWidget(workspace);
        homeLayout->addLayout(workspaceRow, 1);
    } else {
        homeLayout->addStretch();
    }

    connect(m_lyricsButton, &QPushButton::clicked, this, [this] {
        if (songActive())
            showLyrics();
    });
    connect(m_settingsButton, &QToolButton::clicked, this, [this] { openSettings(); });
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
    connect(m_player, &KaraokePlayer::positionChanged, this, [this](qint64 positionMs) {
        if (!m_playStartPending || positionMs <= 0
            || m_player->state() != KaraokePlayer::State::Playing)
            return;
        m_playStartPending = false;
        if (!m_previewing && m_libraryController)
            m_libraryController->recordPlay(m_playSongId, m_songIdentity, m_player->song().mp3Path,
                                            m_player->song().cdgPath);
    });
    connect(m_player, &KaraokePlayer::settingsChanged, this, &MainWindow::updateControls);
    connect(m_player, &KaraokePlayer::frameChanged, m_lyrics, &LyricsView::setFrame);
    connect(m_lyrics, &LyricsView::controlsRequested, this, &MainWindow::hideLyrics);
    if (m_library) {
        connect(m_library, &LibraryView::singRequested, this, &MainWindow::singLibrarySong);
        connect(m_library, &LibraryView::addRequested,
                m_playlistView, &PlaylistView::addSong);
        connect(m_playlistView, &PlaylistView::displayedPlaylistChanged,
                m_library, &LibraryView::setPlaylistAvailable);
        connect(m_playlistView, &PlaylistView::playRequested,
                this, &MainWindow::playPlaylistItem);
        // Escape in the playlist returns to the search box.
        connect(m_playlistView, &PlaylistView::backRequested, this, [this] {
            m_library->searchBox()->setFocus(Qt::OtherFocusReason);
        });
        connect(m_playlistPlayback, &PlaylistPlayback::autoplayRequested,
                this, [this](PlaylistEntry entry) { playPlaylistItem(entry, true); });
        m_library->setPlaylistAvailable(m_playlistView->displayedPlaylistId() != 0);
        // Only the pane in use shows its selection in gold: the one last
        // clicked, or holding the keyboard.
        connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* now) {
            if (now && m_home->isAncestorOf(now))
                setPlaylistInUse(m_playlistView->isAncestorOf(now));
        });
        m_library->resultsList()->viewport()->installEventFilter(this);
        m_playlistView->itemList()->viewport()->installEventFilter(this);
        connect(m_playlistView, &PlaylistView::interacted, this, [this] { setPlaylistInUse(true); });
        connect(m_library, &LibraryView::interacted, this, [this] { setPlaylistInUse(false); });
        // Now Playing follows name corrections for the song loaded.
        const auto retitle = [this] {
            m_titlePath.clear();
            m_titleSongId = -1;
            updateControls();
        };
        connect(m_libraryController, &LibraryController::catalogueChanged, this, retitle);
        connect(m_libraryController, &LibraryController::songKeysChanged, this, [this] {
            m_songKeyForId = -1;
            updateControls();
        });
        connect(m_libraryController, &LibraryController::libraryReady, this, retitle);
        // Enter is decided here first (see eventFilter).
        m_library->searchBox()->installEventFilter(this);
        m_playlistView->itemList()->installEventFilter(this);
        if (m_libraryController->hasActiveRoot())
            m_library->refreshSearch();
    }

    m_lyrics->setFrame(m_player->currentFrame());
    onStateChanged(m_player->state());
    installShortcuts();
    // Sound: the volume always, the chosen output only if it is to be kept.
    m_player->setVolumePercent(m_preferences->number(pref::Volume, 100));
    if (m_preferences->flag(pref::RememberAudioOutput, true))
        m_player->setAudioOutput(m_preferences->text(pref::AudioOutput));
    else
        m_preferences->reset(pref::AudioOutput);  // this session starts on the usual output
    connect(m_preferences, &AppPreferences::changed, this, &MainWindow::applyPreference);
    for (const QString& key : {pref::ShowLabelColumn, pref::ShowPlaysColumn, pref::ShowKeyColumn,
                               pref::ConfirmRemoveSong, pref::AnalyseSongKeys})
        applyPreference(key);
    // Everything above is stated at 100%: bring it to the interface scale.
    theme::rescale(this);
    focusHome();
}

QWidget* MainWindow::buildPlayerBar()
{
    auto* bar = new QFrame(m_home);
    bar->setObjectName(QStringLiteral("playerBar"));

    // Top row: the brand and what is playing.
    auto* brandMark = new QLabel(bar);
    brandMark->setObjectName(QStringLiteral("brandMark"));
    theme::setFixedSize(brandMark, 34, 34);
    brandMark->setAlignment(Qt::AlignCenter);
    const auto drawBrand = [this, brandMark] {
        brandMark->setPixmap(ui::glyphIcon(ui::Glyph::App, theme::color::accent)
                                 .pixmap(QSize(theme::px(22), theme::px(22)), devicePixelRatioF()));
    };
    drawBrand();
    connect(theme::notifier(), &theme::Notifier::changed, brandMark, drawBrand);
    auto* appTitle = makeLabel(QStringLiteral("Frankie's Karaoke Studio"), bar,
                               QStringLiteral("appTitle"));
    auto* nowPlaying = new QFrame(bar);
    nowPlaying->setObjectName(QStringLiteral("nowPlaying"));
    auto* nowCaption = makeLabel(QStringLiteral("NOW PLAYING:"), nowPlaying,
                                 QStringLiteral("nowPlayingCaption"));
    // Long names are shortened with "..." rather than widening the window.
    m_songLabel = makeElidedLabel(nowPlaying, QStringLiteral("nowPlayingSong"));
    m_statusLabel = makeElidedLabel(nowPlaying, QStringLiteral("playerStatus"));
    m_statusLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    auto* nowLayout = new QHBoxLayout(nowPlaying);
    nowLayout->setContentsMargins(14, 7, 14, 7);
    nowLayout->setSpacing(8);
    nowLayout->addWidget(nowCaption);
    nowLayout->addWidget(m_songLabel, 3);
    nowLayout->addWidget(m_statusLabel, 2);

    auto* topRow = new QHBoxLayout;
    topRow->setSpacing(10);
    topRow->addWidget(brandMark);
    topRow->addWidget(appTitle);
    theme::addSpacing(topRow, 12);
    topRow->addWidget(nowPlaying, 1);
    theme::addSpacing(topRow, 4);

    // Bottom row: transport, Autoplay, Key and Tempo, then page buttons.
    // Play and Pause share one place: only the one that applies is shown.
    m_playButton = new ui::IconButton(ui::Glyph::Play, QStringLiteral("Play"), bar);
    m_playButton->setObjectName(QStringLiteral("playPauseButton"));
    m_playButton->setIcon(ui::glyphIcon(ui::Glyph::Play, theme::color::onAccent));
    m_pauseButton = new ui::IconButton(ui::Glyph::Pause, QStringLiteral("Pause"), bar);
    m_pauseButton->setObjectName(QStringLiteral("playPauseButton"));
    m_pauseButton->setIcon(ui::glyphIcon(ui::Glyph::Pause, theme::color::onAccent));
    m_stopButton = new ui::IconButton(ui::Glyph::Stop, QStringLiteral("Stop"), bar);
    m_stopButton->setObjectName(QStringLiteral("transportButton"));
    for (QPushButton* button : {m_playButton, m_pauseButton, m_stopButton})
        theme::setFixedSize(button, 48, 38);

    const auto makeStepper = [bar](const QString& title, QPushButton*& down, QLabel*& value,
                                   QPushButton*& up, QPushButton*& reset) {
        auto* row = new QHBoxLayout;
        row->setSpacing(6);
        row->addWidget(makeLabel(title, bar, QStringLiteral("controlCaption")));
        down = makeButton(QStringLiteral("\u2212"), bar, QStringLiteral("stepButton"));
        value = makeLabel(QString(), bar, QStringLiteral("settingValue"));
        value->setAlignment(Qt::AlignCenter);
        QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        mono.setBold(true);
        value->setFont(mono);
        theme::setFontPixelSize(value, 14);
        up = makeButton(QStringLiteral("+"), bar, QStringLiteral("stepButton"));
        reset = makeButton(QStringLiteral("Reset"), bar, QStringLiteral("linkButton"));
        row->addWidget(down);
        row->addWidget(value);
        row->addWidget(up);
        row->addWidget(reset);
        return row;
    };
    QHBoxLayout* keyRow = makeStepper(QStringLiteral("Key:"), m_keyDownButton, m_keyValueLabel,
                                      m_keyUpButton, m_keyResetButton);
    m_keyValueLabel->setText(QStringLiteral("0"));
    // Beside the Key control: the song's own key and the key sung in.
    m_songKeyLabel = makeLabel(QString(), bar, QStringLiteral("songKeyInfo"));
    m_songKeyLabel->setVisible(false);
    keyRow->addWidget(m_songKeyLabel);
    QHBoxLayout* tempoRow = makeStepper(QStringLiteral("Tempo:"), m_tempoDownButton,
                                        m_tempoValueLabel, m_tempoUpButton, m_tempoResetButton);
    m_tempoValueLabel->setText(QStringLiteral("100%"));

    m_lyricsButton = makeButton(QStringLiteral("Lyrics"), bar, QStringLiteral("ghostButton"));
    m_lyricsButton->setIcon(ui::glyphIcon(ui::Glyph::Lyrics, theme::color::text));
    m_lyricsButton->setToolTip(QStringLiteral("Back to the lyrics (Enter)"));
    m_settingsButton = new QToolButton(bar);
    m_settingsButton->setObjectName(QStringLiteral("settingsButton"));
    m_settingsButton->setFocusPolicy(Qt::NoFocus);
    m_settingsButton->setText(QStringLiteral("Settings"));
    m_settingsButton->setIcon(ui::glyphIcon(ui::Glyph::Gear, theme::color::text));
    theme::setIconSize(m_settingsButton, 18);
    m_settingsButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_settingsButton->setToolTip(QStringLiteral("Settings"));

    const auto separator = [bar](QHBoxLayout* row) {
        theme::addSpacing(row, 10);
        row->addWidget(makeSeparator(bar));
        theme::addSpacing(row, 10);
    };
    auto* controlRow = new QHBoxLayout;
    controlRow->setSpacing(6);
    controlRow->addWidget(m_playButton);
    controlRow->addWidget(m_pauseButton);
    controlRow->addWidget(m_stopButton);
    if (m_playlistView) {
        separator(controlRow);
        controlRow->addWidget(makeLabel(QStringLiteral("Autoplay"), bar,
                                        QStringLiteral("controlCaption")));
        controlRow->addWidget(m_playlistView->takeAutoplayButton());
    }
    separator(controlRow);
    controlRow->addLayout(keyRow);
    separator(controlRow);
    controlRow->addLayout(tempoRow);
    controlRow->addStretch();
    // Lyrics and Settings sit at the top right, beside Now Playing. There is
    // no Exit button: the window's close button, the Exit shortcut and
    // Settings > General > Quit Application close the program.
    topRow->addWidget(m_lyricsButton);
    topRow->addWidget(m_settingsButton);

    auto* layout = new QVBoxLayout(bar);
    layout->setContentsMargins(18, 12, 18, 12);
    layout->setSpacing(12);
    layout->addLayout(topRow);
    layout->addLayout(controlRow);
    return bar;
}

void MainWindow::installShortcuts()
{
    // Each action goes through the same button or slot as the mouse, so a
    // disabled button (no song, no selection, a preview) does nothing here
    // either. Letters and Space typed into a text box always go to the box.
    const QHash<QString, QKeySequence> keys = shortcuts::effective(*m_preferences);
    for (const shortcuts::Action& definition : shortcuts::actions()) {
        auto* action = new QAction(definition.title, this);
        action->setObjectName(definition.id);
        action->setShortcutContext(Qt::WindowShortcut);
        action->setShortcut(keys.value(definition.id));
        const QString id = definition.id;
        connect(action, &QAction::triggered, this, [this, id] { runAction(id); });
        addAction(action);
        m_actions.insert(id, action);
    }
}

void MainWindow::runAction(const QString& id)
{
    if (QApplication::activeModalWidget())
        return;
    // While the lyrics are up only the song itself can be controlled (play,
    // pause, stop, Key, Tempo, lyrics) or the program left: nothing on the
    // hidden home screen is acted on.
    const bool songControl = id.startsWith(QLatin1String("playback."))
        || id.startsWith(QLatin1String("key.")) || id.startsWith(QLatin1String("tempo."));
    if (lyricsVisible() && !songControl && id != QLatin1String("app.exit"))
        return;
    // A maintenance preview is never replaced from the keyboard.
    if (m_previewing
        && (id == QLatin1String("library.sing") || id == QLatin1String("playlist.play")
            || id == QLatin1String("playback.restart") || id == QLatin1String("app.openFile")))
        return;
    const auto press = [](QAbstractButton* button) {
        if (button && button->isEnabled())
            button->click();
    };
    const auto sortBy = [this](LibrarySort sort) {
        if (m_library && m_library->sortBox()->isEnabled())
            m_library->sortBox()->setCurrentIndex(m_library->sortBox()->findData(int(sort)));
    };
    if (id == QLatin1String("playback.playPause"))
        press(m_player->state() == KaraokePlayer::State::Playing ? m_pauseButton : m_playButton);
    else if (id == QLatin1String("playback.stop"))
        press(m_stopButton);
    else if (id == QLatin1String("playback.restart")) {
        // From the beginning, as Stop then Play would (not for a preview).
        if (m_player->hasSong() && !m_previewing) {
            m_player->stop();
            onPlay();
        }
    } else if (id == QLatin1String("playback.autoplay"))
        press(m_playlistView ? m_playlistView->autoplayButton() : nullptr);
    else if (id == QLatin1String("playback.lyrics"))
        press(m_lyricsButton);
    else if (id == QLatin1String("key.down"))
        press(m_keyDownButton);
    else if (id == QLatin1String("key.up"))
        press(m_keyUpButton);
    else if (id == QLatin1String("key.reset"))
        press(m_keyResetButton);
    else if (id == QLatin1String("tempo.down"))
        press(m_tempoDownButton);
    else if (id == QLatin1String("tempo.up"))
        press(m_tempoUpButton);
    else if (id == QLatin1String("tempo.reset"))
        press(m_tempoResetButton);
    else if (!m_library && !id.startsWith(QLatin1String("app.")))
        return;
    else if (id == QLatin1String("library.focusSearch")) {
        if (lyricsVisible())
            return;
        setPlaylistInUse(false);
        m_library->searchBox()->setFocus(Qt::ShortcutFocusReason);
        m_library->searchBox()->selectAll();
    } else if (id == QLatin1String("library.clearSearch"))
        m_library->searchBox()->clear();
    else if (id == QLatin1String("library.sing"))
        press(m_library->singButton());
    else if (id == QLatin1String("library.add"))
        press(m_library->addToPlaylistButton());
    else if (id == QLatin1String("library.sort.artistAsc"))
        sortBy(LibrarySort::ArtistAsc);
    else if (id == QLatin1String("library.sort.artistDesc"))
        sortBy(LibrarySort::ArtistDesc);
    else if (id == QLatin1String("library.sort.titleAsc"))
        sortBy(LibrarySort::TitleAsc);
    else if (id == QLatin1String("library.sort.titleDesc"))
        sortBy(LibrarySort::TitleDesc);
    else if (id == QLatin1String("library.sort.mostPlayed"))
        sortBy(LibrarySort::MostPlayed);
    else if (id == QLatin1String("library.sort.recentlyPlayed"))
        sortBy(LibrarySort::RecentlyPlayed);
    else if (id == QLatin1String("library.sort.labelAsc"))
        sortBy(LibrarySort::LabelAsc);
    else if (id == QLatin1String("playlist.play"))
        press(m_playlistView->playButton());
    else if (id == QLatin1String("playlist.moveUp"))
        press(m_playlistView->moveUpButton());
    else if (id == QLatin1String("playlist.moveDown"))
        press(m_playlistView->moveDownButton());
    else if (id == QLatin1String("playlist.remove"))
        press(m_playlistView->removeButton());
    else if (id == QLatin1String("playlist.new"))
        press(m_playlistView->newButton());
    else if (id == QLatin1String("playlist.rename"))
        press(m_playlistView->renameButton());
    else if (id == QLatin1String("playlist.delete"))
        press(m_playlistView->deleteButton());
    else if (id == QLatin1String("playlist.next"))
        m_playlistView->showAdjacentPlaylist(1);
    else if (id == QLatin1String("playlist.previous"))
        m_playlistView->showAdjacentPlaylist(-1);
    else if (id == QLatin1String("app.settings"))
        openSettings();
    else if (id == QLatin1String("app.review"))
        openMetadataReview();
    else if (id == QLatin1String("app.openFile"))
        chooseSong();
    else if (id == QLatin1String("app.changeFolder")) {
        if (m_library)
            m_library->chooseFolder();
    } else if (id == QLatin1String("app.rescan")) {
        if (m_libraryController)
            m_libraryController->requestRefreshScan();
    } else if (id == QLatin1String("app.exit"))
        close();
}

void MainWindow::applyPreference(const QString& key)
{
    if (key.startsWith(pref::ShortcutPrefix)) {
        // One change can free or take another action's keys: redo them all.
        const QHash<QString, QKeySequence> keys = shortcuts::effective(*m_preferences);
        for (auto it = m_actions.cbegin(); it != m_actions.cend(); ++it)
            it.value()->setShortcut(keys.value(it.key()));
    } else if (key == pref::ShowLabelColumn && m_library) {
        m_library->setColumnVisible(LibraryResultsModel::LabelColumn,
                                    m_preferences->flag(key, true));
    } else if (key == pref::ShowPlaysColumn && m_library) {
        m_library->setColumnVisible(LibraryResultsModel::PlaysColumn,
                                    m_preferences->flag(key, true));
    } else if (key == pref::ShowKeyColumn && m_library) {
        m_library->setColumnVisible(LibraryResultsModel::KeyColumn,
                                    m_preferences->flag(key, true));
    } else if (key == pref::AnalyseSongKeys && m_libraryController) {
        m_libraryController->setSongKeyAnalysisEnabled(m_preferences->flag(key, false));
    } else if (key == pref::ConfirmRemoveSong && m_playlistView) {
        m_playlistView->setConfirmRemove(m_preferences->flag(key, true));
    } else if (key == pref::Volume) {
        m_player->setVolumePercent(m_preferences->number(key, 100));
    } else if (key == pref::AudioOutput) {
        m_player->setAudioOutput(m_preferences->text(key));
    } else if (key == pref::KeepDisplayAwake) {
        applyDisplaySleep();
    } else if (key == pref::ScalePercent) {
        theme::setScalePercent(m_preferences->number(key, 100));
        fitScaleToScreen();
    } else if (key == pref::CompactRows) {
        theme::setCompactRows(m_preferences->flag(key, false));
    } else if (key == pref::AlternateRows) {
        theme::setAlternateRows(m_preferences->flag(key, true));
    }
}

void MainWindow::applyDisplaySleep()
{
    m_displaySleepBlocker.setActive(m_player->state() == KaraokePlayer::State::Playing
                                    && m_preferences->flag(pref::KeepDisplayAwake, true));
}

SettingsDialog* MainWindow::openSettings()
{
    if (!m_settings) {
        SettingsDialog::Context context;
        context.preferences = m_preferences;
        context.libraryController = m_libraryController;
        context.libraryView = m_library;
        context.playlistStore = m_playlistStore;
        context.songSettings = m_settingsStore;
        context.player = m_player;
        context.dataLocations = m_dataLocations;
        context.openSongFile = [this] { chooseSong(); };
        context.openNeedsReview = [this] { openMetadataReview(); };
        // After Settings has closed, as the window's close button would.
        context.quit = [this] { QTimer::singleShot(0, this, [this] { close(); }); };
        m_settings = new SettingsDialog(context, this);
        m_settings->setAttribute(Qt::WA_DeleteOnClose);
        connect(m_settings, &QDialog::finished, this, &MainWindow::restoreFocus);
    }
    QElapsedTimer timer;
    timer.start();
    // Modal, but a window of its own rather than a macOS sheet (whose
    // slide-in animation made Settings feel slow to open).
    m_settings->setWindowModality(Qt::ApplicationModal);
    m_settings->show();
    m_settings->raise();
    m_settings->activateWindow();
    qCInfo(lcTiming).noquote() << "Settings: show" << timer.nsecsElapsed() / 1000000.0 << "ms";
    return m_settings;
}

bool MainWindow::songActive() const
{
    const auto state = m_player->state();
    return !m_previewing
        && (state == KaraokePlayer::State::Playing || state == KaraokePlayer::State::Paused);
}

void MainWindow::restoreFocus()
{
    if (lyricsVisible())
        setFocus(Qt::OtherFocusReason);
    else
        focusHome();
}

void MainWindow::focusHome()
{
    if (m_library && m_library->searchBox()->isEnabled())
        m_library->searchBox()->setFocus(Qt::OtherFocusReason);
    else
        setFocus(Qt::OtherFocusReason);
}

MainWindow::~MainWindow() = default;

bool MainWindow::lyricsVisible() const
{
    return m_pages->currentWidget() == m_lyrics;
}

bool MainWindow::libraryVisible() const
{
    return m_library && m_pages->currentWidget() == m_home;
}

void MainWindow::setPlaylistInUse(bool playlist)
{
    m_playlistView->setActive(playlist);
    m_library->setActive(!playlist);
    // The gold pane is always the one the keyboard acts on: Up/Down and Enter
    // go to the playlist list or to the library's search box. (Buttons never
    // take the keyboard themselves.)
    QWidget* keyboard = playlist ? static_cast<QWidget*>(m_playlistView->itemList())
                                 : static_cast<QWidget*>(m_library->searchBox());
    if (!lyricsVisible() && keyboard->isEnabled() && focusWidget() != keyboard)
        keyboard->setFocus(Qt::MouseFocusReason);
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::MouseButtonPress && m_library) {
        if (watched == m_playlistView->itemList()->viewport())
            setPlaylistInUse(true);
        else if (watched == m_library->resultsList()->viewport()
                 || watched == m_library->searchBox())
            setPlaylistInUse(false);
    }
    if (event->type() == QEvent::KeyPress && isEnterKey(static_cast<QKeyEvent*>(event))) {
        // A maintenance preview is never replaced from the keyboard.
        if (m_previewing)
            return true;
        // While a song is on, Enter in the search box or the playlist only
        // ever returns to its lyrics; it never starts another song.
        if (songActive()) {
            showLyrics();
            return true;
        }
        // With nothing searched for and no song chosen, Enter does not pick
        // the first song of the whole library.
        if (m_library && watched == m_library->searchBox()
            && m_library->searchBox()->text().trimmed().isEmpty()
            && m_library->selectedSongId() == 0)
            return true;
    }
    return QWidget::eventFilter(watched, event);
}

void MainWindow::keyPressEvent(QKeyEvent* event)
{
    if (!QApplication::activeModalWidget()) {
        if (lyricsVisible() && event->key() == Qt::Key_Escape)
            hideLyrics();
        else if (!lyricsVisible() && isEnterKey(event) && songActive())
            showLyrics();
    }
    // No other key, including Space, performs an action.
    event->accept();
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (m_preferences->flag(pref::ConfirmExit, false) && !m_exitConfirmed
        && !(m_exitConfirmation && m_exitConfirmation())) {
        event->ignore();
        if (m_exitConfirmation)
            return;  // declined
        // Non-blocking, so playback is never run from a nested event loop.
        auto* box = new QMessageBox(QMessageBox::Question, QStringLiteral("Exit"),
                                    QStringLiteral("Exit Frankie's Karaoke Studio?"),
                                    QMessageBox::Yes | QMessageBox::No, this);
        box->setDefaultButton(QMessageBox::No);
        box->setAttribute(Qt::WA_DeleteOnClose);
        connect(box, &QMessageBox::finished, this, [this](int result) {
            if (result == QMessageBox::Yes) {
                m_exitConfirmed = true;
                close();
            } else {
                restoreFocus();
            }
        });
        box->open();
        return;
    }
    m_exitConfirmed = false;
    if (m_preferences->flag(pref::RememberWindow, true))
        m_preferences->setText(pref::WindowGeometry, QString::fromLatin1(saveGeometry().toBase64()));
    m_playlistPlayback->cancelPendingAutoplay();
    m_player->stop();
    event->accept();
}

void MainWindow::setKeepScaleWithinScreen(bool keep)
{
    m_keepScaleWithinScreen = keep;
    m_fittedRoom = {};
    if (!keep) {
        for (const QMetaObject::Connection& connection : std::as_const(m_screenConnections))
            disconnect(connection);
        m_screenConnections.clear();
        theme::setScaleLimitPercent(theme::kMaxScalePercent);
        return;
    }
    watchScreen();
    fitScaleToScreen();
}

void MainWindow::watchScreen()
{
    for (const QMetaObject::Connection& connection : std::as_const(m_screenConnections))
        disconnect(connection);
    m_screenConnections.clear();
    if (!m_keepScaleWithinScreen)
        return;
    // Moved to another screen, or this screen changed (resolution, Windows
    // display scaling, taskbar): fit again.
    if (QWindow* window = windowHandle()) {
        m_screenConnections.append(connect(window, &QWindow::screenChanged, this, [this] {
            watchScreen();
            fitScaleToScreen();
        }));
    }
    if (QScreen* where = screen()) {
        m_screenConnections.append(connect(where, &QScreen::geometryChanged, this,
                                           &MainWindow::fitScaleToScreen));
        m_screenConnections.append(connect(where, &QScreen::availableGeometryChanged, this,
                                           &MainWindow::fitScaleToScreen));
        m_screenConnections.append(connect(where, &QScreen::logicalDotsPerInchChanged, this,
                                           &MainWindow::fitScaleToScreen));
    }
}

void MainWindow::fitScaleToScreen()
{
    const QScreen* where = screen();
    if (!m_keepScaleWithinScreen || m_fittingScale || !where)
        return;
    const QScopedValueRollback<bool> fitting(m_fittingScale, true);
    const bool fullScreen = windowState() & Qt::WindowFullScreen;
    QSize room = fullScreen ? where->geometry().size() : where->availableGeometry().size();
    if (!fullScreen)
        room -= frameGeometry().size() - geometry().size();  // title bar and borders
    // Nothing to do when neither the room nor the choice changed (screens
    // report many changes that leave the room as it was).
    const auto currentlyFits = [this, room] {
        const QSize needed = minimumSizeHint();
        return needed.width() <= room.width() && needed.height() <= room.height();
    };
    if (room == m_fittedRoom && theme::chosenScalePercent() == m_fittedChosenScale && currentlyFits())
        return;
    m_fittedRoom = room;
    m_fittedChosenScale = theme::chosenScalePercent();
    const auto ratio = [this, room] {
        const QSize needed = minimumSizeHint();
        if (!needed.isValid() || needed.isEmpty())
            return 1.0;
        return std::min(double(room.width()) / needed.width(), double(room.height()) / needed.height());
    };
    // Sizes grow in step with the scale, so start from an estimate, then make
    // sure: a step smaller until everything fits (80% is the smallest).
    const int step = theme::kScaleStepPercent;
    const int estimate = int(theme::scalePercent() * ratio() + 1e-6) / step * step;
    theme::setScaleLimitPercent(estimate >= theme::chosenScalePercent() ? theme::kMaxScalePercent
                                                                        : estimate);
    while (ratio() < 1.0 && theme::scalePercent() > theme::kMinScalePercent)
        theme::setScaleLimitPercent(theme::scalePercent() - step);
    // A window that grew to an earlier, larger minimum is not shrunk by the
    // system (full screen neither): bring it back within the screen.
    if (isVisible() && (width() > room.width() || height() > room.height())) {
        if (fullScreen)
            setGeometry(where->geometry());
        else
            resize(size().boundedTo(room));
    }
    if (theme::scalePercent() < theme::chosenScalePercent()) {
        qCInfo(lcUi) << "Interface size" << theme::scalePercent() << "% instead of"
                     << theme::chosenScalePercent() << "% so the window fits the screen" << room
                     << "(device pixel ratio" << where->devicePixelRatio() << ")";
    }
}

void MainWindow::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange)
        fitScaleToScreen();  // full screen has more room than a window
    if (event->type() == QEvent::ActivationChange && isActiveWindow()) {
        QWidget* focused = QApplication::focusWidget();
        if (lyricsVisible()) {
            if (focused != this)
                setFocus(Qt::OtherFocusReason);
        } else if (!focused || focused == this || !m_home->isAncestorOf(focused)) {
            focusHome();
        }
    }
}

MetadataReviewDialog* MainWindow::openMetadataReview()
{
    if (!m_libraryController)
        return nullptr;
    if (!m_review) {
        m_review = new MetadataReviewDialog(m_libraryController, this);
        m_review->setPreviewAvailable(true);
        connect(m_review, &MetadataReviewDialog::previewRequested, this, [this](qint64 songId) {
            QString error;
            if (!previewSong(songId, &error) && m_review)
                m_review->showPreviewMessage(error);
        });
        connect(m_review, &MetadataReviewDialog::previewStopRequested,
                this, &MainWindow::stopPreview);
        // Closing the maintenance screen ends its preview.
        connect(m_review, &QDialog::finished, this, &MainWindow::stopPreview);
        connect(m_player, &KaraokePlayer::frameChanged, m_review, [this](const QImage& frame) {
            if (m_previewing)
                m_review->showPreviewFrame(frame);
        });
        theme::rescale(m_review);
    }
    m_review->refresh();
    m_review->show();
    m_review->raise();
    m_review->activateWindow();
    return m_review;
}

bool MainWindow::previewSong(qint64 songId, QString* error)
{
    m_playlistPlayback->cancelPendingAutoplay();
    const auto fail = [error](const QString& message) {
        if (error)
            *error = message;
        return false;
    };
    if (!m_libraryController)
        return fail(QStringLiteral("The song library is unavailable."));
    QString lookupError;
    const PlaybackPaths paths = m_libraryController->playbackPathsForAny(songId, &lookupError);
    if (!paths.playable() || !QFileInfo::exists(paths.mp3Path)
        || !QFileInfo::exists(paths.graphicsPath)) {
        qCWarning(lcUi).noquote() << "Preview song is unavailable:" << lookupError << paths.reason;
        return fail(QStringLiteral("This song can't be played right now (is its music drive connected?)."));
    }
    if (!loadSong(paths.mp3Path))
        return fail(QStringLiteral("This song could not be opened."));
    // Set after loading: stopping the previous song must not end the preview.
    // The song is known, but a preview never counts as a play.
    m_playSongId = songId;
    m_playlistPlayback->clear();
    setPreviewing(true);
    m_player->play();
    if (m_player->state() == KaraokePlayer::State::Error) {
        setPreviewing(false);
        return fail(QStringLiteral("This song could not be played."));
    }
    return true;
}

void MainWindow::stopPreview()
{
    if (!m_previewing)
        return;
    m_playlistPlayback->cancelPendingAutoplay();
    m_player->stop();
    setPreviewing(false);
}

void MainWindow::setPreviewing(bool previewing)
{
    if (m_previewing == previewing)
        return;
    m_previewing = previewing;
    updateControls();
    if (!m_review)
        return;
    const SongPair& song = m_player->song();
    m_review->setPreviewState(previewing, previewing
        ? QFileInfo(song.mp3Path).completeBaseName() : QString());
    if (previewing)
        m_review->showPreviewFrame(m_player->currentFrame());
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
    if (!loadSong(paths.mp3Path))
        return;
    m_playSongId = songId;
    m_playlistPlayback->clear();
    updateControls();
    // Singing a library song starts it straight away (and shows its lyrics),
    // just like Play.
    m_player->play();
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
    m_playSongId = resolution.songId;
    updateControls();
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
    restoreFocus();
    if (path.isEmpty())
        return;  // Cancelled: whatever was playing carries on.

    settings.setValue(QStringLiteral("lastSongFolder"), QFileInfo(path).absolutePath());
    openSong(path);
}

bool MainWindow::openSong(const QString& path)
{
    if (!loadSong(path))
        return false;
    // A file opened directly counts as its catalogue song when it is one.
    if (m_libraryController)
        m_playSongId = m_libraryController->songIdForMp3File(m_player->song().mp3Path);
    m_playlistPlayback->clear();
    updateControls();
    return true;
}

bool MainWindow::loadSong(const QString& path)
{
    qCInfo(lcUi) << "Opening" << path;
    // Any other song ends a preview. It is stopped first, so a replacement
    // that turns out to be unplayable cannot leave the preview running unseen.
    stopPreview();
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

    // A new song is loaded; the caller names its catalogue song, if any. (A
    // rejected load keeps the previous song, and with it the previous id.)
    m_playSongId = 0;
    m_songIdentity = songIdentity(result.pair);
    m_identityWarningLogged = false;
    if (m_songIdentity.isEmpty()) {
        qCWarning(lcUi) << "Song settings cannot be saved because its files could not be fingerprinted";
        m_identityWarningLogged = true;
    }
    // A song sung before keeps its own Key/Tempo; any other starts from the
    // defaults chosen in Settings (normally 0 and 100%).
    const SongSettings settings = (!m_songIdentity.isEmpty()
                                   && m_settingsStore->hasSettingsFor(m_songIdentity)
        ? m_settingsStore->settingsFor(m_songIdentity)
        : SongSettings{m_preferences->number(pref::DefaultKey, 0),
                       m_preferences->number(pref::DefaultTempo, 100)}).clamped();
    m_player->setKeySemitones(settings.keySemitones);
    m_player->setTempoPercent(settings.tempoPercent);
    updateControls();
    return true;
}

void MainWindow::onPlay()
{
    setPreviewing(false);  // the main Play button is normal singing
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
    // A preview never writes Key/Tempo memory.
    if (!m_player->hasSong() || m_previewing)
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
    if (m_previewing)
        return;  // a preview's lyrics stay in the maintenance screen
    // Coming back from the lyrics returns the keyboard to where it was.
    if (!lyricsVisible()) {
        QWidget* focused = focusWidget();
        m_homeFocus = focused && m_home->isAncestorOf(focused) ? focused : nullptr;
    }
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
    const bool fromLyrics = lyricsVisible();
    m_pages->setCurrentWidget(m_home);
    QWidget* focused = focusWidget();
    if (fromLyrics && m_homeFocus && m_homeFocus->isVisible() && m_homeFocus->isEnabled())
        m_homeFocus->setFocus(Qt::OtherFocusReason);
    else if (fromLyrics || !focused || focused == this || !m_home->isAncestorOf(focused))
        focusHome();  // (already on the home screen: the keyboard stays put)
    // Back on the home screen: notice a music drive connected meanwhile.
    if (fromLyrics && m_libraryController)
        m_libraryController->recheckRoot();
}

void MainWindow::onStateChanged(KaraokePlayer::State state)
{
    // A play counts once per start from the beginning (not on resume after
    // Pause, never for a maintenance preview), when its audio first advances.
    const KaraokePlayer::State previous = m_lastPlayerState;
    m_lastPlayerState = state;
    if (state == KaraokePlayer::State::Playing) {
        if (previous != KaraokePlayer::State::Paused && previous != KaraokePlayer::State::Playing)
            m_playStartPending = !m_previewing && m_playSongId != 0 && m_libraryController;
    } else if (state != KaraokePlayer::State::Paused) {
        m_playStartPending = false;
    }
    if (m_libraryController) {
        m_libraryController->setPlaybackActive(
            state == KaraokePlayer::State::Playing || state == KaraokePlayer::State::Paused);
    }
    m_errorText.clear();
    applyDisplaySleep();
    switch (state) {
    case KaraokePlayer::State::Playing:
        // A preview's lyrics stay in the maintenance screen.
        if (!m_previewing)
            showLyrics();
        break;
    case KaraokePlayer::State::Finished:
        setPreviewing(false);
        // The lyrics may stay up at the end of a song (Settings); Autoplay
        // still moves on to the next playlist song.
        if (m_preferences->flag(pref::ReturnHomeAtEnd, true))
            hideLyrics();
        break;
    case KaraokePlayer::State::Stopped:
    case KaraokePlayer::State::Error:
    case KaraokePlayer::State::Empty:
        setPreviewing(false);
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
    connect(box, &QDialog::finished, this, &MainWindow::restoreFocus);
    box->open();
}

QString MainWindow::statusText() const
{
    return m_statusLabel->text();
}

QString MainWindow::songTitle()
{
    const QString path = m_player->song().mp3Path;
    if (path != m_titlePath || m_playSongId != m_titleSongId) {
        m_titlePath = path;
        m_titleSongId = m_playSongId;
        m_title = m_player->song().displayName();
        const auto song = m_libraryController && m_playSongId != 0
            ? m_libraryController->songRef(m_playSongId) : std::nullopt;
        if (song && !song->title.trimmed().isEmpty()) {
            const QString artist = song->artist.trimmed();
            m_title = artist.isEmpty() ? song->title.trimmed()
                                       : artist + QStringLiteral(" \u2013 ") + song->title.trimmed();
        }
    }
    return m_title;
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

    m_songLabel->setText(hasSong ? songTitle() : QStringLiteral("No song loaded"));

    const QString time = formatTime(m_player->positionMs());
    const QString total = m_player->durationMs() > 0
        ? QStringLiteral(" of %1").arg(formatTime(m_player->durationMs()))
        : QString();

    QString status;
    switch (state) {
    case State::Empty:
        status = m_library ? QStringLiteral("Choose a song from the library or a playlist.")
                           : QStringLiteral("Open a song file from Settings.");
        break;
    case State::Ready:
        status = QStringLiteral("Ready. Press Play to start.");
        break;
    case State::Playing:
        status = QStringLiteral("Playing  %1%2").arg(time, total);
        break;
    case State::Paused:
        status = QStringLiteral("Paused at %1. Press Resume to carry on.").arg(time);
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
                                     : theme::dangerStyle());

    m_playButton->setText(state == State::Paused ? QStringLiteral("Resume") : QStringLiteral("Play"));
    m_playButton->setToolTip(m_playButton->text());
    m_playButton->setEnabled(hasSong && state != State::Playing);
    m_pauseButton->setEnabled(state == State::Playing);
    // Play and Pause share one place in the player bar.
    m_pauseButton->setVisible(state == State::Playing);
    m_playButton->setVisible(state != State::Playing);
    m_stopButton->setEnabled(state == State::Playing || state == State::Paused);
    m_lyricsButton->setEnabled(songActive());
    // The library marks the version being sung (not a maintenance preview).
    if (m_library)
        m_library->setPlayingSongId(songActive() ? m_playSongId : 0);

    const int key = hasSong ? m_player->keySemitones() : 0;
    const int tempo = hasSong ? m_player->tempoPercent() : 100;
    m_keyValueLabel->setText(key > 0 ? QStringLiteral("+%1").arg(key) : QString::number(key));
    // Original key -> current key, only for a song whose key is known
    // (never for a maintenance preview).
    const qint64 keySongId = hasSong && !m_previewing ? m_playSongId : 0;
    if (keySongId != m_songKeyForId) {
        m_songKeyForId = keySongId;
        m_songKeyIndex = -1;
        if (keySongId > 0 && m_libraryController) {
            if (const std::optional<SongKeyInfo> info = m_libraryController->songKey(keySongId))
                m_songKeyIndex = info->keyIndex;
        }
    }
    if (m_songKeyIndex >= 0) {
        const QString original = songKeyName(m_songKeyIndex);
        const QString current = transposedKeyName(m_songKeyIndex, key);
        const QString shift = key > 0 ? QStringLiteral("+%1").arg(key) : QString::number(key);
        m_songKeyLabel->setText(key == 0 ? QStringLiteral("(%1)").arg(original)
                                         : QStringLiteral("(%1 \u2192 %2)").arg(original, current));
        m_songKeyLabel->setToolTip(key == 0
            ? QStringLiteral("Original key: %1").arg(original)
            : QStringLiteral("Original key: %1\nCurrent key: %2 (%3)").arg(original, current, shift));
    }
    m_songKeyLabel->setVisible(m_songKeyIndex >= 0);
    m_tempoValueLabel->setText(QStringLiteral("%1%").arg(tempo));
    // Key/Tempo belong to singing, not to a maintenance preview.
    const bool adjustable = hasSong && !m_previewing;
    m_keyDownButton->setEnabled(adjustable && key > kMinKey);
    m_keyUpButton->setEnabled(adjustable && key < kMaxKey);
    m_keyResetButton->setEnabled(adjustable);
    m_tempoDownButton->setEnabled(adjustable && tempo > kMinTempo);
    m_tempoUpButton->setEnabled(adjustable && tempo < kMaxTempo);
    m_tempoResetButton->setEnabled(adjustable);
}
