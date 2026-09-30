#pragma once

#include "KaraokePlayer.h"
#include "SongSettings.h"
#include "platform/DisplaySleepBlocker.h"

#include <QHash>
#include <QPointer>
#include <QWidget>

#include <functional>
#include <memory>

class AppPreferences;
class LyricsView;
class QAction;
class SettingsDialog;
class LibraryController;
class LibraryView;
class MetadataReviewDialog;
class PlaylistPlayback;
class PlaylistStore;
class PlaylistView;
struct PlaylistEntry;
class QStackedWidget;
class QLabel;
class QPushButton;
class QToolButton;

// The home screen (player bar, library search, song library and playlists)
// and the fullscreen lyrics page that it switches to while a song plays.
class MainWindow : public QWidget {
    Q_OBJECT

public:
    explicit MainWindow(KaraokePlayer* player, ISongSettingsStore* settingsStore,
                        LibraryController* libraryController = nullptr,
                        PlaylistStore* playlistStore = nullptr,
                        AppPreferences* preferences = nullptr,
                        QWidget* parent = nullptr);
    ~MainWindow() override;

    // Loads the pair containing the given .mp3 or .cdg file. Reports problems
    // to the user and returns false on failure.
    bool openSong(const QString& path);

    // Keeps the interface size within the screen the window is on. Windows
    // display scaling (125%, 150%) enlarges everything as well, so together
    // with a large size from Settings the window could outgrow the screen;
    // the largest size that fits is used then, and the chosen size comes back
    // when the screen allows. The program turns this on once its window is
    // shown (tests run on a small virtual screen and leave it off).
    void setKeepScaleWithinScreen(bool keep);

    LyricsView* lyricsView() const { return m_lyrics; }
    bool lyricsVisible() const;
    bool libraryVisible() const;
    bool displaySleepBlocked() const { return m_displaySleepBlocker.isActive(); }
    QPushButton* lyricsButton() const { return m_lyricsButton; }
    QToolButton* settingsButton() const { return m_settingsButton; }
    QPushButton* playButton() const { return m_playButton; }
    QPushButton* pauseButton() const { return m_pauseButton; }
    QPushButton* stopButton() const { return m_stopButton; }
    QPushButton* keyDownButton() const { return m_keyDownButton; }
    QPushButton* keyUpButton() const { return m_keyUpButton; }
    QPushButton* keyResetButton() const { return m_keyResetButton; }
    QPushButton* tempoDownButton() const { return m_tempoDownButton; }
    QPushButton* tempoUpButton() const { return m_tempoUpButton; }
    QPushButton* tempoResetButton() const { return m_tempoResetButton; }
    QLabel* keyValueLabel() const { return m_keyValueLabel; }
    // The loaded song's own key and the key heard with Key applied (hidden
    // while the song's key is not known).
    QLabel* songKeyLabel() const { return m_songKeyLabel; }
    QLabel* tempoValueLabel() const { return m_tempoValueLabel; }
    QString statusText() const;
    QString songText() const;
    LibraryView* libraryView() const { return m_library; }
    PlaylistView* playlistView() const { return m_playlistView; }
    PlaylistPlayback* playlistPlayback() const { return m_playlistPlayback; }
    // Library maintenance (song-name review and correction). Opened with
    // Ctrl+Shift+M (Cmd+Shift+M on macOS); not part of the singer's screens.
    MetadataReviewDialog* openMetadataReview();
    // Plays a library song so it can be identified from the maintenance
    // screen. A preview has no playlist context (so it never autoplays), shows
    // its lyrics only in the maintenance screen and never stores Key/Tempo.
    bool previewSong(qint64 songId, QString* error = nullptr);
    void stopPreview();
    bool isPreviewing() const { return m_previewing; }

    // When false, problems are shown only in the status line (used by tests).
    void setShowErrorDialogs(bool show) { m_showErrorDialogs = show; }

    AppPreferences* preferences() const { return m_preferences; }
    // The action behind a configurable shortcut (see ui/Shortcuts.h).
    QAction* shortcutAction(const QString& id) const { return m_actions.value(id); }
    SettingsDialog* openSettings();
    // Where the program keeps its files, for Settings > Advanced (label, path).
    void setDataLocations(QList<QPair<QString, QString>> locations)
    {
        m_dataLocations = std::move(locations);
    }
    // Replaces the Exit confirmation question (tests); returns true to exit.
    void setExitConfirmation(std::function<bool()> confirm) { m_exitConfirmation = std::move(confirm); }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void installShortcuts();
    void runAction(const QString& id);
    void applyPreference(const QString& key);
    void applyDisplaySleep();
    void keyPressEvent(QKeyEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void fitScaleToScreen();
    void watchScreen();
    bool loadSong(const QString& path);
    QWidget* buildPlayerBar();
    void chooseSong();
    // A song is playing or paused for singing (not a maintenance preview).
    bool songActive() const;
    // Keyboard focus for the home screen: the search box when there is one.
    void focusHome();
    // Which of the library and the playlist is in use (shows its selection
    // strongly).
    void setPlaylistInUse(bool playlist);
    // "Artist – Song" for the song loaded, from the library when it is known.
    QString songTitle();
    // Focus after a dialog or menu closes: the window on the lyrics page,
    // otherwise the home screen's.
    void restoreFocus();
    void singLibrarySong(qint64 songId);
    void playPlaylistItem(PlaylistEntry entry, bool autoplay);
    void onPlay();
    void onPause();
    void onStop();
    void changeKey(int delta);
    void changeTempo(int delta);
    void persistCurrentSettings();
    void showLyrics();
    void hideLyrics();
    void onStateChanged(KaraokePlayer::State state);
    void onError(const QString& message);
    void updateControls();
    void showError(const QString& message);
    void setPreviewing(bool previewing);

    std::unique_ptr<AppPreferences> m_ownPreferences;
    AppPreferences* m_preferences;
    QHash<QString, QAction*> m_actions;
    QPointer<SettingsDialog> m_settings;
    QList<QPair<QString, QString>> m_dataLocations;
    std::function<bool()> m_exitConfirmation;
    bool m_exitConfirmed = false;
    KaraokePlayer* m_player;
    ISongSettingsStore* m_settingsStore;
    LibraryController* m_libraryController;
    PlaylistStore* m_playlistStore;
    PlaylistPlayback* m_playlistPlayback;
    DisplaySleepBlocker m_displaySleepBlocker;
    bool m_keepScaleWithinScreen = false;
    bool m_fittingScale = false;
    QSize m_fittedRoom;       // the screen room and chosen size last fitted to
    int m_fittedChosenScale = 0;
    QList<QMetaObject::Connection> m_screenConnections;
    QStackedWidget* m_pages;
    QWidget* m_home;
    LyricsView* m_lyrics;
    LibraryView* m_library = nullptr;
    PlaylistView* m_playlistView = nullptr;
    MetadataReviewDialog* m_review = nullptr;
    QLabel* m_songLabel;
    QLabel* m_statusLabel;
    QPushButton* m_lyricsButton;
    QToolButton* m_settingsButton;
    QPushButton* m_playButton;
    QPushButton* m_pauseButton;
    QPushButton* m_stopButton;
    QLabel* m_keyValueLabel;
    QLabel* m_songKeyLabel = nullptr;
    QLabel* m_tempoValueLabel;
    QPushButton* m_keyDownButton;
    QPushButton* m_keyUpButton;
    QPushButton* m_keyResetButton;
    QPushButton* m_tempoDownButton;
    QPushButton* m_tempoUpButton;
    QPushButton* m_tempoResetButton;

    QString m_songIdentity;
    bool m_identityWarningLogged = false;
    QString m_errorText;
    bool m_showErrorDialogs = true;
    bool m_previewing = false;
    // The catalogue song now loaded, for play statistics (0 when unknown).
    qint64 m_playSongId = 0;
    KaraokePlayer::State m_lastPlayerState = KaraokePlayer::State::Empty;
    // A start is counted once its audio position first advances, so a start
    // that fails before any sound is never a play.
    bool m_playStartPending = false;
    // The home-screen widget that had the keyboard before the lyrics opened.
    QPointer<QWidget> m_homeFocus;
    // The loaded song's key (-1 unknown), looked up once per song and again
    // when keys change, not on every position update.
    int m_songKeyIndex = -1;
    qint64 m_songKeyForId = -1;
    // songTitle() is looked up once per song, not on every position update.
    QString m_title;
    QString m_titlePath;
    qint64 m_titleSongId = -1;
};
