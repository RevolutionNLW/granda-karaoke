#pragma once

#include "KaraokePlayer.h"
#include "SongSettings.h"
#include "platform/DisplaySleepBlocker.h"

#include <QWidget>

class LyricsView;
class QStackedWidget;
class QLabel;
class QPushButton;

// The main controls: Open Song, Play/Resume, Pause, Stop and the song status.
class MainWindow : public QWidget {
    Q_OBJECT

public:
    explicit MainWindow(KaraokePlayer* player, ISongSettingsStore* settingsStore,
                        QWidget* parent = nullptr);
    ~MainWindow() override;

    // Loads the pair containing the given .mp3 or .cdg file. Reports problems
    // to the user and returns false on failure.
    bool openSong(const QString& path);

    LyricsView* lyricsView() const { return m_lyrics; }
    bool lyricsVisible() const;
    bool displaySleepBlocked() const { return m_displaySleepBlocker.isActive(); }
    QPushButton* exitButton() const { return m_exitButton; }
    QPushButton* openButton() const { return m_openButton; }
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
    QLabel* tempoValueLabel() const { return m_tempoValueLabel; }
    QString statusText() const;

    // When false, problems are shown only in the status line (used by tests).
    void setShowErrorDialogs(bool show) { m_showErrorDialogs = show; }

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void chooseSong();
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

    KaraokePlayer* m_player;
    ISongSettingsStore* m_settingsStore;
    DisplaySleepBlocker m_displaySleepBlocker;
    QStackedWidget* m_pages;
    QWidget* m_controls;
    LyricsView* m_lyrics;
    QLabel* m_songLabel;
    QLabel* m_statusLabel;
    QLabel* m_hintLabel;
    QPushButton* m_openButton;
    QPushButton* m_playButton;
    QPushButton* m_pauseButton;
    QPushButton* m_stopButton;
    QLabel* m_keyValueLabel;
    QLabel* m_tempoValueLabel;
    QPushButton* m_keyDownButton;
    QPushButton* m_keyUpButton;
    QPushButton* m_keyResetButton;
    QPushButton* m_tempoDownButton;
    QPushButton* m_tempoUpButton;
    QPushButton* m_tempoResetButton;
    QPushButton* m_exitButton;
    QString m_songIdentity;
    bool m_identityWarningLogged = false;
    QString m_errorText;
    bool m_showErrorDialogs = true;
};
