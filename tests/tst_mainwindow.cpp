// Checks the presentation rules of the main window: which page is shown,
// and that switching between lyrics and controls never touches playback.
// Runs on the offscreen platform, so real full-screen behaviour on a monitor
// still needs a manual check.

#include "LyricsView.h"
#include "MainWindow.h"
#include "SettingsDialog.h"
#include "BusTestPlayer.h"
#include "SongSettings.h"
#include "TestMedia.h"
#include "ui/Theme.h"

#include <QPushButton>
#include <QLabel>
#include <QMessageBox>
#include <QFileDialog>
#include <QStackedWidget>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QToolButton>
#include <QAction>
#include <QtTest>

using State = KaraokePlayer::State;

class TestMainWindow : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();
    void openShowsReadySong();
    void controlsFitDefaultSize();
    void settingsControlsDefaultsAndFocus();
    void keyAndTempoButtonsClampAndReset();
    void perSongSettingsPersistAcrossWindows();
    void playShowsLyricsPage();
    void escapeReturnsToControlsWithoutTouchingAudio();
    void keysNeverClickButtons();
    void enterDoesNothingWhenNotPlaying();
    void pauseShowsResume();
    void stopReturnsToControlsAndResets();
    void endOfTrackReturnsToControls();
    void openingAnotherSongStopsPrevious();
    void missingCompanionIsReported();
    void failedOpenKeepsPlaying_data();
    void failedOpenKeepsPlaying();
    void rejectedOpenErrorClearsOnStateChange_data();
    void rejectedOpenErrorClearsOnStateChange();
    void playbackErrorKeepsSpecificMessage();
    void clickingLyricsReturnsToControls();
    void pageSwitchesNeverChangeWindowState();
    void lyricsRestoreFullscreenIfUserLeftIt();
    void errorDialogAppearsExactlyOnce();
    void modalDialogKeysStayInDialog_data();
    void modalDialogKeysStayInDialog();
    void exitStopsAndCloses();
    void noExitButtonOnTheMainScreen();
    void displaySleepFollowsPlayerStates();
    void displaySleepReleasedOnDestruction();

private:
    void startPlaying();
    void createWindow(bool clearSettings);

    QTemporaryDir m_dir;
    QString m_songPath;
    QString m_shortSongPath;
    QString m_settingsPath;
    std::unique_ptr<BusTestPlayer> m_player;
    std::unique_ptr<SongSettingsStore> m_store;
    std::unique_ptr<MainWindow> m_window;
};

void TestMainWindow::initTestCase()
{
    // Measure and drive the window as it ships: with the application theme.
    theme::apply(*qobject_cast<QApplication*>(QCoreApplication::instance()));
    QString error;
    QVERIFY2(KaraokePlayer::initializeGStreamer(&error), qPrintable(error));
    m_songPath = m_dir.filePath("Long Song.mp3");
    m_shortSongPath = m_dir.filePath("Short Song.cdg");
    QVERIFY(testmedia::writeMp3(m_songPath, 4000));
    QVERIFY(testmedia::writeCdg(m_dir.filePath("Long Song.cdg"), testmedia::markerCdg(4000, 300)));
    QVERIFY(testmedia::writeMp3(m_dir.filePath("Short Song.mp3"), 1200));
    QVERIFY(testmedia::writeCdg(m_shortSongPath, testmedia::markerCdg(1200, 300)));
    m_settingsPath = m_dir.filePath("song-settings.json");
}

void TestMainWindow::init()
{
    createWindow(true);
}

void TestMainWindow::createWindow(bool clearSettings)
{
    m_player = std::make_unique<BusTestPlayer>();
    if (clearSettings)
        QFile::remove(m_settingsPath);
    m_store = std::make_unique<SongSettingsStore>(m_settingsPath);
    m_window = std::make_unique<MainWindow>(m_player.get(), m_store.get());
    m_window->setShowErrorDialogs(false);
    m_window->showFullScreen();
    m_window->setFocus();
    QVERIFY(QTest::qWaitForWindowExposed(m_window.get()));
}

void TestMainWindow::cleanup()
{
    m_window.reset();
    m_player.reset();
    m_store.reset();
}

void TestMainWindow::startPlaying()
{
    QVERIFY(m_window->openSong(m_songPath));
    QTest::mouseClick(m_window->playButton(), Qt::LeftButton);
    QCOMPARE(m_player->state(), State::Playing);
}

void TestMainWindow::openShowsReadySong()
{
    QVERIFY(!m_window->playButton()->isEnabled());
    QVERIFY(m_window->openSong(m_songPath));
    QCOMPARE(m_player->state(), State::Ready);
    QVERIFY(m_window->playButton()->isEnabled());
    QVERIFY(!m_window->pauseButton()->isEnabled());
    QVERIFY(m_window->statusText().contains("Ready"));
    QVERIFY(!m_window->lyricsView()->isVisible());
}

void TestMainWindow::controlsFitDefaultSize()
{
    const QSize minimum = m_window->minimumSizeHint();
    QVERIFY2(minimum.width() <= 900,
             qPrintable(QStringLiteral("minimum width is %1").arg(minimum.width())));
    QVERIFY2(minimum.height() <= 520,
             qPrintable(QStringLiteral("minimum height is %1").arg(minimum.height())));
    m_window->showNormal();
    m_window->resize(900, 520);
    QCoreApplication::processEvents();
    QCOMPARE(m_window->size(), QSize(900, 520));
}

void TestMainWindow::settingsControlsDefaultsAndFocus()
{
    QCOMPARE(m_window->keyValueLabel()->text(), QStringLiteral("0"));
    QCOMPARE(m_window->tempoValueLabel()->text(), QStringLiteral("100%"));
    const QList<QPushButton*> buttons{
        m_window->keyDownButton(), m_window->keyUpButton(), m_window->keyResetButton(),
        m_window->tempoDownButton(), m_window->tempoUpButton(), m_window->tempoResetButton(),
    };
    for (QPushButton* button : buttons) {
        QCOMPARE(button->focusPolicy(), Qt::NoFocus);
        QVERIFY(!button->isEnabled());
    }

    QVERIFY(m_window->openSong(m_songPath));
    QCOMPARE(m_window->keyValueLabel()->text(), QStringLiteral("0"));
    QCOMPARE(m_window->tempoValueLabel()->text(), QStringLiteral("100%"));
    for (QPushButton* button : buttons)
        QVERIFY(button->isEnabled());
}

void TestMainWindow::keyAndTempoButtonsClampAndReset()
{
    QVERIFY(m_window->openSong(m_songPath));
    for (int key = 0; key < kMaxKey; ++key)
        QTest::mouseClick(m_window->keyUpButton(), Qt::LeftButton);
    QCOMPARE(m_player->keySemitones(), kMaxKey);
    QCOMPARE(m_window->keyValueLabel()->text(), QStringLiteral("+6"));
    QVERIFY(!m_window->keyUpButton()->isEnabled());
    QVERIFY(m_window->keyDownButton()->isEnabled());

    for (int key = kMaxKey; key > kMinKey; --key)
        QTest::mouseClick(m_window->keyDownButton(), Qt::LeftButton);
    QCOMPARE(m_player->keySemitones(), kMinKey);
    QCOMPARE(m_window->keyValueLabel()->text(), QStringLiteral("-6"));
    QVERIFY(!m_window->keyDownButton()->isEnabled());
    QTest::mouseClick(m_window->keyResetButton(), Qt::LeftButton);
    QCOMPARE(m_player->keySemitones(), 0);
    QCOMPARE(m_window->keyValueLabel()->text(), QStringLiteral("0"));

    for (int tempo = 100; tempo < kMaxTempo; tempo += kTempoStep)
        QTest::mouseClick(m_window->tempoUpButton(), Qt::LeftButton);
    QCOMPARE(m_player->tempoPercent(), kMaxTempo);
    QCOMPARE(m_window->tempoValueLabel()->text(), QStringLiteral("130%"));
    QVERIFY(!m_window->tempoUpButton()->isEnabled());
    for (int tempo = kMaxTempo; tempo > kMinTempo; tempo -= kTempoStep)
        QTest::mouseClick(m_window->tempoDownButton(), Qt::LeftButton);
    QCOMPARE(m_player->tempoPercent(), kMinTempo);
    QCOMPARE(m_window->tempoValueLabel()->text(), QStringLiteral("70%"));
    QVERIFY(!m_window->tempoDownButton()->isEnabled());
    QTest::mouseClick(m_window->tempoResetButton(), Qt::LeftButton);
    QCOMPARE(m_player->tempoPercent(), 100);
    QCOMPARE(m_window->tempoValueLabel()->text(), QStringLiteral("100%"));
}

void TestMainWindow::perSongSettingsPersistAcrossWindows()
{
    QVERIFY(m_window->openSong(m_songPath));
    QTest::mouseClick(m_window->keyDownButton(), Qt::LeftButton);
    QTest::mouseClick(m_window->keyDownButton(), Qt::LeftButton);
    for (int i = 0; i < 3; ++i)
        QTest::mouseClick(m_window->tempoDownButton(), Qt::LeftButton);
    QCOMPARE(m_player->keySemitones(), -2);
    QCOMPARE(m_player->tempoPercent(), 94);

    QVERIFY(m_window->openSong(m_shortSongPath));
    QCOMPARE(m_player->keySemitones(), 0);
    QCOMPARE(m_player->tempoPercent(), 100);
    QTest::mouseClick(m_window->keyUpButton(), Qt::LeftButton);
    QTest::mouseClick(m_window->tempoUpButton(), Qt::LeftButton);
    QCOMPARE(m_player->keySemitones(), 1);
    QCOMPARE(m_player->tempoPercent(), 102);

    QVERIFY(m_window->openSong(m_songPath));
    QCOMPARE(m_player->keySemitones(), -2);
    QCOMPARE(m_player->tempoPercent(), 94);

    m_window.reset();
    m_player.reset();
    m_store.reset();
    createWindow(false);
    QVERIFY(m_window->openSong(m_songPath));
    QCOMPARE(m_player->keySemitones(), -2);
    QCOMPARE(m_player->tempoPercent(), 94);

    QTest::mouseClick(m_window->keyResetButton(), Qt::LeftButton);
    QTest::mouseClick(m_window->tempoResetButton(), Qt::LeftButton);
    m_window.reset();
    m_player.reset();
    m_store.reset();
    createWindow(false);
    QVERIFY(m_window->openSong(m_songPath));
    QCOMPARE(m_player->keySemitones(), 0);
    QCOMPARE(m_player->tempoPercent(), 100);
}

void TestMainWindow::playShowsLyricsPage()
{
    startPlaying();
    LyricsView* lyrics = m_window->lyricsView();
    QVERIFY(lyrics->isVisible());
    QVERIFY(m_window->lyricsVisible());
    QVERIFY(!lyrics->isWindow());
    QCOMPARE(lyrics->window(), m_window.get());
    QCOMPARE(m_window->findChild<QStackedWidget*>()->currentWidget(), lyrics);
    QCOMPARE(lyrics->cursor().shape(), Qt::BlankCursor);
    QCOMPARE(QApplication::focusWidget(), m_window.get());
}

void TestMainWindow::escapeReturnsToControlsWithoutTouchingAudio()
{
    startPlaying();
    QSignalSpy states(m_player.get(), &KaraokePlayer::stateChanged);
    LyricsView* lyrics = m_window->lyricsView();
    QCOMPARE(QApplication::focusWidget(), m_window.get());
    QTest::qWait(300);
    const qint64 before = m_player->positionMs();

    QTest::keyClick(m_window->windowHandle(), Qt::Key_Escape);
    QVERIFY(!lyrics->isVisible());
    QVERIFY(m_window->isVisible());
    QCOMPARE(m_player->state(), State::Playing);

    // Audio keeps running and was never restarted.
    QTest::qWait(400);
    m_player->tick();
    QVERIFY(m_player->positionMs() > before + 200);
    QCOMPARE(states.count(), 0);
}

void TestMainWindow::keysNeverClickButtons()
{
    std::vector<std::unique_ptr<QSignalSpy>> clicks;
    const QList<QAbstractButton*> buttons = m_window->findChildren<QAbstractButton*>();
    QVERIFY(buttons.contains(m_window->lyricsButton()));
    QVERIFY(buttons.contains(m_window->settingsButton()));
    for (QAbstractButton* button : buttons) {
        QCOMPARE(button->focusPolicy(), Qt::NoFocus);
        clicks.push_back(std::make_unique<QSignalSpy>(button, &QAbstractButton::clicked));
    }
    QVERIFY(m_window->openSong(m_songPath));
    m_player->play();
    QSignalSpy states(m_player.get(), &KaraokePlayer::stateChanged);
    for (bool paused : {false, true}) {
        if (paused) {
            m_player->pause();
            QVERIFY(m_window->lyricsVisible()); // Pause preserves the current page.
            states.clear();
        }
        const QImage frame = m_player->currentFrame();
        for (const auto key : {Qt::Key_Return, Qt::Key_Enter}) {
            for (const auto modifiers : {Qt::NoModifier, Qt::KeypadModifier}) {
                QTest::keyClick(m_window->windowHandle(), Qt::Key_Escape);
                QVERIFY(!m_window->lyricsVisible());
                QCOMPARE(QApplication::focusWidget(), m_window.get());
                QTest::keyClick(m_window->windowHandle(), Qt::Key_Space);
                QTest::keyClick(m_window->windowHandle(), Qt::Key_A);
                QVERIFY(!m_window->lyricsVisible());
                QTest::keyClick(m_window->windowHandle(), key, modifiers);
                QVERIFY(m_window->lyricsVisible());
                QCOMPARE(QApplication::focusWidget(), m_window.get());
                QTest::keyClick(m_window->windowHandle(), Qt::Key_Space);
                QTest::keyClick(m_window->windowHandle(), Qt::Key_A);
                QTest::keyClick(m_window->windowHandle(), key, modifiers);
                QVERIFY(m_window->lyricsVisible());
                QCOMPARE(m_player->state(), paused ? State::Paused : State::Playing);
                if (paused)
                    QCOMPARE(m_window->lyricsView()->frame(), frame);
            }
        }
        QCOMPARE(states.count(), 0);
    }
    for (const auto& spy : clicks)
        QCOMPARE(spy->count(), 0);
}

void TestMainWindow::enterDoesNothingWhenNotPlaying()
{
    auto check = [&]() {
        const auto state = m_player->state();
        for (const auto key : {Qt::Key_Return, Qt::Key_Enter}) {
            for (const auto modifiers : {Qt::NoModifier, Qt::KeypadModifier}) {
                QTest::keyClick(m_window->windowHandle(), key, modifiers);
                QCOMPARE(m_player->state(), state);
                QVERIFY(!m_window->lyricsVisible());
            }
        }
    };
    check(); // Empty
    QVERIFY(m_window->openSong(m_shortSongPath));
    check(); // Ready
    m_player->stop();
    check(); // Stopped
    m_player->play();
    QTRY_COMPARE_WITH_TIMEOUT(m_player->state(), State::Finished, 5000);
    check();
    m_player->play();
    QVERIFY(m_player->postError());
    m_player->tick();
    QCOMPARE(m_player->state(), State::Error);
    check();
}

void TestMainWindow::pauseShowsResume()
{
    startPlaying();
    QTest::keyClick(m_window->windowHandle(), Qt::Key_Escape);
    QTest::mouseClick(m_window->pauseButton(), Qt::LeftButton);
    QCOMPARE(m_player->state(), State::Paused);
    QCOMPARE(m_window->playButton()->text(), QStringLiteral("Resume"));
    QVERIFY(!m_window->pauseButton()->isEnabled());
    QVERIFY(m_window->statusText().contains("Paused"));
    const qint64 pausedAt = m_player->positionMs();
    const QImage pausedFrame = m_player->currentFrame();

    // Showing the lyrics while paused keeps the paused picture.
    QTest::keyClick(m_window->windowHandle(), Qt::Key_Return);
    QVERIFY(m_window->lyricsView()->isVisible());
    QCOMPARE(m_player->state(), State::Paused);
    QCOMPARE(m_window->lyricsView()->frame(), pausedFrame);
    QTest::keyClick(m_window->windowHandle(), Qt::Key_Escape);

    QTest::mouseClick(m_window->playButton(), Qt::LeftButton);
    QCOMPARE(m_player->state(), State::Playing);
    QCOMPARE(m_window->playButton()->text(), QStringLiteral("Play"));
    QVERIFY(m_window->lyricsView()->isVisible());
    QTest::qWait(200);
    m_player->tick();
    QVERIFY(m_player->positionMs() >= pausedAt);
}

void TestMainWindow::stopReturnsToControlsAndResets()
{
    startPlaying();
    QTest::qWait(500);
    QTest::keyClick(m_window->windowHandle(), Qt::Key_Escape);
    QTest::mouseClick(m_window->stopButton(), Qt::LeftButton);
    QCOMPARE(m_player->state(), State::Stopped);
    QCOMPARE(m_player->positionMs(), 0);
    QCOMPARE(m_player->decoder().packetsApplied(), 0u);
    QVERIFY(!m_window->lyricsView()->isVisible());
    QVERIFY(m_window->statusText().contains("Stopped"));
    QCOMPARE(m_window->lyricsView()->frame(), m_player->currentFrame());

    QTest::mouseClick(m_window->playButton(), Qt::LeftButton);
    QCOMPARE(m_player->state(), State::Playing);
    QTest::qWait(200);
    m_player->tick();
    QVERIFY(m_player->positionMs() > 100);
    QVERIFY(m_player->positionMs() < 600);
}

void TestMainWindow::endOfTrackReturnsToControls()
{
    QVERIFY(m_window->openSong(m_shortSongPath));  // opened via its .cdg
    QTest::mouseClick(m_window->playButton(), Qt::LeftButton);
    QVERIFY(m_window->lyricsView()->isVisible());
    QTRY_COMPARE_WITH_TIMEOUT(m_player->state(), State::Finished, 5000);
    QVERIFY(!m_window->lyricsView()->isVisible());
    QVERIFY(m_window->isVisible());
    QVERIFY(m_window->statusText().contains("Finished"));
    QVERIFY(m_window->playButton()->isEnabled());
}

void TestMainWindow::openingAnotherSongStopsPrevious()
{
    startPlaying();
    QTest::qWait(400);
    QVERIFY(m_window->openSong(m_shortSongPath));
    QCOMPARE(m_player->state(), State::Ready);
    QCOMPARE(m_player->positionMs(), 0);
    QVERIFY(m_player->song().mp3Path.endsWith("Short Song.mp3"));
    QVERIFY(!m_window->lyricsView()->isVisible());
    QVERIFY(m_window->statusText().contains("Ready"));
}

void TestMainWindow::missingCompanionIsReported()
{
    const QString lonely = m_dir.filePath("Lonely.mp3");
    QVERIFY(QFile::copy(m_songPath, lonely));
    QVERIFY(!m_window->openSong(lonely));
    QVERIFY(m_window->statusText().contains("missing"));
    QCOMPARE(m_player->state(), State::Empty);
}

void TestMainWindow::failedOpenKeepsPlaying_data()
{
    QTest::addColumn<bool>("garbageCdg");
    QTest::newRow("missing companion") << false;
    QTest::newRow("garbage CDG") << true;
}

void TestMainWindow::failedOpenKeepsPlaying()
{
    QFETCH(bool, garbageCdg);
    const QString base = garbageCdg ? "Garbage" : "Missing";
    const QString path = m_dir.filePath(base + ".mp3");
    QVERIFY(QFile::copy(m_songPath, path));
    if (garbageCdg)
        QVERIFY(testmedia::writeFile(m_dir.filePath(base + ".cdg"), QByteArray(24000, '\x5a')));
    startPlaying();
    QTRY_VERIFY_WITH_TIMEOUT(m_player->positionMs() > 100, 3000);
    m_player->setKeySemitones(-2);
    m_player->setTempoPercent(94);
    const qint64 before = m_player->positionMs();
    QSignalSpy states(m_player.get(), &KaraokePlayer::stateChanged);
    QVERIFY(!m_window->openSong(path));
    QCOMPARE(m_player->state(), State::Playing);
    QCOMPARE(m_player->song().mp3Path, m_songPath);
    QCOMPARE(m_player->keySemitones(), -2);
    QCOMPARE(m_player->tempoPercent(), 94);
    QCOMPARE(m_player->positionMs(), before);
    QCOMPARE(states.count(), 0);
    QVERIFY(!m_window->lyricsVisible());
    QVERIFY(m_window->statusText().contains(garbageCdg ? "empty or damaged" : "missing"));
    QTRY_VERIFY_WITH_TIMEOUT(m_player->positionMs() > before + 100, 1500);
    QCOMPARE(m_player->state(), State::Playing);
}

void TestMainWindow::rejectedOpenErrorClearsOnStateChange_data()
{
    QTest::addColumn<bool>("finishNaturally");
    QTest::newRow("stop") << false;
    QTest::newRow("finish") << true;
}

void TestMainWindow::rejectedOpenErrorClearsOnStateChange()
{
    QFETCH(bool, finishNaturally);
    QTemporaryDir badDir;
    const QString path = badDir.filePath("Damaged.mp3");
    QVERIFY(QFile::copy(m_songPath, path));
    QVERIFY(testmedia::writeFile(badDir.filePath("Damaged.cdg"), QByteArray(24000, '\x5a')));
    m_window->setShowErrorDialogs(true);
    startPlaying();
    QTRY_VERIFY_WITH_TIMEOUT(m_player->positionMs() > 100, 3000);
    QSignalSpy states(m_player.get(), &KaraokePlayer::stateChanged);
    QVERIFY(!m_window->openSong(path));
    const QString rejectedError = m_window->statusText();
    QVERIFY(rejectedError.contains("empty or damaged"));
    QCOMPARE(m_player->state(), State::Playing);
    QCOMPARE(m_player->song().mp3Path, m_songPath);
    QCOMPARE(states.count(), 0);

    const auto boxes = m_window->findChildren<QMessageBox*>();
    QCOMPARE(boxes.size(), 1);
    auto* box = boxes.first();
    QTRY_COMPARE(QApplication::activeModalWidget(), box);
    QTRY_VERIFY(QApplication::focusWidget() && QApplication::focusWidget()->window() == box);
    QTest::mouseClick(box->button(QMessageBox::Ok), Qt::LeftButton);
    QTRY_VERIFY(!QApplication::activeModalWidget());
    // Offscreen has no window manager to reactivate the parent after a dialog.
    // Request activation, leaving focus restoration to MainWindow.
    if (QGuiApplication::platformName() == QStringLiteral("offscreen"))
        m_window->activateWindow();
    QTRY_COMPARE(QApplication::focusWidget(), m_window.get());
    QCOMPARE(m_window->statusText(), rejectedError);

    // Use the focus-based window delivery path after dismissing the real dialog.
    QTest::keyClick(m_window->windowHandle(), Qt::Key_Return);
    QVERIFY(m_window->lyricsVisible());
    QTest::keyClick(m_window->windowHandle(), Qt::Key_Escape);
    QVERIFY(!m_window->lyricsVisible());
    QCOMPARE(QApplication::focusWidget(), m_window.get());
    QCOMPARE(m_player->state(), State::Playing);
    QCOMPARE(states.count(), 0);
    QCOMPARE(m_window->statusText(), rejectedError);

    if (finishNaturally) {
        QTRY_COMPARE_WITH_TIMEOUT(m_player->state(), State::Finished, 6000);
        QCOMPARE(m_window->statusText(), QStringLiteral("Finished. Press Play to sing it again."));
    } else {
        QTest::mouseClick(m_window->stopButton(), Qt::LeftButton);
        QCOMPARE(m_player->state(), State::Stopped);
        QCOMPARE(m_window->statusText(), QStringLiteral("Stopped. Press Play to start from the beginning."));
    }
}

void TestMainWindow::playbackErrorKeepsSpecificMessage()
{
    startPlaying();
    QSignalSpy errors(m_player.get(), &KaraokePlayer::errorOccurred);
    QVERIFY(m_player->postError());
    m_player->tick();
    QCOMPARE(m_player->state(), State::Error);
    QCOMPARE(errors.count(), 1);
    const QString expected = QStringLiteral("The music file for \"Long Song\" could not be played. "
                                           "It may be damaged or in an unsupported format.");
    QCOMPARE(errors.first().first().toString(), expected);
    QCOMPARE(m_window->statusText(), expected);
    m_player->tick();
    QCOMPARE(m_window->statusText(), expected);
}

void TestMainWindow::clickingLyricsReturnsToControls()
{
    startPlaying();
    QTRY_VERIFY_WITH_TIMEOUT(m_player->positionMs() > 100, 3000);
    QSignalSpy states(m_player.get(), &KaraokePlayer::stateChanged);
    const qint64 before = m_player->positionMs();
    QTest::mouseClick(m_window->lyricsView(), Qt::LeftButton);
    QVERIFY(!m_window->lyricsVisible());
    QCOMPARE(m_player->state(), State::Playing);
    QCOMPARE(states.count(), 0);
    QVERIFY(m_window->cursor().shape() != Qt::BlankCursor);
    QTRY_VERIFY_WITH_TIMEOUT(m_player->positionMs() > before + 100, 1500);
    QTest::keyClick(m_window->windowHandle(), Qt::Key_Return);
    QVERIFY(m_window->lyricsVisible());
}

class WindowStateCounter : public QObject {
public:
    int changes = 0;
protected:
    bool eventFilter(QObject*, QEvent* event) override
    {
        if (event->type() == QEvent::WindowStateChange)
            ++changes;
        return false;
    }
};

void TestMainWindow::pageSwitchesNeverChangeWindowState()
{
    // init() has already shown fullscreen and processed its initial events.
    WindowStateCounter counter;
    m_window->installEventFilter(&counter);
    const auto initialState = m_window->windowState();
    startPlaying();
    for (int i = 0; i < 3; ++i) {
        QTest::keyClick(m_window->windowHandle(), Qt::Key_Escape);
        QTest::keyClick(m_window->windowHandle(), Qt::Key_Return);
    }
    m_player->stop();
    QVERIFY(!m_window->lyricsVisible());
    QVERIFY(m_window->openSong(m_shortSongPath));
    m_player->play();
    QTRY_COMPARE_WITH_TIMEOUT(m_player->state(), State::Finished, 5000);
    QVERIFY(!m_window->lyricsVisible());
    m_player->play();
    QVERIFY(m_player->postError());
    m_player->tick();
    QCOMPARE(m_player->state(), State::Error);
    QVERIFY(!m_window->lyricsVisible());
    QCOMPARE(m_window->windowState(), initialState);
    QCOMPARE(counter.changes, 0);
    QVERIFY(m_window->isVisible());
}

void TestMainWindow::lyricsRestoreFullscreenIfUserLeftIt()
{
    startPlaying();
    QTest::keyClick(m_window->windowHandle(), Qt::Key_Escape);
    m_window->showNormal();
    // Settle the native window transition before counting Enter's changes.
    QCoreApplication::processEvents();
    QVERIFY(!m_window->isFullScreen());
    WindowStateCounter counter;
    m_window->installEventFilter(&counter);
    QTest::keyClick(m_window->windowHandle(), Qt::Key_Return);
    QVERIFY(m_window->isFullScreen());
    QVERIFY(m_window->lyricsVisible());
    QCOMPARE(counter.changes, 1);
    QCOMPARE(m_player->state(), State::Playing);
}

void TestMainWindow::errorDialogAppearsExactlyOnce()
{
    m_window->setShowErrorDialogs(true);
    const QString path = m_dir.filePath("Broken.mp3");
    QVERIFY(testmedia::writeFile(path, QByteArray(20000, '\x5a')));
    QVERIFY(testmedia::writeCdg(m_dir.filePath("Broken.cdg"), testmedia::markerCdg(1000, 300)));
    QSignalSpy errors(m_player.get(), &KaraokePlayer::errorOccurred);
    QVERIFY(m_window->openSong(path));
    QTRY_COMPARE_WITH_TIMEOUT(m_player->state(), State::Error, 3000);
    QTest::qWait(200);
    QCOMPARE(errors.count(), 1);
    const auto boxes = m_window->findChildren<QMessageBox*>();
    QCOMPARE(boxes.size(), 1);
    QVERIFY(boxes.first()->isVisible());
    QVERIFY(!m_window->lyricsVisible());
    boxes.first()->close();
}

void TestMainWindow::modalDialogKeysStayInDialog_data()
{
    QTest::addColumn<bool>("fileDialog");
    QTest::addColumn<int>("key");
    for (bool file : {false, true}) {
        QTest::newRow(file ? "file escape" : "message escape") << file << int(Qt::Key_Escape);
        QTest::newRow(file ? "file return" : "message return") << file << int(Qt::Key_Return);
    }
}

void TestMainWindow::modalDialogKeysStayInDialog()
{
    QFETCH(bool, fileDialog);
    QFETCH(int, key);
    startPlaying();
    QTest::keyClick(m_window->windowHandle(), Qt::Key_Escape);
    std::unique_ptr<QDialog> dialog;
    if (fileDialog) {
        auto* file = new QFileDialog(m_window.get());
        file->setOption(QFileDialog::DontUseNativeDialog);
        file->setFileMode(QFileDialog::ExistingFile);
        file->selectFile(m_songPath);
        dialog.reset(file);
    } else {
        dialog = std::make_unique<QMessageBox>(QMessageBox::Warning, "Test", "Test error",
                                               QMessageBox::Ok, m_window.get());
    }
    QSignalSpy finished(dialog.get(), &QDialog::finished);
    dialog->open();
    QTRY_COMPARE(QApplication::activeModalWidget(), dialog.get());
    QTRY_VERIFY(QApplication::focusWidget() && QApplication::focusWidget()->window() == dialog.get());
    QTest::keyClick(dialog->windowHandle(), static_cast<Qt::Key>(key));
    QTRY_COMPARE(finished.count(), 1);
    QVERIFY(!m_window->lyricsVisible());
    QCOMPARE(m_player->state(), State::Playing);
    dialog->close();
}

void TestMainWindow::exitStopsAndCloses()
{
    startPlaying();
    QTest::keyClick(m_window->windowHandle(), Qt::Key_Escape);
    // Settings > General > Quit Application.
    SettingsDialog* settings = m_window->openSettings();
    settings->showPage(QStringLiteral("General"));
    QPushButton* quit = nullptr;
    for (QPushButton* button : settings->findChildren<QPushButton*>()) {
        if (button->text() == QStringLiteral("Quit Application"))
            quit = button;
    }
    QVERIFY(quit);
    QTest::mouseClick(quit, Qt::LeftButton);
    QTRY_VERIFY(!m_window->isVisible());
    QCOMPARE(m_player->state(), State::Stopped);
    QCOMPARE(m_player->positionMs(), 0);
    QVERIFY(!m_window->displaySleepBlocked());
}

void TestMainWindow::noExitButtonOnTheMainScreen()
{
    // The window's close button, the Exit shortcut and Settings close the
    // program; the main screen has no Exit button, in full screen or not.
    const auto exitButtons = [this] {
        int count = 0;
        for (QAbstractButton* button : m_window->findChildren<QAbstractButton*>()) {
            const QString text = button->text().remove(QLatin1Char('&'));
            if (text.compare(QStringLiteral("Exit"), Qt::CaseInsensitive) == 0
                || text.compare(QStringLiteral("Quit"), Qt::CaseInsensitive) == 0)
                ++count;
        }
        return count;
    };
    QVERIFY(m_window->isFullScreen());
    QCOMPARE(exitButtons(), 0);
    m_window->showNormal();
    QTRY_VERIFY(!m_window->isFullScreen());
    QCOMPARE(exitButtons(), 0);
    QVERIFY(m_window->lyricsButton()->isVisible());
    QVERIFY(m_window->settingsButton()->isVisible());

    // Full screen still leads back to the controls with Escape.
    startPlaying();
    QTRY_VERIFY(m_window->isFullScreen());
    QVERIFY(m_window->lyricsVisible());
    QTest::keyClick(m_window->windowHandle(), Qt::Key_Escape);
    QVERIFY(!m_window->lyricsVisible());
    QCOMPARE(m_player->state(), State::Playing);
    QTest::keyClick(m_window->windowHandle(), Qt::Key_Return);
    QVERIFY(m_window->lyricsVisible());
    QTest::keyClick(m_window->windowHandle(), Qt::Key_Escape);

    // The Exit shortcut closes the program.
    QAction* exit = m_window->shortcutAction(QStringLiteral("app.exit"));
    QVERIFY(exit);
    exit->trigger();
    QTRY_VERIFY(!m_window->isVisible());
    QCOMPARE(m_player->state(), State::Stopped);
}

void TestMainWindow::displaySleepFollowsPlayerStates()
{
    QVERIFY(!m_window->displaySleepBlocked()); // Empty
    QVERIFY(m_window->openSong(m_shortSongPath));
    QVERIFY(!m_window->displaySleepBlocked()); // Ready
    m_player->play();
    QVERIFY(m_window->displaySleepBlocked());
    m_player->pause();
    QVERIFY(!m_window->displaySleepBlocked());
    m_player->play();
    QVERIFY(m_window->displaySleepBlocked());
    m_player->stop();
    QVERIFY(!m_window->displaySleepBlocked());
    m_player->play();
    QVERIFY(m_window->displaySleepBlocked());
    QTRY_COMPARE_WITH_TIMEOUT(m_player->state(), State::Finished, 5000);
    QVERIFY(!m_window->displaySleepBlocked());
    m_player->play();
    QVERIFY(m_window->displaySleepBlocked());
    QVERIFY(m_player->postError());
    m_player->tick();
    QCOMPARE(m_player->state(), State::Error);
    QVERIFY(!m_window->displaySleepBlocked());
    m_player->play();
    QVERIFY(m_window->displaySleepBlocked());
    QVERIFY(m_window->openSong(m_songPath)); // Replacement passes through Empty.
    QVERIFY(!m_window->displaySleepBlocked());
}

void TestMainWindow::displaySleepReleasedOnDestruction()
{
    startPlaying();
    QVERIFY(m_window->displaySleepBlocked());
    QTest::ignoreMessage(QtInfoMsg, "Display sleep blocker released");
    m_window.reset();
}

QTEST_MAIN(TestMainWindow)
#include "tst_mainwindow.moc"
