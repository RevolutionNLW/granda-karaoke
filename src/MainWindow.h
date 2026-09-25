#pragma once

#include "KaraokePlayer.h"
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
    explicit MainWindow(KaraokePlayer* player, QWidget* parent = nullptr);
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
    void showLyrics();
    void hideLyrics();
    void onStateChanged(KaraokePlayer::State state);
    void onError(const QString& message);
    void updateControls();
    void showError(const QString& message);

    KaraokePlayer* m_player;
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
    QPushButton* m_exitButton;
    QString m_errorText;
    bool m_showErrorDialogs = true;
};
