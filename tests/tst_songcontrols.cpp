// What Play, Key, Tempo and Now Playing act on: the song being sung, else the
// song selected (Key/Tempo before it plays); and the song key shown the same
// way in the library, the playlists and Now Playing.
#include "AppPreferences.h"
#include "BusTestPlayer.h"
#include "LibraryController.h"
#include "LibraryResultsModel.h"
#include "LibraryView.h"
#include "MainWindow.h"
#include "PlaylistView.h"
#include "SettingsDialog.h"
#include "SongPair.h"
#include "SongSettings.h"
#include "TestMedia.h"
#include "playlist/PlaylistPlayback.h"
#include "playlist/PlaylistStore.h"

#include <QAction>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTreeView>
#include <QtTest>

#include <memory>

namespace {

using State = KaraokePlayer::State;

constexpr int kLongMs = 8000;
constexpr int kShortMs = 1200;  // the longest of the short songs

bool writeSong(const QString& base, int durationMs)
{
    return testmedia::writeMp3(base + QStringLiteral(".mp3"), durationMs)
        && testmedia::writeCdg(base + QStringLiteral(".cdg"),
                               testmedia::markerCdg(durationMs, qMax(50, durationMs / 3)));
}

// The song files' Key/Tempo identity, as the player works it out.
QString identityOf(const QString& mp3)
{
    return songIdentity(resolveSongPair(mp3).pair);
}

// A library of a few songs, a playlist store and the program's window over
// them, as the program sets them up.
struct Studio {
    QTemporaryDir temporary;
    QString root;
    QMap<QString, QString> mp3;   // short name -> MP3 path
    QMap<QString, qint64> ids;    // short name -> catalogue song
    std::unique_ptr<LibraryController> controller;
    std::unique_ptr<PlaylistStore> playlists;
    std::unique_ptr<SongSettingsStore> settings;
    std::unique_ptr<BusTestPlayer> player;
    QHash<QString, QString> stored;  // the preferences, kept in memory
    std::unique_ptr<AppPreferences> preferences;
    std::unique_ptr<MainWindow> window;

    QString settingsPath() const { return temporary.filePath(QStringLiteral("app/song-settings.json")); }

    bool setUp()
    {
        root = temporary.filePath(QStringLiteral("music"));
        const QList<std::tuple<QString, QString, int>> songs = {
            {QStringLiteral("alpha"), QStringLiteral("SC001-01 - Test Singer - Alpha Song"), kLongMs},
            // Lengths differ, so every song has an identity of its own.
            {QStringLiteral("bravo"), QStringLiteral("SC001-02 - Test Singer - Bravo Song"), kLongMs + 200},
            {QStringLiteral("charlie"), QStringLiteral("SC001-03 - Test Singer - Charlie Song"), kShortMs - 100},
            {QStringLiteral("delta"), QStringLiteral("SC001-04 - Test Singer - Delta Song"), kShortMs},
        };
        if (!QDir().mkpath(root))
            return false;
        for (const auto& [name, file, duration] : songs) {
            if (!writeSong(root + QLatin1Char('/') + file, duration))
                return false;
            mp3.insert(name, root + QLatin1Char('/') + file + QStringLiteral(".mp3"));
        }
        controller = std::make_unique<LibraryController>(
            temporary.filePath(QStringLiteral("app/library.sqlite")), QString(),
            temporary.filePath(QStringLiteral("app/metadata-overrides.sqlite")));
        QSignalSpy finished(controller.get(), &LibraryController::scanFinished);
        if (!controller->chooseRoot(root) || (finished.isEmpty() && !finished.wait(20000)))
            return false;
        for (const auto& [name, file, duration] : songs) {
            Q_UNUSED(duration)
            const QString title = file.section(QStringLiteral(" - "), 2);
            const auto rows = controller->search(title, 10);
            if (rows.size() != 1)
                return false;
            ids.insert(name, rows.first().songId);
        }
        playlists = std::make_unique<PlaylistStore>(temporary.filePath(QStringLiteral("app/playlists.sqlite")));
        return playlists->open(nullptr, controller->libraryRoots()) && playlists->setAutoplay(false);
    }

    void openWindow()
    {
        window.reset();
        player.reset();
        settings = std::make_unique<SongSettingsStore>(settingsPath());
        player = std::make_unique<BusTestPlayer>();
        preferences = std::make_unique<AppPreferences>(
            [this](const QString& key) { return stored.value(key); },
            [this](const QString& key, const QString& value) {
                stored.insert(key, value);
                return true;
            });
        window = std::make_unique<MainWindow>(player.get(), settings.get(), controller.get(),
                                              playlists.get(), preferences.get());
        window->setShowErrorDialogs(false);
        window->resize(1280, 800);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
    }

    LibraryView* library() const { return window->libraryView(); }
    PlaylistView* playlist() const { return window->playlistView(); }

    // Shows the whole library and clicks the song's row, as Frankie would.
    void clickLibrarySong(const QString& name)
    {
        library()->searchBox()->clear();
        library()->refreshSearch();
        QTreeView* results = library()->resultsList();
        int row = -1;
        for (int r = 0; r < results->model()->rowCount(); ++r) {
            if (results->model()->index(r, 0).data(LibraryResultsModel::SongIdRole).toLongLong()
                == ids.value(name))
                row = r;
        }
        QVERIFY(row >= 0);
        const QRect rect = results->visualRect(results->model()->index(row, 0));
        QTest::mouseClick(results->viewport(), Qt::LeftButton, {}, rect.center());
        QCOMPARE(library()->selectedSongId(), ids.value(name));
        QVERIFY(library()->isActive());
    }

    void clickPlaylistRow(int row)
    {
        QListWidget* list = playlist()->itemList();
        QVERIFY(row < list->count());
        list->scrollToItem(list->item(row));
        QTest::mouseClick(list->viewport(), Qt::LeftButton, {},
                          list->visualItemRect(list->item(row)).center());
        QCOMPARE(list->currentRow(), row);
        QVERIFY(playlist()->isActive());
    }

    // The Key shown in the library row of a song ("" for none).
    QString libraryKey(const QString& name) const
    {
        QAbstractItemModel* model = library()->resultsList()->model();
        for (int r = 0; r < model->rowCount(); ++r) {
            const QModelIndex index = model->index(r, LibraryResultsModel::KeyColumn);
            if (index.data(LibraryResultsModel::SongIdRole).toLongLong() == ids.value(name))
                return index.data().toString();
        }
        return QStringLiteral("<no row>");
    }

    qint64 addToPlaylist(qint64 playlistId, const QString& name)
    {
        qint64 itemId = 0;
        const auto ref = controller->songRef(ids.value(name));
        if (!ref || !playlists->addItem(playlistId, *ref, &itemId))
            return 0;
        return itemId;
    }

    // Scans the music folder again, as after songs were moved or removed.
    bool rescan()
    {
        QSignalSpy finished(controller.get(), &LibraryController::scanFinished);
        controller->requestRefreshScan();
        return finished.wait(20000);
    }

    // Forget Every Song's Key and Tempo, in Settings > Advanced.
    void forgetAll()
    {
        SettingsDialog* dialog = window->openSettings();
        dialog->showPage(QStringLiteral("Advanced"));
        dialog->setForgetConfirmation([] { return true; });
        QPushButton* forget = nullptr;
        for (QPushButton* button : dialog->findChildren<QPushButton*>()) {
            if (button->text() == QStringLiteral("Forget Every Song's Key and Tempo\u2026"))
                forget = button;
        }
        QVERIFY(forget);
        forget->click();
        QVERIFY(!QFileInfo::exists(settingsPath()) || !savedSettings(QStringLiteral("alpha")));
        dialog->close();
    }

    // The song's remembered Key/Tempo as a fresh store reads them from disk.
    std::optional<SongSettings> savedSettings(const QString& name) const
    {
        SongSettingsStore onDisk(settingsPath());
        const QString identity = identityOf(mp3.value(name));
        if (identity.isEmpty() || !onDisk.hasSettingsFor(identity))
            return std::nullopt;
        return onDisk.settingsFor(identity);
    }
};

// Back from the lyrics to the home screen (Escape), as Frankie would.
void showHome(MainWindow& window)
{
    if (window.lyricsVisible())
        QTest::keyClick(&window, Qt::Key_Escape);
    QVERIFY(!window.lyricsVisible());
}

QString songText(const QString& title)
{
    return QStringLiteral("Test Singer – ") + title;
}

} // namespace

class TestSongControls : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void selectedSongTransposesBeforeItPlays();
    void selectedPlaylistSongTransposesBeforeItPlays();
    void theSongBeingSungKeepsTheControls();
    void preplayKeyAndTempoSurviveARestart();
    void playStartsTheSelectedSongOnceNothingIsSung();
    void keyAndTempoNeedASong();
    void nowPlayingShowsOnlyWhatIsPlaying();
    void nowPlayingFollowsAutoplayAndClearsAtTheEnd();
    void previewLeavesNoNowPlaying();
    void songKeyIsShownTheSameEverywhere();
    void playFromThePlaylistCarriesOnWithAutoplay();
    void lyricsLeftUpNeverStartTheSelection();
    void forgettingEverySongResetsTheLoadedSong();
    void forgettingWhileSingingShowsDefaultsOnceStopped();
    void aMovedSongKeepsItsKeyOnceItIsPlayed();
    void aPausedPreviewCanCarryOn();
    void aMissingPlaylistSongStillPlaysThroughThePlaylist();
    void restartSongRestartsTheLoadedSong();
    void anUnsavedPreplayChangeSaysSo();
};

void TestSongControls::initTestCase()
{
    QString error;
    QVERIFY2(KaraokePlayer::initializeGStreamer(&error), qPrintable(error));
}

void TestSongControls::selectedSongTransposesBeforeItPlays()
{
    Studio studio;
    QVERIFY(studio.setUp());
    QString error;
    QVERIFY2(studio.controller->setManualOriginalKey(studio.ids.value(QStringLiteral("alpha")), 0, &error),
             qPrintable(error));  // C
    studio.openWindow();
    MainWindow& window = *studio.window;

    studio.clickLibrarySong(QStringLiteral("alpha"));
    QCOMPARE(window.keyValueLabel()->text(), QStringLiteral("0"));
    QVERIFY(window.keyUpButton()->isEnabled());
    window.keyUpButton()->click();
    window.keyUpButton()->click();
    window.tempoDownButton()->click();

    // Nothing plays, nothing is loaded; the setting is saved at once.
    QCOMPARE(studio.player->state(), State::Empty);
    QVERIFY(!studio.player->hasSong());
    QVERIFY(!window.lyricsVisible());
    QCOMPARE(window.songText(), QStringLiteral("Nothing playing"));
    QCOMPARE(window.keyValueLabel()->text(), QStringLiteral("+2"));
    QCOMPARE(window.tempoValueLabel()->text(), QStringLiteral("98%"));
    const auto saved = studio.savedSettings(QStringLiteral("alpha"));
    QVERIFY(saved);
    QCOMPARE(saved->keySemitones, 2);
    QCOMPARE(saved->tempoPercent, 98);
    QCOMPARE(studio.libraryKey(QStringLiteral("alpha")), QStringLiteral("C → D"));
    QVERIFY(!window.songKeyLabel()->isVisible());

    // Play starts it at +2 (D).
    window.playButton()->click();
    QCOMPARE(studio.player->state(), State::Playing);
    QCOMPARE(studio.player->keySemitones(), 2);
    QCOMPARE(studio.player->tempoPercent(), 98);
    QCOMPARE(window.songText(), songText(QStringLiteral("Alpha Song")));
    QCOMPARE(window.songKeyLabel()->text(), QStringLiteral("Key C → D"));

    // Live Key while it plays changes what is heard, the rows and the memory.
    window.keyDownButton()->click();
    window.keyDownButton()->click();
    window.keyDownButton()->click();
    window.keyDownButton()->click();
    QCOMPARE(studio.player->keySemitones(), -2);
    QCOMPARE(window.songKeyLabel()->text(), QStringLiteral("Key C → Bb"));
    QCOMPARE(studio.libraryKey(QStringLiteral("alpha")), QStringLiteral("C → Bb"));
    QCOMPARE(studio.savedSettings(QStringLiteral("alpha"))->keySemitones, -2);
    // The original key is never changed by Key.
    QCOMPARE(studio.controller->songKeyDetails(studio.ids.value(QStringLiteral("alpha")))->manualKeyIndex, 0);
}

void TestSongControls::selectedPlaylistSongTransposesBeforeItPlays()
{
    Studio studio;
    QVERIFY(studio.setUp());
    QString error;
    QVERIFY2(studio.controller->setManualOriginalKey(studio.ids.value(QStringLiteral("bravo")), 7, &error),
             qPrintable(error));  // G
    qint64 playlistId = 0;
    QVERIFY(studio.playlists->createPlaylist(QStringLiteral("Party"), &playlistId));
    QVERIFY(studio.addToPlaylist(playlistId, QStringLiteral("alpha")));
    QVERIFY(studio.addToPlaylist(playlistId, QStringLiteral("bravo")));
    studio.openWindow();
    MainWindow& window = *studio.window;
    QTRY_VERIFY(studio.playlist()->keyColumnShown());

    studio.clickPlaylistRow(1);
    window.keyDownButton()->click();
    QCOMPARE(studio.player->state(), State::Empty);
    QCOMPARE(window.songText(), QStringLiteral("Nothing playing"));
    QCOMPARE(window.keyValueLabel()->text(), QStringLiteral("-1"));
    QCOMPARE(studio.savedSettings(QStringLiteral("bravo"))->keySemitones, -1);
    QVERIFY(!studio.savedSettings(QStringLiteral("alpha")));
    QCOMPARE(studio.playlist()->rowKeyText(1), QStringLiteral("G → F#"));
    QCOMPARE(studio.libraryKey(QStringLiteral("bravo")), QStringLiteral("G → F#"));

    // The keyboard's Key shortcut does the same.
    window.shortcutAction(QStringLiteral("key.down"))->trigger();
    QCOMPARE(studio.savedSettings(QStringLiteral("bravo"))->keySemitones, -2);
    QCOMPARE(studio.playlist()->rowKeyText(1), QStringLiteral("G → F"));
    QCOMPARE(studio.player->state(), State::Empty);

    // Playing the row starts it at -2.
    QTest::mouseClick(studio.playlist()->playButton(), Qt::LeftButton);
    QCOMPARE(studio.player->state(), State::Playing);
    QCOMPARE(studio.player->keySemitones(), -2);
    QCOMPARE(window.songKeyLabel()->text(), QStringLiteral("Key G → F"));
}

void TestSongControls::theSongBeingSungKeepsTheControls()
{
    Studio studio;
    QVERIFY(studio.setUp());
    qint64 playlistId = 0;
    QVERIFY(studio.playlists->createPlaylist(QStringLiteral("Party"), &playlistId));
    QVERIFY(studio.addToPlaylist(playlistId, QStringLiteral("bravo")));
    studio.openWindow();
    MainWindow& window = *studio.window;

    // A plays; B is then selected in the library, and in the playlist.
    studio.clickLibrarySong(QStringLiteral("alpha"));
    QTest::mouseClick(studio.library()->singButton(), Qt::LeftButton);
    QCOMPARE(studio.player->state(), State::Playing);
    showHome(window);
    studio.clickLibrarySong(QStringLiteral("bravo"));
    window.keyUpButton()->click();
    QCOMPARE(studio.player->keySemitones(), 1);
    QCOMPARE(window.keyValueLabel()->text(), QStringLiteral("+1"));
    QCOMPARE(studio.savedSettings(QStringLiteral("alpha"))->keySemitones, 1);
    QVERIFY(!studio.savedSettings(QStringLiteral("bravo")));
    QCOMPARE(window.songText(), songText(QStringLiteral("Alpha Song")));

    studio.clickPlaylistRow(0);  // Bravo again
    window.tempoUpButton()->click();
    QCOMPARE(studio.player->tempoPercent(), 102);
    QVERIFY(!studio.savedSettings(QStringLiteral("bravo")));

    // Paused is still being sung.
    window.pauseButton()->click();
    QCOMPARE(studio.player->state(), State::Paused);
    showHome(window);
    studio.clickLibrarySong(QStringLiteral("bravo"));
    window.keyUpButton()->click();
    QCOMPARE(studio.player->keySemitones(), 2);
    QVERIFY(!studio.savedSettings(QStringLiteral("bravo")));
    QCOMPARE(window.songText(), songText(QStringLiteral("Alpha Song")));
    QCOMPARE(window.playButton()->text(), QStringLiteral("Resume"));
    // Resume carries on with A, not the selection.
    window.playButton()->click();
    QCOMPARE(studio.player->state(), State::Playing);
    QVERIFY(QFileInfo(studio.player->song().mp3Path).fileName().contains(QStringLiteral("Alpha")));

    // Once stopped, the controls belong to the selection; A is untouched.
    window.stopButton()->click();
    QCOMPARE(studio.player->state(), State::Stopped);
    QCOMPARE(window.keyValueLabel()->text(), QStringLiteral("0"));  // Bravo's
    window.keyDownButton()->click();
    QCOMPARE(studio.savedSettings(QStringLiteral("bravo"))->keySemitones, -1);
    QCOMPARE(studio.savedSettings(QStringLiteral("alpha"))->keySemitones, 2);
    QCOMPARE(studio.player->state(), State::Stopped);
}

void TestSongControls::preplayKeyAndTempoSurviveARestart()
{
    Studio studio;
    QVERIFY(studio.setUp());
    QString error;
    QVERIFY2(studio.controller->setManualOriginalKey(studio.ids.value(QStringLiteral("bravo")), 2, &error),
             qPrintable(error));  // D
    studio.openWindow();
    studio.clickLibrarySong(QStringLiteral("bravo"));
    for (int i = 0; i < 3; ++i)
        studio.window->keyUpButton()->click();
    QCOMPARE(studio.libraryKey(QStringLiteral("bravo")), QStringLiteral("D → F"));

    // Restart: a new window and settings store read it back.
    studio.openWindow();
    QCOMPARE(studio.libraryKey(QStringLiteral("bravo")), QStringLiteral("D → F"));
    QCOMPARE(studio.window->songText(), QStringLiteral("Nothing playing"));
    studio.clickLibrarySong(QStringLiteral("bravo"));
    QCOMPARE(studio.window->keyValueLabel()->text(), QStringLiteral("+3"));
    studio.window->playButton()->click();
    QCOMPARE(studio.player->state(), State::Playing);
    QCOMPARE(studio.player->keySemitones(), 3);
    QCOMPARE(studio.window->songKeyLabel()->text(), QStringLiteral("Key D → F"));
}

void TestSongControls::playStartsTheSelectedSongOnceNothingIsSung()
{
    Studio studio;
    QVERIFY(studio.setUp());
    qint64 playlistId = 0;
    QVERIFY(studio.playlists->createPlaylist(QStringLiteral("Party"), &playlistId));
    QVERIFY(studio.addToPlaylist(playlistId, QStringLiteral("delta")));
    studio.openWindow();
    MainWindow& window = *studio.window;

    studio.clickLibrarySong(QStringLiteral("alpha"));
    window.playButton()->click();
    QCOMPARE(studio.player->state(), State::Playing);
    QVERIFY(studio.player->song().mp3Path.contains(QStringLiteral("Alpha")));
    window.stopButton()->click();

    // Stopped: Play starts the song now selected, from the library...
    studio.clickLibrarySong(QStringLiteral("bravo"));
    window.playButton()->click();
    QCOMPARE(studio.player->state(), State::Playing);
    QVERIFY(studio.player->song().mp3Path.contains(QStringLiteral("Bravo")));
    window.stopButton()->click();

    // ...or from the playlist, as its row (so Autoplay carries on from it).
    studio.clickPlaylistRow(0);
    window.playButton()->click();
    QCOMPARE(studio.player->state(), State::Playing);
    QVERIFY(studio.player->song().mp3Path.contains(QStringLiteral("Delta")));
    QVERIFY(window.playlistPlayback()->context());
    QTRY_COMPARE_WITH_TIMEOUT(studio.player->state(), State::Finished, kShortMs + 4000);
    QCOMPARE(window.songText(), QStringLiteral("Nothing playing"));

    // With no song selected, Play has nothing to start.
    studio.clickLibrarySong(QStringLiteral("bravo"));
    studio.library()->resultsList()->setCurrentIndex(QModelIndex());
    QVERIFY(!window.playButton()->isEnabled());
}

void TestSongControls::keyAndTempoNeedASong()
{
    Studio studio;
    QVERIFY(studio.setUp());
    studio.openWindow();
    MainWindow& window = *studio.window;
    QCOMPARE(studio.library()->selectedSongId(), 0);
    QVERIFY(!window.keyUpButton()->isEnabled());
    QVERIFY(!window.keyDownButton()->isEnabled());
    QVERIFY(!window.tempoUpButton()->isEnabled());
    QVERIFY(!window.playButton()->isEnabled());
    window.shortcutAction(QStringLiteral("key.up"))->trigger();
    QCOMPARE(window.keyValueLabel()->text(), QStringLiteral("0"));
    QVERIFY(!QFileInfo::exists(studio.settingsPath()));
}

void TestSongControls::nowPlayingShowsOnlyWhatIsPlaying()
{
    Studio studio;
    QVERIFY(studio.setUp());
    qint64 playlistId = 0;
    QVERIFY(studio.playlists->createPlaylist(QStringLiteral("Party"), &playlistId));
    QVERIFY(studio.addToPlaylist(playlistId, QStringLiteral("bravo")));
    studio.openWindow();
    MainWindow& window = *studio.window;

    // Start-up and single clicks: nothing.
    QCOMPARE(window.songText(), QStringLiteral("Nothing playing"));
    studio.clickLibrarySong(QStringLiteral("alpha"));
    QCOMPARE(window.songText(), QStringLiteral("Nothing playing"));
    studio.clickPlaylistRow(0);
    QCOMPARE(window.songText(), QStringLiteral("Nothing playing"));

    // Playing and paused: the song.
    studio.clickLibrarySong(QStringLiteral("alpha"));
    QTest::mouseClick(studio.library()->singButton(), Qt::LeftButton);
    QCOMPARE(studio.player->state(), State::Playing);
    QCOMPARE(window.songText(), songText(QStringLiteral("Alpha Song")));
    window.pauseButton()->click();
    QCOMPARE(window.songText(), songText(QStringLiteral("Alpha Song")));
    showHome(window);
    studio.clickPlaylistRow(0);  // a click while paused changes nothing
    QCOMPARE(window.songText(), songText(QStringLiteral("Alpha Song")));
    window.playButton()->click();  // Resume
    QCOMPARE(window.songText(), songText(QStringLiteral("Alpha Song")));

    // Stop clears it.
    window.stopButton()->click();
    QCOMPARE(studio.player->state(), State::Stopped);
    QCOMPARE(window.songText(), QStringLiteral("Nothing playing"));
    QVERIFY(!window.songKeyLabel()->isVisible());

    // A song that plays to its end (not from a playlist) clears it too.
    studio.clickLibrarySong(QStringLiteral("charlie"));
    QTest::mouseClick(studio.library()->singButton(), Qt::LeftButton);
    QCOMPARE(window.songText(), songText(QStringLiteral("Charlie Song")));
    QTRY_COMPARE_WITH_TIMEOUT(studio.player->state(), State::Finished, kShortMs + 4000);
    QCOMPARE(window.songText(), QStringLiteral("Nothing playing"));
    QCOMPARE(window.statusText(), QStringLiteral("Finished. Choose a song and press Play."));
}

void TestSongControls::nowPlayingFollowsAutoplayAndClearsAtTheEnd()
{
    Studio studio;
    QVERIFY(studio.setUp());
    QVERIFY(studio.playlists->setAutoplay(true));
    qint64 playlistId = 0;
    QVERIFY(studio.playlists->createPlaylist(QStringLiteral("Party"), &playlistId));
    QVERIFY(studio.addToPlaylist(playlistId, QStringLiteral("charlie")));
    QVERIFY(studio.addToPlaylist(playlistId, QStringLiteral("delta")));
    studio.openWindow();
    MainWindow& window = *studio.window;

    studio.clickPlaylistRow(0);
    QTest::mouseClick(studio.playlist()->playButton(), Qt::LeftButton);
    QCOMPARE(window.songText(), songText(QStringLiteral("Charlie Song")));
    // The next song replaces it as soon as it starts.
    QTRY_COMPARE_WITH_TIMEOUT(window.songText(), songText(QStringLiteral("Delta Song")), kShortMs + 4000);
    QCOMPARE(studio.player->state(), State::Playing);
    // After the last one, nothing.
    QTRY_COMPARE_WITH_TIMEOUT(studio.player->state(), State::Finished, kShortMs + 4000);
    QTest::qWait(100);
    QCOMPARE(window.songText(), QStringLiteral("Nothing playing"));
}

void TestSongControls::previewLeavesNoNowPlaying()
{
    Studio studio;
    QVERIFY(studio.setUp());
    studio.openWindow();
    MainWindow& window = *studio.window;
    QVERIFY(window.previewSong(studio.ids.value(QStringLiteral("alpha"))));
    QVERIFY(window.isPreviewing());
    // A preview never offers Key/Tempo and never shows a key.
    QVERIFY(!window.keyUpButton()->isEnabled());
    QVERIFY(!window.songKeyLabel()->isVisible());
    window.stopPreview();
    QCOMPARE(studio.player->state(), State::Stopped);
    QCOMPARE(window.songText(), QStringLiteral("Nothing playing"));
    QVERIFY(!QFileInfo::exists(studio.settingsPath()));
}

void TestSongControls::songKeyIsShownTheSameEverywhere()
{
    Studio studio;
    QVERIFY(studio.setUp());
    const qint64 alpha = studio.ids.value(QStringLiteral("alpha"));
    QString error;
    qint64 playlistId = 0;
    QVERIFY(studio.playlists->createPlaylist(QStringLiteral("Party"), &playlistId));
    QVERIFY(studio.addToPlaylist(playlistId, QStringLiteral("alpha")));
    QVERIFY(studio.addToPlaylist(playlistId, QStringLiteral("bravo")));
    QVERIFY2(studio.controller->setManualOriginalKey(alpha, 0, &error), qPrintable(error));  // C
    studio.openWindow();
    MainWindow& window = *studio.window;
    QTRY_VERIFY(studio.playlist()->keyColumnShown());
    QCOMPARE(studio.libraryKey(QStringLiteral("alpha")), QStringLiteral("C"));
    QCOMPARE(studio.playlist()->rowKeyText(0), QStringLiteral("C"));

    // Transposed before playing: both lists at once.
    studio.clickLibrarySong(QStringLiteral("alpha"));
    window.keyUpButton()->click();
    window.keyUpButton()->click();
    QCOMPARE(studio.libraryKey(QStringLiteral("alpha")), QStringLiteral("C → D"));
    QCOMPARE(studio.playlist()->rowKeyText(0), QStringLiteral("C → D"));

    // A new original key by hand: everywhere at once, the transpose kept.
    QVERIFY2(studio.controller->setManualOriginalKey(alpha, 21, &error), qPrintable(error));  // Am
    QCOMPARE(studio.libraryKey(QStringLiteral("alpha")), QStringLiteral("Am → Bm"));
    QCOMPARE(studio.playlist()->rowKeyText(0), QStringLiteral("Am → Bm"));
    QCOMPARE(window.keyValueLabel()->text(), QStringLiteral("+2"));

    // Playing: Now Playing says the same, and follows live Key.
    window.playButton()->click();
    QCOMPARE(studio.player->state(), State::Playing);
    QCOMPARE(window.songKeyLabel()->text(), QStringLiteral("Key Am → Bm"));
    window.keyDownButton()->click();
    QCOMPARE(window.songKeyLabel()->text(), QStringLiteral("Key Am → Bbm"));
    QCOMPARE(studio.libraryKey(QStringLiteral("alpha")), QStringLiteral("Am → Bbm"));
    QCOMPARE(studio.playlist()->rowKeyText(0), QStringLiteral("Am → Bbm"));
    QCOMPARE(studio.controller->songKeyDetails(alpha)->manualKeyIndex, 21);

    // No original key: nothing shown, and no key made up from the transpose.
    QVERIFY2(studio.controller->setManualOriginalKey(alpha, std::nullopt, &error), qPrintable(error));
    QCOMPARE(studio.libraryKey(QStringLiteral("alpha")), QString());
    QCOMPARE(studio.playlist()->rowKeyText(0), QString());
    QVERIFY(!window.songKeyLabel()->isVisible());
    QCOMPARE(window.keyValueLabel()->text(), QStringLiteral("+1"));
    // A song never transposed shows its key alone.
    QCOMPARE(studio.playlist()->rowKeyText(1), QString());
    QCOMPARE(studio.libraryKey(QStringLiteral("bravo")), QString());
}

void TestSongControls::playFromThePlaylistCarriesOnWithAutoplay()
{
    Studio studio;
    QVERIFY(studio.setUp());
    QVERIFY(studio.playlists->setAutoplay(true));
    qint64 playlistId = 0;
    QVERIFY(studio.playlists->createPlaylist(QStringLiteral("Party"), &playlistId));
    const qint64 first = studio.addToPlaylist(playlistId, QStringLiteral("charlie"));
    const qint64 second = studio.addToPlaylist(playlistId, QStringLiteral("delta"));
    QVERIFY(first && second);
    studio.openWindow();
    MainWindow& window = *studio.window;
    studio.clickPlaylistRow(0);
    QSignalSpy states(studio.player.get(), &KaraokePlayer::stateChanged);
    window.playButton()->click();  // the player bar's Play, nothing being sung
    QCOMPARE(studio.player->state(), State::Playing);
    QVERIFY(window.playlistPlayback()->context());
    QCOMPARE(window.playlistPlayback()->context()->playlistId, playlistId);
    QCOMPARE(window.playlistPlayback()->context()->itemId, first);
    QTRY_COMPARE_WITH_TIMEOUT(window.playlistPlayback()->context()->itemId, second, kShortMs + 4000);
    QTRY_COMPARE_WITH_TIMEOUT(studio.player->state(), State::Finished, kShortMs + 4000);
    QTest::qWait(200);
    int starts = 0;
    for (const QList<QVariant>& change : std::as_const(states))
        starts += change.first().value<State>() == State::Playing ? 1 : 0;
    QCOMPARE(starts, 2);  // each song started once
    QCOMPARE(window.songText(), QStringLiteral("Nothing playing"));
}

void TestSongControls::lyricsLeftUpNeverStartTheSelection()
{
    Studio studio;
    QVERIFY(studio.setUp());
    studio.openWindow();
    MainWindow& window = *studio.window;
    studio.preferences->setFlag(pref::ReturnHomeAtEnd, false);  // lyrics stay up at the end
    studio.clickLibrarySong(QStringLiteral("charlie"));
    QTest::mouseClick(studio.library()->singButton(), Qt::LeftButton);
    QCOMPARE(studio.player->state(), State::Playing);
    // While it plays, another song is selected on the home screen.
    showHome(window);
    studio.clickLibrarySong(QStringLiteral("bravo"));
    window.lyricsButton()->click();
    QVERIFY(window.lyricsVisible());
    QTRY_COMPARE_WITH_TIMEOUT(studio.player->state(), State::Finished, kShortMs + 4000);
    QVERIFY(window.lyricsVisible());
    // The lyrics page's keys never reach the hidden selection.
    window.shortcutAction(QStringLiteral("playback.playPause"))->trigger();
    window.shortcutAction(QStringLiteral("key.up"))->trigger();
    QCOMPARE(studio.player->state(), State::Finished);
    QVERIFY(studio.player->song().mp3Path.contains(QStringLiteral("Charlie")));
    QVERIFY(!studio.savedSettings(QStringLiteral("bravo")));
    // Back home, Play starts it.
    showHome(window);
    window.playButton()->click();
    QCOMPARE(studio.player->state(), State::Playing);
    QVERIFY(studio.player->song().mp3Path.contains(QStringLiteral("Bravo")));
}

void TestSongControls::forgettingEverySongResetsTheLoadedSong()
{
    Studio studio;
    QVERIFY(studio.setUp());
    studio.openWindow();
    MainWindow& window = *studio.window;
    // A song opened from its file waits for Play, at +2.
    QVERIFY(window.openSong(studio.mp3.value(QStringLiteral("alpha"))));
    QCOMPARE(studio.player->state(), State::Ready);
    window.keyUpButton()->click();
    window.keyUpButton()->click();
    QCOMPARE(studio.savedSettings(QStringLiteral("alpha"))->keySemitones, 2);
    studio.forgetAll();
    QCOMPARE(studio.player->keySemitones(), 0);
    QCOMPARE(window.keyValueLabel()->text(), QStringLiteral("0"));
    window.playButton()->click();
    QCOMPARE(studio.player->state(), State::Playing);
    QCOMPARE(studio.player->keySemitones(), 0);
}

void TestSongControls::forgettingWhileSingingShowsDefaultsOnceStopped()
{
    Studio studio;
    QVERIFY(studio.setUp());
    QString error;
    QVERIFY2(studio.controller->setManualOriginalKey(studio.ids.value(QStringLiteral("alpha")), 0, &error),
             qPrintable(error));  // C
    studio.openWindow();
    MainWindow& window = *studio.window;
    studio.clickLibrarySong(QStringLiteral("alpha"));
    QTest::mouseClick(studio.library()->singButton(), Qt::LeftButton);
    window.keyUpButton()->click();
    window.keyUpButton()->click();
    QCOMPARE(studio.libraryKey(QStringLiteral("alpha")), QStringLiteral("C → D"));
    showHome(window);
    studio.forgetAll();
    // Still sung at +2 ...
    QCOMPARE(studio.player->keySemitones(), 2);
    QCOMPARE(studio.libraryKey(QStringLiteral("alpha")), QStringLiteral("C → D"));
    // ... and after Stop it starts from the default again, as shown.
    window.stopButton()->click();
    QCOMPARE(studio.libraryKey(QStringLiteral("alpha")), QStringLiteral("C"));
    QCOMPARE(window.keyValueLabel()->text(), QStringLiteral("0"));
}

void TestSongControls::aMovedSongKeepsItsKeyOnceItIsPlayed()
{
    Studio studio;
    QVERIFY(studio.setUp());
    studio.openWindow();
    studio.clickLibrarySong(QStringLiteral("alpha"));
    studio.window->keyUpButton()->click();
    studio.window->keyUpButton()->click();
    studio.window.reset();

    // The song's files are renamed in the music folder.
    const QString moved = studio.root + QStringLiteral("/SC001-05 - Test Singer - Alpha Song Moved");
    QVERIFY(QFile::rename(studio.mp3.value(QStringLiteral("alpha")), moved + QStringLiteral(".mp3")));
    QVERIFY(QFile::rename(studio.root + QStringLiteral("/SC001-01 - Test Singer - Alpha Song.cdg"),
                          moved + QStringLiteral(".cdg")));
    QVERIFY(studio.rescan());
    const auto rows = studio.controller->search(QStringLiteral("Alpha Song Moved"), 10);
    QCOMPARE(rows.size(), 1);
    studio.ids.insert(QStringLiteral("alpha"), rows.first().songId);
    studio.mp3.insert(QStringLiteral("alpha"), moved + QStringLiteral(".mp3"));

    // Played once, its remembered Key is shown for it from then on.
    studio.openWindow();
    studio.clickLibrarySong(QStringLiteral("alpha"));
    studio.window->playButton()->click();
    QCOMPARE(studio.player->state(), State::Playing);
    QCOMPARE(studio.player->keySemitones(), 2);
    studio.window->stopButton()->click();
    showHome(*studio.window);
    QCOMPARE(studio.window->keyValueLabel()->text(), QStringLiteral("+2"));
    studio.openWindow();  // a restart
    studio.clickLibrarySong(QStringLiteral("alpha"));
    QCOMPARE(studio.window->keyValueLabel()->text(), QStringLiteral("+2"));
}

void TestSongControls::aPausedPreviewCanCarryOn()
{
    Studio studio;
    QVERIFY(studio.setUp());
    studio.openWindow();
    MainWindow& window = *studio.window;
    QVERIFY(window.previewSong(studio.ids.value(QStringLiteral("alpha"))));
    window.pauseButton()->click();
    QCOMPARE(studio.player->state(), State::Paused);
    QVERIFY(window.playButton()->isEnabled());
    window.playButton()->click();
    QCOMPARE(studio.player->state(), State::Playing);
    QVERIFY(studio.player->song().mp3Path.contains(QStringLiteral("Alpha")));
}

void TestSongControls::aMissingPlaylistSongStillPlaysThroughThePlaylist()
{
    Studio studio;
    QVERIFY(studio.setUp());
    qint64 playlistId = 0;
    QVERIFY(studio.playlists->createPlaylist(QStringLiteral("Party"), &playlistId));
    QVERIFY(studio.addToPlaylist(playlistId, QStringLiteral("delta")));
    QVERIFY(QFile::remove(studio.mp3.value(QStringLiteral("delta"))));
    QVERIFY(studio.rescan());
    studio.openWindow();
    MainWindow& window = *studio.window;
    studio.clickPlaylistRow(0);
    // Its Key can't be set without its files: nothing is saved, and it says why.
    window.keyUpButton()->click();
    QVERIFY(!QFileInfo::exists(studio.settingsPath()));
    QVERIFY(studio.playlist()->messageLabel()->text().contains(QStringLiteral("can't be changed")));
    QVERIFY(window.playButton()->isEnabled());
    window.playButton()->click();
    QCOMPARE(studio.player->state(), State::Empty);
    QCOMPARE(studio.playlist()->messageLabel()->text(), QStringLiteral("This song can't be found right now."));
}

void TestSongControls::restartSongRestartsTheLoadedSong()
{
    // Restart Song (a shortcut) restarts the song loaded, as in v0.1.0, even
    // after Stop or its end; it never starts the selection.
    Studio studio;
    QVERIFY(studio.setUp());
    studio.openWindow();
    MainWindow& window = *studio.window;
    QAction* restart = window.shortcutAction(QStringLiteral("playback.restart"));
    QVERIFY(restart);
    QVERIFY(window.openSong(studio.mp3.value(QStringLiteral("charlie"))));
    studio.clickLibrarySong(QStringLiteral("bravo"));
    restart->trigger();  // Ready
    QCOMPARE(studio.player->state(), State::Playing);
    QVERIFY(studio.player->song().mp3Path.contains(QStringLiteral("Charlie")));
    showHome(window);
    window.stopButton()->click();
    restart->trigger();  // Stopped
    QCOMPARE(studio.player->state(), State::Playing);
    QVERIFY(studio.player->song().mp3Path.contains(QStringLiteral("Charlie")));
    QTRY_COMPARE_WITH_TIMEOUT(studio.player->state(), State::Finished, kShortMs + 4000);
    restart->trigger();  // Finished
    QCOMPARE(studio.player->state(), State::Playing);
    QVERIFY(studio.player->song().mp3Path.contains(QStringLiteral("Charlie")));
    QCOMPARE(window.songText(), songText(QStringLiteral("Charlie Song")));
}

void TestSongControls::anUnsavedPreplayChangeSaysSo()
{
    Studio studio;
    QVERIFY(studio.setUp());
    // A settings file from a newer version is never overwritten.
    QVERIFY(QDir().mkpath(QFileInfo(studio.settingsPath()).absolutePath()));
    const QByteArray newer = R"({"format":"fks-song-settings","version":99,"songs":{}})";
    QVERIFY(testmedia::writeFile(studio.settingsPath(), newer));
    studio.openWindow();
    MainWindow& window = *studio.window;
    studio.clickLibrarySong(QStringLiteral("alpha"));
    window.keyUpButton()->click();
    QVERIFY(studio.library()->messageLabel()->text().contains(QStringLiteral("could not be saved")));
    QCOMPARE(window.keyValueLabel()->text(), QStringLiteral("+1"));  // used for now
    QFile file(studio.settingsPath());
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), newer);
    // And it plays at +1 this time.
    window.playButton()->click();
    QCOMPARE(studio.player->keySemitones(), 1);
}

QTEST_MAIN(TestSongControls)
#include "tst_songcontrols.moc"
