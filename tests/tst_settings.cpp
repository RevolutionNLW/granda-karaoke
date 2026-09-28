#include "AppPreferences.h"
#include "BackgroundWork.h"
#include "BusTestPlayer.h"
#include "LibraryController.h"
#include "LibraryResultsModel.h"
#include "LibraryView.h"
#include "MetadataReviewDialog.h"
#include "MainWindow.h"
#include "PlaylistView.h"
#include "AudioOutputs.h"
#include "SettingsDialog.h"
#include "Shutdown.h"
#include "library/Catalogue.h"
#include "SongSettings.h"
#include "TestMedia.h"
#include "library/UserStateStore.h"
#include "playlist/PlaylistStore.h"
#include "ui/Controls.h"
#include "ui/ElidedLabel.h"
#include "ui/Shortcuts.h"
#include "ui/Theme.h"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QKeySequenceEdit>
#include <QMessageBox>
#include <QPointer>
#include <QScopeGuard>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTreeView>
#include <QTreeWidget>
#include <QtTest>

#include <atomic>
#include <thread>

#include <memory>

namespace {

// The player's pipeline, to check what reaches GStreamer.
class InspectablePlayer : public BusTestPlayer {
public:
    using BusTestPlayer::pipeline;
};

// The same, with the computer's real sound output path.
class RealOutputPlayer : public KaraokePlayer {
public:
    using KaraokePlayer::pipeline;
};

struct Songs {
    QString firstMp3;
    QString secondMp3;
    qint64 firstId = 0;
    qint64 secondId = 0;
};

bool writeSong(const QString& base, int durationMs)
{
    return testmedia::writeMp3(base + QStringLiteral(".mp3"), durationMs)
        && testmedia::writeCdg(base + QStringLiteral(".cdg"),
                               testmedia::markerCdg(durationMs, qMax(50, durationMs / 3)));
}

Songs prepareLibrary(const QString& root, LibraryController& controller)
{
    Songs songs;
    const QString first = root + QStringLiteral("/ST001-01 - Test Singer - First Song");
    const QString second = root + QStringLiteral("/ST001-02 - Test Singer - Second Song");
    songs.firstMp3 = first + QStringLiteral(".mp3");
    songs.secondMp3 = second + QStringLiteral(".mp3");
    if (!QDir().mkpath(root) || !writeSong(first, 3000) || !writeSong(second, 3000))
        qFatal("Could not make synthetic media");
    QSignalSpy finished(&controller, &LibraryController::scanFinished);
    if (!controller.chooseRoot(root) || (finished.count() == 0 && !finished.wait(30000)))
        qFatal("Synthetic library scan did not finish");
    songs.firstId = controller.search(QStringLiteral("First Song"), 5).value(0).songId;
    songs.secondId = controller.search(QStringLiteral("Second Song"), 5).value(0).songId;
    if (songs.firstId == 0 || songs.secondId == 0)
        qFatal("Synthetic songs were not searchable");
    return songs;
}

bool sameFile(const QString& a, const QString& b)
{
    return QFileInfo(a).canonicalFilePath() == QFileInfo(b).canonicalFilePath();
}

// Settings kept in the controller's user-state store, as in the application.
std::unique_ptr<AppPreferences> storedPreferences(LibraryController& controller)
{
    auto prefs = std::make_unique<AppPreferences>(
        [&controller](const QString& key) { return controller.preference(key); },
        [&controller](const QString& key, const QString& value) {
            return controller.setPreference(key, value);
        });
    prefs->setBatchWriter([&controller](const QList<QPair<QString, QString>>& values) {
        return controller.setPreferences(values);
    });
    return prefs;
}

// No two actions ever answer the same keys.
bool shortcutsAreUnique(const MainWindow& window)
{
    QList<QKeySequence> seen;
    for (const shortcuts::Action& action : shortcuts::actions()) {
        const QKeySequence keys = window.shortcutAction(action.id)->shortcut();
        if (keys.isEmpty())
            continue;
        if (seen.contains(keys))
            return false;
        seen.append(keys);
    }
    return true;
}

// Picks an action on the Shortcuts page and presses keys into its box, as a
// user does (press, then release).
void captureShortcut(SettingsDialog* dialog, const QString& id, Qt::Key key)
{
    dialog->showPage(QStringLiteral("Shortcuts"));
    QTreeWidget* list = dialog->shortcutList();
    for (int row = 0; row < list->topLevelItemCount(); ++row) {
        if (list->topLevelItem(row)->data(0, Qt::UserRole + 1).toString() == id)
            list->setCurrentItem(list->topLevelItem(row));
    }
    QKeySequenceEdit* editor = dialog->shortcutEditor();
    editor->setFocus();
    QTest::keyPress(editor, key);
    QTest::keyRelease(editor, key);
    QCoreApplication::processEvents();
}

// Every "already assigned" question open anywhere.
int openQuestions()
{
    int count = 0;
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (widget->objectName() == QLatin1String("shortcutConflictPrompt") && widget->isVisible())
            ++count;
    }
    return count;
}

QPushButton* buttonWithText(QWidget* root, const QString& text)
{
    for (QPushButton* button : root->findChildren<QPushButton*>()) {
        if (button->text() == text)
            return button;
    }
    return nullptr;
}

} // namespace

class TestSettings : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanup();
    void shortcutsSurviveRestartAndCatalogueRebuild();
    void resetAndResetAll();
    void conflictsAreRefusedUnlessMovedDeliberately();
    void escapeAndEnterCannotBeAssigned();
    void typingInTextBoxesNeverTriggersPlainKeyActions();
    void actionsActOnTheSelectedSongsOnly();
    void interfaceScaleAppliesLiveAndPersists();
    void nothingClipsOrOverlapsAtAnyScale();
    void interfaceSizeStaysWithinTheScreen();
    void removeConfirmationIsOptionalButDeleteAlwaysAsks();
    void exitCanAskFirst();
    void lyricsCanStayUpAtTheEndOfASong();
    void newSongsStartAtTheDefaultKeyAndTempo();
    void forgettingKeyAndTempoKeepsACopy();
    void startupChoicesForAutoplayAndPlaylist();
    void unusableStoredShortcutsAreIgnored();
    void onlySongControlsActWhileLyricsAreUp();
    void unsavedChangesDoNotTakeEffect();
    void everySettingSurvivesARestart();
    void volumeAppliesLiveAndOutputWaitsForTheNextSong();
    void settingsPagesAreOrganised();
    void everyVisibleDropDownShowsItsArrow();
    void titleScreenFilesNeverGoInsideTheMusicFolder();
    void folderContainmentComparesWholeFolders();
    void conflictAsksExactlyOnceAndCancelChangesNothing();
    void conflictReassignMovesBothTogether();
    void closingOrEscapingTheQuestionIsCancel();
    void repeatedConflictsNeverStackQuestions();
    void capturingKeysNeverRunsTheirAction();
    void closingSettingsDuringAQuestionIsSafe();
    void openingSettingsDoesNoSlowWork();
    void slowNameCountsNeverFreezeThePages();
    void staleNameCountsAreIgnoredAndCachesRefresh();
    void findingSoundOutputsNeverBlocks();
    void songCountIsRememberedButStaysCurrent();
    void shortcutMovesAreAllOrNothing();
    void reassignOnlyWhatWasAskedAbout();
    void failedNameCountsSaySo();
    void quittingNeverWaitsLongForBackgroundWork();
    void theProgramWaitsForStuckWorkOnlyBriefly();
    void everyWayOutClosesTheSameWay();
    void quittingWhileSettingsWorkIsStillRunning_data();
    void quittingWhileSettingsWorkIsStillRunning();

private:
    QTemporaryDir m_media;
    QString m_songPath;
};

void TestSettings::initTestCase()
{
    theme::apply(*qobject_cast<QApplication*>(QCoreApplication::instance()));
    QString error;
    QVERIFY2(KaraokePlayer::initializeGStreamer(&error), qPrintable(error));
    m_songPath = m_media.filePath(QStringLiteral("Loose Song.mp3"));
    QVERIFY(writeSong(m_media.filePath(QStringLiteral("Loose Song")), 3000));
}

void TestSettings::cleanup()
{
    theme::setScalePercent(100);
    theme::setCompactRows(false);
    theme::setAlternateRows(true);
}

void TestSettings::shortcutsSurviveRestartAndCatalogueRebuild()
{
    QTemporaryDir temporary;
    const QString statePath = temporary.filePath(QStringLiteral("app/user-state.sqlite"));
    const QString cataloguePath = temporary.filePath(QStringLiteral("app/library.sqlite"));
    const QKeySequence custom(Qt::CTRL | Qt::SHIFT | Qt::Key_K);
    {
        LibraryController controller(cataloguePath, {}, {}, nullptr, {}, statePath);
        auto prefs = storedPreferences(controller);
        BusTestPlayer player;
        SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
        MainWindow window(&player, &settings, &controller, nullptr, prefs.get());
        SettingsDialog* dialog = window.openSettings();
        QVERIFY(dialog->assignShortcut(QStringLiteral("key.up"), custom));
        QCOMPARE(window.shortcutAction(QStringLiteral("key.up"))->shortcut(), custom);
        dialog->close();
    }
    // The catalogue is thrown away and rebuilt; the settings live elsewhere.
    QVERIFY(QFile::remove(cataloguePath));
    {
        LibraryController controller(cataloguePath, {}, {}, nullptr, {}, statePath);
        auto prefs = storedPreferences(controller);
        BusTestPlayer player;
        SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
        MainWindow window(&player, &settings, &controller, nullptr, prefs.get());
        QCOMPARE(window.shortcutAction(QStringLiteral("key.up"))->shortcut(), custom);
        QCOMPARE(window.shortcutAction(QStringLiteral("library.focusSearch"))->shortcut(),
                 QKeySequence(Qt::CTRL | Qt::Key_F));
    }
    UserStateStore state(statePath);
    QVERIFY(state.open());
    QCOMPARE(QKeySequence::fromString(state.preference(QStringLiteral("shortcut.key.up")),
                                      QKeySequence::PortableText), custom);
}

void TestSettings::resetAndResetAll()
{
    QTemporaryDir temporary;
    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings);
    SettingsDialog* dialog = window.openSettings();
    dialog->showPage(QStringLiteral("Shortcuts"));
    const QString search = QStringLiteral("library.focusSearch");
    QVERIFY(dialog->assignShortcut(search, QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F)));
    QVERIFY(dialog->assignShortcut(QStringLiteral("key.up"), QKeySequence(Qt::CTRL | Qt::Key_U)));
    QVERIFY(dialog->assignShortcut(QStringLiteral("playlist.moveUp"), {}));

    // Default: select the action and press Default.
    QTreeWidget* list = dialog->shortcutList();
    for (int row = 0; row < list->topLevelItemCount(); ++row) {
        if (list->topLevelItem(row)->data(0, Qt::UserRole + 1).toString() == search)
            list->setCurrentItem(list->topLevelItem(row));
    }
    QTest::mouseClick(buttonWithText(dialog, QStringLiteral("Default")), Qt::LeftButton);
    QCOMPARE(window.shortcutAction(search)->shortcut(), QKeySequence(Qt::CTRL | Qt::Key_F));
    QCOMPARE(window.shortcutAction(QStringLiteral("key.up"))->shortcut(), QKeySequence(Qt::CTRL | Qt::Key_U));

    // Choosing an action's usual keys stores "the default", so a future change
    // of default reaches it; "none" is stored as a deliberate choice.
    QVERIFY(dialog->assignShortcut(QStringLiteral("app.openFile"), {}));
    QCOMPARE(window.preferences()->text(pref::ShortcutPrefix + QStringLiteral("app.openFile")),
             QStringLiteral("none"));
    QVERIFY(window.shortcutAction(QStringLiteral("app.openFile"))->shortcut().isEmpty());
    QVERIFY(dialog->assignShortcut(search, QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F)));
    QVERIFY(dialog->assignShortcut(search, QKeySequence(Qt::CTRL | Qt::Key_F)));
    QVERIFY(window.preferences()->text(pref::ShortcutPrefix + search).isEmpty());
    QTest::mouseClick(buttonWithText(dialog, QStringLiteral("Reset All Shortcuts")), Qt::LeftButton);
    for (const shortcuts::Action& action : shortcuts::actions())
        QCOMPARE(window.shortcutAction(action.id)->shortcut(), action.defaultKeys);
    QVERIFY(shortcutsAreUnique(window));
}

void TestSettings::conflictsAreRefusedUnlessMovedDeliberately()
{
    QTemporaryDir temporary;
    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings);
    SettingsDialog* dialog = window.openSettings();
    const QKeySequence taken(Qt::CTRL | Qt::Key_F);  // Go to Search's default
    int asked = 0;
    bool answer = false;
    QString askedFrom;
    QString askedTo;
    dialog->setReassignConfirmation([&](const QString&, const QString& from, const QString& to) {
        ++asked;
        askedFrom = from;
        askedTo = to;
        return answer;
    });

    QString message;
    QVERIFY(!dialog->assignShortcut(QStringLiteral("key.up"), taken, &message));
    QCOMPARE(asked, 1);
    QCOMPARE(askedFrom, QStringLiteral("Go to Search"));
    QCOMPARE(askedTo, QStringLiteral("Key Up"));
    QVERIFY(message.contains(QStringLiteral("Go to Search")));
    QVERIFY(window.shortcutAction(QStringLiteral("key.up"))->shortcut().isEmpty());
    QCOMPARE(window.shortcutAction(QStringLiteral("library.focusSearch"))->shortcut(), taken);
    QVERIFY(shortcutsAreUnique(window));

    answer = true;
    QVERIFY(dialog->assignShortcut(QStringLiteral("key.up"), taken));
    QCOMPARE(asked, 2);
    QCOMPARE(window.shortcutAction(QStringLiteral("key.up"))->shortcut(), taken);
    QVERIFY(window.shortcutAction(QStringLiteral("library.focusSearch"))->shortcut().isEmpty());
    QVERIFY(shortcutsAreUnique(window));

    // Through the editor on the Shortcuts page the real question is asked
    // (never the direct-call confirmation); declining it changes nothing.
    dialog->showPage(QStringLiteral("Shortcuts"));
    QTreeWidget* list = dialog->shortcutList();
    for (int row = 0; row < list->topLevelItemCount(); ++row) {
        if (list->topLevelItem(row)->data(0, Qt::UserRole + 1).toString() == QLatin1String("key.down"))
            list->setCurrentItem(list->topLevelItem(row));
    }
    dialog->shortcutEditor()->setKeySequence(taken);
    emit dialog->shortcutEditor()->editingFinished();
    QTRY_VERIFY(dialog->shortcutConflictPrompt());
    QCOMPARE(asked, 2);
    QVERIFY(dialog->shortcutConflictPrompt()->text().contains(QStringLiteral("\u201cKey Up\u201d")));
    QVERIFY(dialog->shortcutConflictPrompt()->text().contains(QStringLiteral("\u201cKey Down\u201d")));
    buttonWithText(dialog->shortcutConflictPrompt(), QStringLiteral("Cancel"))->click();
    QTRY_VERIFY(!dialog->shortcutConflictPrompt());
    QVERIFY(dialog->shortcutEditor()->keySequence().isEmpty());
    QVERIFY(window.shortcutAction(QStringLiteral("key.down"))->shortcut().isEmpty());
    QVERIFY(shortcutsAreUnique(window));
}

void TestSettings::escapeAndEnterCannotBeAssigned()
{
    QTemporaryDir temporary;
    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings);
    SettingsDialog* dialog = window.openSettings();
    for (const QKeySequence& keys : {QKeySequence(Qt::Key_Escape), QKeySequence(Qt::Key_Return),
                                     QKeySequence(Qt::Key_Enter),
                                     QKeySequence(Qt::CTRL | Qt::Key_Return),
                                     QKeySequence(Qt::SHIFT | Qt::Key_Escape),
                                     QKeySequence(Qt::Key_Tab), QKeySequence(Qt::Key_Up),
                                     QKeySequence(Qt::Key_Down)}) {
        QString message;
        QVERIFY2(!dialog->assignShortcut(QStringLiteral("playback.stop"), keys, &message),
                 qPrintable(keys.toString()));
        QVERIFY(!message.isEmpty());
        QVERIFY(window.shortcutAction(QStringLiteral("playback.stop"))->shortcut().isEmpty());
    }
    // The built-in rows are shown but cannot be picked for editing.
    QTreeWidget* list = dialog->shortcutList();
    QCOMPARE(list->topLevelItem(0)->text(1).left(6), QStringLiteral("Escape"));
    QCOMPARE(list->topLevelItem(1)->text(1).left(5), QStringLiteral("Enter"));
    QVERIFY(!(list->topLevelItem(0)->flags() & Qt::ItemIsSelectable));
    QVERIFY(!(list->topLevelItem(1)->flags() & Qt::ItemIsSelectable));
    // Modifier arrows are fine.
    QVERIFY(dialog->assignShortcut(QStringLiteral("playback.stop"), QKeySequence(Qt::ALT | Qt::Key_Left)));
}

void TestSettings::typingInTextBoxesNeverTriggersPlainKeyActions()
{
    QTemporaryDir temporary;
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")));
    prepareLibrary(temporary.filePath(QStringLiteral("music")), controller);
    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings, &controller);
    SettingsDialog* dialog = window.openSettings();
    QVERIFY(dialog->assignShortcut(QStringLiteral("key.up"), QKeySequence(Qt::Key_K)));
    QVERIFY(dialog->assignShortcut(QStringLiteral("playback.playPause"), QKeySequence(Qt::Key_Space)));
    dialog->close();
    window.show();
    window.activateWindow();
    if (!QTest::qWaitForWindowActive(&window))
        QSKIP("Keyboard shortcuts need an active window on this platform.");
    QVERIFY(window.openSong(m_songPath));
    QCOMPARE(player.state(), KaraokePlayer::State::Ready);

    QLineEdit* search = window.libraryView()->searchBox();
    search->setFocus();
    QTest::keyClicks(search, QStringLiteral("k k"));
    QCOMPARE(search->text(), QStringLiteral("k k"));
    QCOMPARE(player.keySemitones(), 0);
    QCOMPARE(player.state(), KaraokePlayer::State::Ready);

    // Outside a text box the same keys act.
    window.playlistView()->itemList()->setFocus();
    QTest::keyClick(window.playlistView()->itemList(), Qt::Key_K);
    QCOMPARE(player.keySemitones(), 1);
    QTest::keyClick(window.playlistView()->itemList(), Qt::Key_Space);
    QCOMPARE(player.state(), KaraokePlayer::State::Playing);
    player.stop();
}

void TestSettings::actionsActOnTheSelectedSongsOnly()
{
    QTemporaryDir temporary;
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")));
    const Songs songs = prepareLibrary(temporary.filePath(QStringLiteral("music")), controller);
    PlaylistStore playlists(temporary.filePath(QStringLiteral("app/playlists.sqlite")));
    QVERIFY(playlists.open(nullptr, controller.libraryRoots()));
    qint64 playlistId = 0;
    QVERIFY(playlists.createPlaylist(QStringLiteral("Targets"), &playlistId));
    qint64 firstItem = 0;
    qint64 secondItem = 0;
    QVERIFY(playlists.addItem(playlistId, *controller.songRef(songs.firstId), &firstItem));
    QVERIFY(playlists.addItem(playlistId, *controller.songRef(songs.secondId), &secondItem));
    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings, &controller, &playlists);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    LibraryView* library = window.libraryView();
    PlaylistView* view = window.playlistView();
    int removeAsked = 0;
    view->setRemoveConfirmation([&](QWidget*, const QString&, const QString&) {
        ++removeAsked;
        return true;
    });

    // Nothing chosen: nothing happens.
    library->resultsList()->setCurrentIndex({});
    window.shortcutAction(QStringLiteral("library.sing"))->trigger();
    QCOMPARE(player.state(), KaraokePlayer::State::Empty);
    view->itemList()->setCurrentRow(-1);
    window.shortcutAction(QStringLiteral("playlist.remove"))->trigger();
    QCOMPARE(removeAsked, 0);
    QCOMPARE(playlists.items(playlistId).size(), 2);

    // The chosen library song is the one sung.
    int secondRow = -1;
    for (int row = 0; row < library->songResultCount(); ++row) {
        if (library->resultsList()->model()->index(row, 0)
                .data(LibraryResultsModel::SongIdRole).toLongLong() == songs.secondId)
            secondRow = row;
    }
    QVERIFY(secondRow >= 0);
    library->resultsList()->setCurrentIndex(library->resultsList()->model()->index(secondRow, 0));
    window.shortcutAction(QStringLiteral("library.sing"))->trigger();
    QCOMPARE(player.state(), KaraokePlayer::State::Playing);
    QVERIFY(sameFile(player.song().mp3Path, songs.secondMp3));
    player.stop();

    // The chosen playlist song is the one removed, from the playlist shown.
    view->itemList()->setCurrentRow(1);
    QCOMPARE(view->selectedItemId(), secondItem);
    window.shortcutAction(QStringLiteral("playlist.remove"))->trigger();
    QCOMPARE(removeAsked, 1);
    const auto remaining = playlists.items(playlistId);
    QCOMPARE(remaining.size(), 1);
    QCOMPARE(remaining.first().itemId, firstItem);
}

void TestSettings::interfaceScaleAppliesLiveAndPersists()
{
    QTemporaryDir temporary;
    const QString statePath = temporary.filePath(QStringLiteral("app/user-state.sqlite"));
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")), {}, {},
                                 nullptr, {}, statePath);
    prepareLibrary(temporary.filePath(QStringLiteral("music")), controller);
    auto prefs = storedPreferences(controller);
    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings, &controller, nullptr, prefs.get());
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QTreeView* results = window.libraryView()->resultsList();
    const int rowAt100 = results->sizeHintForRow(0);
    const int fontAt100 = QApplication::font().pixelSize();

    SettingsDialog* dialog = window.openSettings();
    dialog->showPage(QStringLiteral("Appearance"));
    QCOMPARE(dialog->scaleValueLabel()->text(), QStringLiteral("100%"));
    QVERIFY(!dialog->scaleResetButton()->isEnabled());
    QTest::mouseClick(dialog->scaleUpButton(), Qt::LeftButton);
    QTest::mouseClick(dialog->scaleUpButton(), Qt::LeftButton);
    QCOMPARE(theme::scalePercent(), 110);
    QCOMPARE(dialog->scaleValueLabel()->text(), QStringLiteral("110%"));
    QCOMPARE(prefs->number(pref::ScalePercent, 100), 110);
    QVERIFY(QApplication::font().pixelSize() > fontAt100);
    QTRY_VERIFY(results->sizeHintForRow(0) > rowAt100);

    for (int i = 0; i < 20; ++i)
        QTest::mouseClick(dialog->scaleUpButton(), Qt::LeftButton);
    QCOMPARE(theme::scalePercent(), theme::kMaxScalePercent);
    QVERIFY(!dialog->scaleUpButton()->isEnabled());
    QTest::mouseClick(dialog->scaleResetButton(), Qt::LeftButton);
    QCOMPARE(theme::scalePercent(), 100);
    QCOMPARE(QApplication::font().pixelSize(), fontAt100);
    QTRY_COMPARE(results->sizeHintForRow(0), rowAt100);
    QVERIFY(prefs->text(pref::ScalePercent).isEmpty());
    for (int i = 0; i < 20; ++i)
        QTest::mouseClick(dialog->scaleDownButton(), Qt::LeftButton);
    QCOMPARE(theme::scalePercent(), theme::kMinScalePercent);

    // Remembered: a new session reads the chosen size back.
    QTest::mouseClick(dialog->scaleResetButton(), Qt::LeftButton);
    QTest::mouseClick(dialog->scaleUpButton(), Qt::LeftButton);
    auto again = storedPreferences(controller);
    QCOMPARE(again->number(pref::ScalePercent, 100), 105);
}

void TestSettings::interfaceSizeStaysWithinTheScreen()
{
    // Windows display scaling makes the screen smaller in Qt's terms: a
    // 1366x768 laptop at 150% is 911x512. The test screen (800x600) is as
    // small, so a large size from Settings cannot fit it.
    QTemporaryDir temporary;
    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings);
    window.preferences()->setNumber(pref::ScalePercent, 150);
    QCOMPARE(theme::scalePercent(), 150);
    window.showFullScreen();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    const QSize screen = window.screen()->geometry().size();
    if (window.minimumSizeHint().width() <= screen.width()
        && window.minimumSizeHint().height() <= screen.height())
        QSKIP("This test needs a (virtual) screen too small for 150%");

    window.setKeepScaleWithinScreen(true);
    qInfo("Screen %dx%d: 150%% chosen, %d%% used, smallest window %dx%d", screen.width(), screen.height(),
          theme::scalePercent(), window.minimumSizeHint().width(), window.minimumSizeHint().height());
    QVERIFY(theme::scalePercent() < 150);
    QCOMPARE(theme::chosenScalePercent(), 150);
    QCOMPARE(window.preferences()->number(pref::ScalePercent, 100), 150);  // the choice is kept
    QVERIFY(window.minimumSizeHint().width() <= screen.width());
    QVERIFY(window.minimumSizeHint().height() <= screen.height());
    // Settings shows the size in use and says why it is not bigger.
    SettingsDialog* dialog = window.openSettings();
    QCOMPARE(dialog->scaleValueLabel()->text(), QStringLiteral("%1%").arg(theme::scalePercent()));
    QVERIFY(!dialog->scaleUpButton()->isEnabled());
    QVERIFY(!dialog->scaleLimitedLabel()->isHidden());
    // Smaller still works, and is exact.
    dialog->scaleDownButton()->click();
    const int smaller = theme::scalePercent();
    QCOMPARE(window.preferences()->number(pref::ScalePercent, 100), smaller);
    QCOMPARE(theme::chosenScalePercent(), smaller);
    dialog->close();

    // Without the limit (another, bigger screen) the chosen size is used.
    window.preferences()->setNumber(pref::ScalePercent, 150);
    window.setKeepScaleWithinScreen(false);
    QCOMPARE(theme::scalePercent(), 150);
    window.preferences()->reset(pref::ScalePercent);
    QCOMPARE(theme::scalePercent(), 100);
}

void TestSettings::nothingClipsOrOverlapsAtAnyScale()
{
    QTemporaryDir temporary;
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")));
    const Songs songs = prepareLibrary(temporary.filePath(QStringLiteral("music")), controller);
    PlaylistStore playlists(temporary.filePath(QStringLiteral("app/playlists.sqlite")));
    QVERIFY(playlists.open(nullptr, controller.libraryRoots()));
    qint64 playlistId = 0;
    QVERIFY(playlists.createPlaylist(QStringLiteral("Friday Night"), &playlistId));
    QVERIFY(playlists.addItem(playlistId, *controller.songRef(songs.firstId)));
    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings, &controller, &playlists);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    // A widget that shows text must be at least as wide as that text needs
    // (labels that shorten themselves with "..." are exempt).
    const auto checkNothingClipped = [](QWidget* root, const QString& where) {
        for (QWidget* widget : root->findChildren<QWidget*>()) {
            if (!widget->isVisibleTo(root) || qobject_cast<ElidedLabel*>(widget)
                || qobject_cast<ui::IconButton*>(widget) || qobject_cast<ui::ToggleSwitch*>(widget))
                continue;
            const auto* label = qobject_cast<QLabel*>(widget);
            const auto* button = qobject_cast<QAbstractButton*>(widget);
            if ((label && !label->wordWrap() && !label->text().isEmpty()) || (button && !button->text().isEmpty())) {
                const QSize needed = widget->minimumSizeHint().expandedTo(
                    button ? widget->sizeHint() : widget->minimumSizeHint());
                QVERIFY2(widget->width() >= needed.width() && widget->height() >= needed.height(),
                         qPrintable(QStringLiteral("%1: %2 \"%3\" is %4x%5, needs %6x%7")
                                        .arg(where, widget->metaObject()->className(),
                                             label ? label->text() : button->text())
                                        .arg(widget->width()).arg(widget->height())
                                        .arg(needed.width()).arg(needed.height())));
            }
        }
    };
    // Controls side by side in the player bar never overlap.
    const auto checkNoOverlap = [](QWidget* bar) {
        QList<QWidget*> shown;
        for (QWidget* widget : bar->findChildren<QWidget*>(Qt::FindDirectChildrenOnly)) {
            if (widget->isVisibleTo(bar) && widget->width() > 0 && widget->height() > 0)
                shown.append(widget);
        }
        for (int i = 0; i < shown.size(); ++i) {
            for (int j = i + 1; j < shown.size(); ++j)
                QVERIFY2(!shown[i]->geometry().intersects(shown[j]->geometry()),
                         qPrintable(QStringLiteral("%1 overlaps %2").arg(
                             shown[i]->objectName(), shown[j]->objectName())));
        }
    };

    // A 1366 x 768 laptop, less the Windows taskbar.
    const QSize laptop(1366, 728);
    for (const int percent : {80, 100, 110, 125, 150}) {
        window.preferences()->setNumber(pref::ScalePercent, percent);
        QCOMPARE(theme::scalePercent(), percent);
        qInfo("%d%%: smallest main window %dx%d", percent, window.minimumSizeHint().width(),
              window.minimumSizeHint().height());
        QVERIFY2(window.minimumSizeHint().width() <= laptop.width()
                     && window.minimumSizeHint().height() <= laptop.height(),
                 qPrintable(QStringLiteral("%1%: the window cannot fit a 1366x768 laptop").arg(percent)));
        // At its smallest allowed size, and filling the laptop screen.
        for (const QSize size : {window.minimumSizeHint(), laptop}) {
            window.resize(size);
            QTRY_COMPARE(window.size(), size);
            QCoreApplication::processEvents();
            const QString where = QStringLiteral("%1% at %2x%3").arg(percent).arg(window.width()).arg(window.height());
            checkNothingClipped(&window, where);
            if (QTest::currentTestFailed())
                return;
            checkNoOverlap(window.findChild<QWidget*>(QStringLiteral("playerBar")));
            if (QTest::currentTestFailed())
                return;
        }
        SettingsDialog* dialog = window.openSettings();
        for (const QString& page : dialog->pageNames()) {
            dialog->showPage(page);
            QCoreApplication::processEvents();
            checkNothingClipped(dialog, QStringLiteral("Settings %1 at %2%").arg(page).arg(percent));
            if (QTest::currentTestFailed())
                return;
        }
        dialog->close();
        QCoreApplication::processEvents();
    }
    window.preferences()->reset(pref::ScalePercent);
}

void TestSettings::removeConfirmationIsOptionalButDeleteAlwaysAsks()
{
    QTemporaryDir temporary;
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")));
    const Songs songs = prepareLibrary(temporary.filePath(QStringLiteral("music")), controller);
    PlaylistStore playlists(temporary.filePath(QStringLiteral("app/playlists.sqlite")));
    QVERIFY(playlists.open(nullptr, controller.libraryRoots()));
    qint64 playlistId = 0;
    QVERIFY(playlists.createPlaylist(QStringLiteral("Asks"), &playlistId));
    QVERIFY(playlists.addItem(playlistId, *controller.songRef(songs.firstId)));
    QVERIFY(playlists.addItem(playlistId, *controller.songRef(songs.secondId)));
    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings, &controller, &playlists);
    PlaylistView* view = window.playlistView();
    int removeAsked = 0;
    int deleteAsked = 0;
    view->setRemoveConfirmation([&](QWidget*, const QString&, const QString&) {
        ++removeAsked;
        return false;
    });
    view->setDeleteConfirmation([&](QWidget*, const QString&) {
        ++deleteAsked;
        return false;
    });

    view->itemList()->setCurrentRow(0);
    view->removeButton()->click();
    QCOMPARE(removeAsked, 1);
    QCOMPARE(playlists.items(playlistId).size(), 2);

    window.preferences()->setFlag(pref::ConfirmRemoveSong, false);
    view->itemList()->setCurrentRow(0);
    view->removeButton()->click();
    QCOMPARE(removeAsked, 1);
    QCOMPARE(playlists.items(playlistId).size(), 1);

    view->deleteButton()->click();
    QCOMPARE(deleteAsked, 1);
    QVERIFY(playlists.playlist(playlistId));
}

void TestSettings::exitCanAskFirst()
{
    QTemporaryDir temporary;
    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.openSong(m_songPath));
    player.play();
    int asked = 0;
    bool answer = false;
    window.setExitConfirmation([&] {
        ++asked;
        return answer;
    });
    window.shortcutAction(QStringLiteral("app.exit"))->trigger();
    QCOMPARE(asked, 0);  // off by default
    QVERIFY(!window.isVisible());

    window.show();
    player.play();
    window.preferences()->setFlag(pref::ConfirmExit, true);
    window.shortcutAction(QStringLiteral("app.exit"))->trigger();
    QCOMPARE(asked, 1);
    QVERIFY(window.isVisible());
    QCOMPARE(player.state(), KaraokePlayer::State::Playing);
    answer = true;
    window.shortcutAction(QStringLiteral("app.exit"))->trigger();
    QCOMPARE(asked, 2);
    QVERIFY(!window.isVisible());
    QCOMPARE(player.state(), KaraokePlayer::State::Stopped);

    // Settings > General > Quit Application asks the same way.
    window.show();
    answer = false;
    const auto quit = [&window] {
        SettingsDialog* dialog = window.openSettings();
        dialog->showPage(QStringLiteral("General"));
        QPushButton* button = buttonWithText(dialog, QStringLiteral("Quit Application"));
        QVERIFY(button);
        button->click();
        QTRY_VERIFY(!dialog->isVisible());
    };
    quit();
    QTRY_COMPARE(asked, 3);
    QVERIFY(window.isVisible());
    answer = true;
    quit();
    QTRY_COMPARE(asked, 4);
    QTRY_VERIFY(!window.isVisible());
}

void TestSettings::lyricsCanStayUpAtTheEndOfASong()
{
    QTemporaryDir temporary;
    const QString shortSong = temporary.filePath(QStringLiteral("Short"));
    QVERIFY(writeSong(shortSong, 500));
    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings);
    window.setShowErrorDialogs(false);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    window.preferences()->setFlag(pref::ReturnHomeAtEnd, false);
    QVERIFY(window.openSong(shortSong + QStringLiteral(".mp3")));
    window.playButton()->click();
    QVERIFY(window.lyricsVisible());
    QTRY_COMPARE_WITH_TIMEOUT(player.state(), KaraokePlayer::State::Finished, 5000);
    QVERIFY(window.lyricsVisible());
    QTest::keyClick(&window, Qt::Key_Escape);
    QVERIFY(!window.lyricsVisible());

    window.preferences()->reset(pref::ReturnHomeAtEnd);
    window.playButton()->click();
    QVERIFY(window.lyricsVisible());
    QTRY_COMPARE_WITH_TIMEOUT(player.state(), KaraokePlayer::State::Finished, 5000);
    QVERIFY(!window.lyricsVisible());

    // Keeping the screen awake can be turned off.
    window.preferences()->setFlag(pref::KeepDisplayAwake, false);
    window.playButton()->click();
    QCOMPARE(player.state(), KaraokePlayer::State::Playing);
    QVERIFY(!window.displaySleepBlocked());
    window.preferences()->reset(pref::KeepDisplayAwake);
    QVERIFY(window.displaySleepBlocked());
    player.stop();
}

void TestSettings::newSongsStartAtTheDefaultKeyAndTempo()
{
    QTemporaryDir temporary;
    const QString known = temporary.filePath(QStringLiteral("Known"));
    const QString fresh = temporary.filePath(QStringLiteral("Fresh"));
    QVERIFY(writeSong(known, 800));
    QVERIFY(testmedia::writeMp3(fresh + QStringLiteral(".mp3"), 900));
    QVERIFY(testmedia::writeCdg(fresh + QStringLiteral(".cdg"), testmedia::markerCdg(900, 200)));
    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings);
    QVERIFY(window.openSong(known + QStringLiteral(".mp3")));
    window.keyDownButton()->click();  // remembered for this song: -1, 100%
    QCOMPARE(player.keySemitones(), -1);

    window.preferences()->setNumber(pref::DefaultKey, 2);
    window.preferences()->setNumber(pref::DefaultTempo, 106);
    QVERIFY(window.openSong(fresh + QStringLiteral(".mp3")));
    QCOMPARE(player.keySemitones(), 2);
    QCOMPARE(player.tempoPercent(), 106);
    QVERIFY(!settings.hasSettingsFor(songIdentity(player.song())));  // not stored until changed

    QVERIFY(window.openSong(known + QStringLiteral(".mp3")));
    QCOMPARE(player.keySemitones(), -1);
    QCOMPARE(player.tempoPercent(), 100);
}

void TestSettings::forgettingKeyAndTempoKeepsACopy()
{
    QTemporaryDir temporary;
    const QString settingsPath = temporary.filePath(QStringLiteral("settings.json"));
    BusTestPlayer player;
    SongSettingsStore settings(settingsPath);
    MainWindow window(&player, &settings);
    QVERIFY(window.openSong(m_songPath));
    window.keyUpButton()->click();
    const QString identity = songIdentity(player.song());
    QVERIFY(settings.hasSettingsFor(identity));
    QFile original(settingsPath);
    QVERIFY(original.open(QIODevice::ReadOnly));
    const QByteArray before = original.readAll();
    original.close();

    SettingsDialog* dialog = window.openSettings();
    dialog->showPage(QStringLiteral("Advanced"));
    bool answer = false;
    dialog->setForgetConfirmation([&] { return answer; });
    QPushButton* forget = buttonWithText(dialog, QStringLiteral("Forget Every Song's Key and Tempo…"));
    QVERIFY(forget);
    forget->click();
    QVERIFY(settings.hasSettingsFor(identity));

    answer = true;
    forget->click();
    QVERIFY(!settings.hasSettingsFor(identity));
    const QStringList copies = QDir(temporary.path()).entryList(
        {QStringLiteral("settings.json.before-reset-*")}, QDir::Files);
    QCOMPARE(copies.size(), 1);
    QFile copy(temporary.filePath(copies.first()));
    QVERIFY(copy.open(QIODevice::ReadOnly));
    QCOMPARE(copy.readAll(), before);
    // Forgotten on disk too, for the next session.
    SongSettingsStore reread(settingsPath);
    QVERIFY(!reread.hasSettingsFor(identity));
}

void TestSettings::startupChoicesForAutoplayAndPlaylist()
{
    QTemporaryDir temporary;
    PlaylistStore playlists(temporary.filePath(QStringLiteral("playlists.sqlite")));
    QVERIFY(playlists.open());
    qint64 first = 0;
    qint64 second = 0;
    QVERIFY(playlists.createPlaylist(QStringLiteral("First"), &first));
    QVERIFY(playlists.createPlaylist(QStringLiteral("Second"), &second));
    QVERIFY(playlists.setLastPlaylistId(second));
    QVERIFY(playlists.setAutoplay(true));
    AppPreferences prefs;

    applyStartupChoices(prefs, playlists);  // defaults: as they were
    QVERIFY(playlists.autoplay());
    QCOMPARE(playlists.lastPlaylistId(), second);

    prefs.setText(pref::AutoplayAtStartup, QStringLiteral("off"));
    prefs.setText(pref::StartupPlaylist, QString::number(first));
    applyStartupChoices(prefs, playlists);
    QVERIFY(!playlists.autoplay());
    QCOMPARE(playlists.lastPlaylistId(), first);

    prefs.setText(pref::AutoplayAtStartup, QStringLiteral("on"));
    prefs.setText(pref::StartupPlaylist, QStringLiteral("999999"));  // deleted since
    QVERIFY(playlists.setLastPlaylistId(second));
    applyStartupChoices(prefs, playlists);
    QVERIFY(playlists.autoplay());
    QCOMPARE(playlists.lastPlaylistId(), second);
}

void TestSettings::unusableStoredShortcutsAreIgnored()
{
    QTemporaryDir temporary;
    AppPreferences prefs;
    // As if written by hand, damaged, or saved by an older version.
    prefs.setText(pref::ShortcutPrefix + QStringLiteral("playback.stop"), QStringLiteral("Esc"));
    prefs.setText(pref::ShortcutPrefix + QStringLiteral("playback.restart"), QStringLiteral("Return"));
    prefs.setText(pref::ShortcutPrefix + QStringLiteral("key.up"), QStringLiteral("###"));
    prefs.setText(pref::ShortcutPrefix + QStringLiteral("key.down"), QStringLiteral("Ctrl+K, Ctrl+J"));
    prefs.setText(pref::ShortcutPrefix + QStringLiteral("app.openFile"), QStringLiteral("Up"));
    // Two stored choices collide, and one takes a default's keys.
    prefs.setText(pref::ShortcutPrefix + QStringLiteral("tempo.up"), QStringLiteral("Ctrl+T"));
    prefs.setText(pref::ShortcutPrefix + QStringLiteral("tempo.down"), QStringLiteral("Ctrl+T"));
    prefs.setText(pref::ShortcutPrefix + QStringLiteral("key.reset"), QStringLiteral("Ctrl+F"));
    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings, nullptr, nullptr, &prefs);
    const auto keys = [&window](const char* id) {
        return window.shortcutAction(QString::fromLatin1(id))->shortcut();
    };
    QVERIFY(keys("playback.stop").isEmpty());
    QVERIFY(keys("playback.restart").isEmpty());
    QVERIFY(keys("key.up").isEmpty());
    QVERIFY(keys("key.down").isEmpty());
    QCOMPARE(keys("app.openFile"), QKeySequence(Qt::CTRL | Qt::Key_O));  // its default
    QCOMPARE(keys("tempo.down"), QKeySequence(Qt::CTRL | Qt::Key_T));    // listed first
    QVERIFY(keys("tempo.up").isEmpty());
    QCOMPARE(keys("key.reset"), QKeySequence(Qt::CTRL | Qt::Key_F));     // the user's choice
    QVERIFY(keys("library.focusSearch").isEmpty());                      // not shared
    QVERIFY(shortcutsAreUnique(window));
}

void TestSettings::onlySongControlsActWhileLyricsAreUp()
{
    QTemporaryDir temporary;
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")));
    const Songs songs = prepareLibrary(temporary.filePath(QStringLiteral("music")), controller);
    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings, &controller);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QTreeView* results = window.libraryView()->resultsList();
    results->setCurrentIndex(results->model()->index(0, 0));
    window.libraryView()->singButton()->click();
    QCOMPARE(player.state(), KaraokePlayer::State::Playing);
    QVERIFY(window.lyricsVisible());
    const QString singing = player.song().mp3Path;
    // Another song is chosen on the hidden home screen.
    results->setCurrentIndex(results->model()->index(1, 0));
    const int sortBefore = window.libraryView()->sortBox()->currentIndex();
    for (const char* id : {"library.sing", "library.sort.titleDesc", "playlist.next",
                           "library.focusSearch", "app.settings"})
        window.shortcutAction(QString::fromLatin1(id))->trigger();
    QVERIFY(sameFile(player.song().mp3Path, singing));
    QCOMPARE(window.libraryView()->sortBox()->currentIndex(), sortBefore);
    QVERIFY(window.lyricsVisible());
    QVERIFY(!QApplication::activeModalWidget());
    // The song itself can still be controlled.
    window.shortcutAction(QStringLiteral("key.up"))->trigger();
    QCOMPARE(player.keySemitones(), 1);
    window.shortcutAction(QStringLiteral("playback.playPause"))->trigger();
    QCOMPARE(player.state(), KaraokePlayer::State::Paused);
    QVERIFY(window.lyricsVisible());
    player.stop();
    Q_UNUSED(songs)
}

void TestSettings::unsavedChangesDoNotTakeEffect()
{
    bool writable = false;
    QString refusedKey;  // when set, only this key cannot be written
    QHash<QString, QString> disk;
    AppPreferences prefs([&disk](const QString& key) { return disk.value(key); },
                         [&](const QString& key, const QString& value) {
                             if (!writable || key == refusedKey)
                                 return false;
                             disk.insert(key, value);
                             return true;
                         });
    QSignalSpy failed(&prefs, &AppPreferences::saveFailed);
    QVERIFY(!prefs.setNumber(pref::ScalePercent, 125));
    QCOMPARE(failed.count(), 1);
    QCOMPARE(prefs.number(pref::ScalePercent, 100), 100);
    writable = true;
    QVERIFY(prefs.setNumber(pref::ScalePercent, 125));  // the same choice, tried again
    QCOMPARE(prefs.number(pref::ScalePercent, 100), 125);
    QCOMPARE(disk.value(pref::ScalePercent), QStringLiteral("125"));

    // A shortcut move is saved whole or not at all.
    QTemporaryDir temporary;
    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings, nullptr, nullptr, &prefs);
    SettingsDialog* dialog = window.openSettings();
    dialog->setReassignConfirmation([](const QString&, const QString&, const QString&) { return true; });
    for (const bool firstFails : {true, false}) {
        // The first write (freeing the keys) or the second (taking them) fails.
        refusedKey = pref::ShortcutPrefix
            + (firstFails ? QStringLiteral("library.focusSearch") : QStringLiteral("key.up"));
        QVERIFY(!dialog->assignShortcut(QStringLiteral("key.up"), QKeySequence(Qt::CTRL | Qt::Key_F)));
        QCOMPARE(window.shortcutAction(QStringLiteral("library.focusSearch"))->shortcut(),
                 QKeySequence(Qt::CTRL | Qt::Key_F));
        QVERIFY(window.shortcutAction(QStringLiteral("key.up"))->shortcut().isEmpty());
        QVERIFY(disk.value(pref::ShortcutPrefix + QStringLiteral("library.focusSearch")).isEmpty());
    }
}

void TestSettings::everySettingSurvivesARestart()
{
    QTemporaryDir temporary;
    const QString statePath = temporary.filePath(QStringLiteral("app/user-state.sqlite"));
    const QString cataloguePath = temporary.filePath(QStringLiteral("app/library.sqlite"));
    const QList<QPair<QString, QString>> chosen = {
        {pref::ScalePercent, QStringLiteral("120")}, {pref::StartFullscreen, QStringLiteral("0")},
        {pref::ShowSplash, QStringLiteral("0")},
        {pref::ConfirmExit, QStringLiteral("1")}, {pref::CompactRows, QStringLiteral("1")},
        {pref::AlternateRows, QStringLiteral("0")}, {pref::ShowLabelColumn, QStringLiteral("0")},
        {pref::ShowPlaysColumn, QStringLiteral("0")}, {pref::AutoplayAtStartup, QStringLiteral("off")},
        {pref::DefaultKey, QStringLiteral("-2")}, {pref::DefaultTempo, QStringLiteral("96")},
        {pref::ReturnHomeAtEnd, QStringLiteral("0")}, {pref::KeepDisplayAwake, QStringLiteral("0")},
        {pref::StartupPlaylist, QStringLiteral("7")}, {pref::ConfirmRemoveSong, QStringLiteral("0")},
    };
    {
        LibraryController controller(cataloguePath, {}, {}, nullptr, {}, statePath);
        auto prefs = storedPreferences(controller);
        for (const auto& [key, value] : chosen)
            QVERIFY(prefs->setText(key, value));
    }
    QVERIFY(QFile::remove(cataloguePath));  // the catalogue is rebuilt
    LibraryController controller(cataloguePath, {}, {}, nullptr, {}, statePath);
    auto prefs = storedPreferences(controller);
    for (const auto& [key, value] : chosen)
        QCOMPARE(prefs->text(key), value);
    // Restore Defaults puts them back.
    for (const auto& [key, value] : chosen)
        QVERIFY(prefs->reset(key));
    auto reread = storedPreferences(controller);
    for (const auto& [key, value] : chosen)
        QVERIFY(reread->text(key).isEmpty());
}

void TestSettings::volumeAppliesLiveAndOutputWaitsForTheNextSong()
{
    QTemporaryDir temporary;
    const QString second = temporary.filePath(QStringLiteral("Second"));
    QVERIFY(writeSong(second, 800));
    InspectablePlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    AppPreferences prefs;
    prefs.setNumber(pref::Volume, 40);
    MainWindow window(&player, &settings, nullptr, nullptr, &prefs);
    QCOMPARE(player.volumePercent(), 40);
    QVERIFY(window.openSong(m_songPath));
    const auto volume = [&player] {
        gdouble value = -1;
        g_object_get(player.pipeline(), "volume", &value, nullptr);
        return qRound(value * 100);
    };
    QCOMPARE(volume(), 40);
    prefs.setNumber(pref::Volume, 70);  // live, while a song is loaded
    QCOMPARE(volume(), 70);
    prefs.reset(pref::Volume);
    QCOMPARE(volume(), 100);

    // A new output never disturbs the song that is loaded: it starts with the next.
    player.play();
    // (Marked, as a new pipeline may reuse the old one's memory address.)
    g_object_set_data(G_OBJECT(player.pipeline()), "fks-test-marker", &player);
    const auto samePipeline = [&player] {
        return g_object_get_data(G_OBJECT(player.pipeline()), "fks-test-marker") == &player;
    };
    prefs.setText(pref::AudioOutput, QStringLiteral("No Such Speakers"));
    QCOMPARE(player.audioOutput(), QStringLiteral("No Such Speakers"));
    QVERIFY(samePipeline());
    QCOMPARE(player.state(), KaraokePlayer::State::Playing);
    QVERIFY(window.openSong(second + QStringLiteral(".mp3")));
    QVERIFY(!samePipeline());
    QCOMPARE(player.state(), KaraokePlayer::State::Ready);  // missing output: the default is used
    QCOMPARE(volume(), 100);
    // A missing output is looked for again with the next song (it may have
    // been plugged in by then). This needs the real output path, not the
    // test sink: the song is only loaded, never played.
    {
        RealOutputPlayer real;
        MainWindow realWindow(&real, &settings);
        real.setAudioOutput(QStringLiteral("No Such Speakers"));
        if (!realWindow.openSong(m_songPath))
            QSKIP("No sound output on this computer.");
        g_object_set_data(G_OBJECT(real.pipeline()), "fks-test-marker", &real);
        QVERIFY(realWindow.openSong(second + QStringLiteral(".mp3")));
        QVERIFY(g_object_get_data(G_OBJECT(real.pipeline()), "fks-test-marker") != &real);
        // A chosen output that is found is kept from song to song.
        real.setAudioOutput(QString());
        QVERIFY(realWindow.openSong(m_songPath));
        g_object_set_data(G_OBJECT(real.pipeline()), "fks-test-marker", &real);
        QVERIFY(realWindow.openSong(second + QStringLiteral(".mp3")));
        QVERIFY(g_object_get_data(G_OBJECT(real.pipeline()), "fks-test-marker") == &real);
    }
    QVERIFY(!audio::createSink(QStringLiteral("No Such Speakers")));
    for (const audio::Output& output : audio::outputs())
        QVERIFY(!output.id.isEmpty() && !output.label.isEmpty());

    // Not kept for the next start unless asked to.
    prefs.setFlag(pref::RememberAudioOutput, false);
    InspectablePlayer nextPlayer;
    MainWindow nextWindow(&nextPlayer, &settings, nullptr, nullptr, &prefs);
    QVERIFY(nextPlayer.audioOutput().isEmpty());
    QVERIFY(prefs.text(pref::AudioOutput).isEmpty());  // Settings shows what is really used
    prefs.setText(pref::AudioOutput, QStringLiteral("No Such Speakers"));
    prefs.reset(pref::RememberAudioOutput);
    InspectablePlayer thirdPlayer;
    MainWindow thirdWindow(&thirdPlayer, &settings, nullptr, nullptr, &prefs);
    QCOMPARE(thirdPlayer.audioOutput(), QStringLiteral("No Such Speakers"));
}

void TestSettings::settingsPagesAreOrganised()
{
    QTemporaryDir temporary;
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")));
    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings, &controller);
    window.setDataLocations({{QStringLiteral("Song catalogue"), temporary.filePath(QStringLiteral("app/library.sqlite"))},
                             {QStringLiteral("Playlists"), temporary.filePath(QStringLiteral("app/playlists.sqlite"))}});
    SettingsDialog* dialog = window.openSettings();
    QCOMPARE(dialog->pageNames(),
             (QStringList{QStringLiteral("General"), QStringLiteral("Appearance"), QStringLiteral("Playback"),
                          QStringLiteral("Audio"), QStringLiteral("Library"), QStringLiteral("Playlists"),
                          QStringLiteral("Shortcuts"), QStringLiteral("Metadata"), QStringLiteral("Advanced"),
                          QStringLiteral("About")}));

    // Remembering the window's size only matters when not starting full screen.
    const auto checkBox = [dialog](const QString& text) -> QCheckBox* {
        for (QCheckBox* box : dialog->findChildren<QCheckBox*>()) {
            if (box->text() == text)
                return box;
        }
        return nullptr;
    };
    QCheckBox* remember = checkBox(QStringLiteral("Remember the window's size and position"));
    QVERIFY(remember);
    QVERIFY(!remember->isEnabled());
    checkBox(QStringLiteral("Start in full screen"))->click();
    QVERIFY(remember->isEnabled());
    // The splash screen is shown unless turned off.
    QCheckBox* splash = checkBox(QStringLiteral("Show the splash screen"));
    QVERIFY(splash);
    QVERIFY(splash->isChecked());
    QVERIFY(window.preferences()->flag(pref::ShowSplash, true));
    splash->click();
    QVERIFY(!window.preferences()->flag(pref::ShowSplash, true));

    // File locations are on Advanced only; the forget button too.
    const auto pageText = [dialog](const QString& page) {
        dialog->showPage(page);
        QStringList texts;
        for (QLabel* label : dialog->findChildren<QLabel*>()) {
            if (label->isVisibleTo(dialog))
                texts.append(label->text());
        }
        for (QPushButton* button : dialog->findChildren<QPushButton*>()) {
            if (button->isVisibleTo(dialog))
                texts.append(button->text());
        }
        return texts.join(QLatin1Char('\n'));
    };
    const QString library = pageText(QStringLiteral("Library"));
    QVERIFY(!library.contains(QStringLiteral("library.sqlite")));
    QVERIFY(library.contains(QStringLiteral("Rescan")));
    QVERIFY(!pageText(QStringLiteral("Playback")).contains(QStringLiteral("Forget Every")));
    const QString advanced = pageText(QStringLiteral("Advanced"));
    QVERIFY(advanced.contains(temporary.filePath(QStringLiteral("app/playlists.sqlite"))));
    QVERIFY(advanced.contains(QStringLiteral("Forget Every Song's Key and Tempo")));
    QVERIFY(pageText(QStringLiteral("Appearance")).contains(QStringLiteral("Theme")));
}

void TestSettings::everyVisibleDropDownShowsItsArrow()
{
    // The style sheet leaves the arrow to ui::ComboBox, so every drop-down a
    // user can see must be one.
    QTemporaryDir temporary;
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")));
    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings, &controller);
    window.show();
    SettingsDialog* dialog = window.openSettings();
    for (const QString& page : dialog->pageNames())
        dialog->showPage(page);
    QWidget* review = window.openMetadataReview();
    QVERIFY(review);
    int checked = 0;
    for (QWidget* root : {static_cast<QWidget*>(&window), static_cast<QWidget*>(dialog), review}) {
        for (QComboBox* box : root->findChildren<QComboBox*>()) {
            if (box->objectName() == QLatin1String("playlistChooser"))
                continue;  // never shown: the playlist tabs stand for it
            QVERIFY2(qobject_cast<ui::ComboBox*>(box),
                     qPrintable(QStringLiteral("%1 in %2").arg(box->objectName(), root->objectName())));
            ++checked;
        }
    }
    QVERIFY(checked >= 6);
    review->close();
    dialog->close();
}

void TestSettings::titleScreenFilesNeverGoInsideTheMusicFolder()
{
    QTemporaryDir temporary;
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")));
    const QString root = temporary.filePath(QStringLiteral("music"));
    prepareLibrary(root, controller);
    QString summary;
    QString error;
    QVERIFY(!controller.exportTitleScreens(root + QStringLiteral("/screens.json"), &summary, &error));
    QVERIFY(error.contains(QStringLiteral("music folder")));
    QVERIFY(!QFileInfo::exists(root + QStringLiteral("/screens.json")));
    const QString outside = temporary.filePath(QStringLiteral("screens.json"));
    QVERIFY2(controller.exportTitleScreens(outside, &summary, &error), qPrintable(error));
    QVERIFY(QFileInfo::exists(outside));
    QVERIFY(QFile::copy(outside, root + QStringLiteral("/copy.json")));
    QVERIFY(!controller.importTitleScreens(root + QStringLiteral("/copy.json"), &summary, &error));
    QVERIFY2(controller.importTitleScreens(outside, &summary, &error), qPrintable(error));
}

void TestSettings::folderContainmentComparesWholeFolders()
{
    QTemporaryDir temporary;
    const QString music = temporary.filePath(QStringLiteral("music"));
    QVERIFY(QDir().mkpath(music + QStringLiteral("/Disc 1")));
    QVERIFY(QDir().mkpath(temporary.filePath(QStringLiteral("music2"))));
    QVERIFY(Catalogue::pathIsInsideOrEqual(music, music));
    QVERIFY(Catalogue::pathIsInsideOrEqual(music + QStringLiteral("/Disc 1"), music));
    QVERIFY(Catalogue::pathIsInsideOrEqual(music + QStringLiteral("/Disc 1/new.json"), music));
    QVERIFY(Catalogue::pathIsInsideOrEqual(music + QStringLiteral("/not-yet/deeper/new.json"), music));
    QVERIFY(!Catalogue::pathIsInsideOrEqual(temporary.filePath(QStringLiteral("music2/a.json")), music));
    QVERIFY(!Catalogue::pathIsInsideOrEqual(temporary.filePath(QStringLiteral("a.json")), music));
    // Given with the system's own separators (backslashes on Windows).
    QVERIFY(Catalogue::pathIsInsideOrEqual(QDir::toNativeSeparators(music + QStringLiteral("/Disc 1/x.db")),
                                           QDir::toNativeSeparators(music)));
}

void TestSettings::conflictAsksExactlyOnceAndCancelChangesNothing()
{
    QTemporaryDir temporary;
    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings);
    SettingsDialog* dialog = window.openSettings();
    const QString stop = QStringLiteral("playback.stop");
    const QString keyUp = QStringLiteral("key.up");
    QVERIFY(dialog->assignShortcut(stop, QKeySequence(Qt::Key_S)));  // step 1: Stop = S

    captureShortcut(dialog, keyUp, Qt::Key_S);                       // steps 2-3
    QTRY_COMPARE(openQuestions(), 1);
    QMessageBox* prompt = dialog->shortcutConflictPrompt();
    QVERIFY(prompt);
    QVERIFY(prompt->text().contains(QStringLiteral("S is already assigned to “Stop”")));
    QVERIFY(prompt->text().contains(QStringLiteral("Reassign it to “Key Up”?")));
    // The box finishing again (it does when it loses focus) asks nothing more.
    dialog->shortcutEditor()->clearFocus();
    emit dialog->shortcutEditor()->editingFinished();
    QCoreApplication::processEvents();
    QCOMPARE(openQuestions(), 1);
    QCOMPARE(dialog->shortcutConflictPrompt(), prompt);
    // Nothing changes while the question is open.
    QCOMPARE(window.shortcutAction(stop)->shortcut(), QKeySequence(Qt::Key_S));
    QVERIFY(window.shortcutAction(keyUp)->shortcut().isEmpty());

    buttonWithText(prompt, QStringLiteral("Cancel"))->click();
    QTRY_COMPARE(openQuestions(), 0);
    QTRY_VERIFY(!dialog->shortcutConflictPrompt());
    QCOMPARE(window.shortcutAction(stop)->shortcut(), QKeySequence(Qt::Key_S));
    QVERIFY(window.shortcutAction(keyUp)->shortcut().isEmpty());
    QVERIFY(dialog->shortcutEditor()->keySequence().isEmpty());  // back to Key Up's own
    QCOMPARE(window.preferences()->text(pref::ShortcutPrefix + keyUp), QString());
    // Finishing again after Cancel asks nothing (the box shows Key Up's keys).
    emit dialog->shortcutEditor()->editingFinished();
    QCoreApplication::processEvents();
    QCOMPARE(openQuestions(), 0);
}

void TestSettings::conflictReassignMovesBothTogether()
{
    QTemporaryDir temporary;
    const QString statePath = temporary.filePath(QStringLiteral("app/user-state.sqlite"));
    {
        LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")), {}, {},
                                     nullptr, {}, statePath);
        auto prefs = storedPreferences(controller);
        BusTestPlayer player;
        SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
        MainWindow window(&player, &settings, &controller, nullptr, prefs.get());
        SettingsDialog* dialog = window.openSettings();
        QVERIFY(dialog->assignShortcut(QStringLiteral("playback.stop"), QKeySequence(Qt::Key_S)));
        captureShortcut(dialog, QStringLiteral("key.up"), Qt::Key_S);
        QTRY_COMPARE(openQuestions(), 1);
        buttonWithText(dialog->shortcutConflictPrompt(), QStringLiteral("Reassign"))->click();
        QTRY_COMPARE(openQuestions(), 0);
        QVERIFY(window.shortcutAction(QStringLiteral("playback.stop"))->shortcut().isEmpty());
        QCOMPARE(window.shortcutAction(QStringLiteral("key.up"))->shortcut(), QKeySequence(Qt::Key_S));
        QCOMPARE(dialog->shortcutEditor()->keySequence(), QKeySequence(Qt::Key_S));
        QVERIFY(shortcutsAreUnique(window));
        // Both rows show the change at once.
        QStringList rows;
        for (int row = 0; row < dialog->shortcutList()->topLevelItemCount(); ++row) {
            const QTreeWidgetItem* item = dialog->shortcutList()->topLevelItem(row);
            rows.append(item->text(0) + QLatin1Char('=') + item->text(1));
        }
        QVERIFY(rows.contains(QStringLiteral("Playback: Stop=—")));
        QVERIFY(rows.contains(QStringLiteral("Key and Tempo: Key Up=S")));
    }
    // Saved: a new session has the same.
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")), {}, {},
                                 nullptr, {}, statePath);
    auto prefs = storedPreferences(controller);
    QVERIFY(shortcuts::current(*prefs, QStringLiteral("playback.stop")).isEmpty());
    QCOMPARE(shortcuts::current(*prefs, QStringLiteral("key.up")), QKeySequence(Qt::Key_S));
}

void TestSettings::closingOrEscapingTheQuestionIsCancel()
{
    QTemporaryDir temporary;
    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings);
    SettingsDialog* dialog = window.openSettings();
    QVERIFY(dialog->assignShortcut(QStringLiteral("playback.stop"), QKeySequence(Qt::Key_S)));
    for (const bool escape : {false, true}) {
        captureShortcut(dialog, QStringLiteral("key.up"), Qt::Key_S);
        QTRY_COMPARE(openQuestions(), 1);
        QMessageBox* prompt = dialog->shortcutConflictPrompt();
        if (escape)
            QTest::keyClick(prompt, Qt::Key_Escape);
        else
            prompt->close();  // the window's close button
        QTRY_COMPARE(openQuestions(), 0);
        QTRY_VERIFY(!dialog->shortcutConflictPrompt());
        QCOMPARE(window.shortcutAction(QStringLiteral("playback.stop"))->shortcut(),
                 QKeySequence(Qt::Key_S));
        QVERIFY(window.shortcutAction(QStringLiteral("key.up"))->shortcut().isEmpty());
        QVERIFY(dialog->shortcutEditor()->keySequence().isEmpty());
    }
}

void TestSettings::repeatedConflictsNeverStackQuestions()
{
    QTemporaryDir temporary;
    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings);
    SettingsDialog* dialog = window.openSettings();
    QVERIFY(dialog->assignShortcut(QStringLiteral("playback.stop"), QKeySequence(Qt::Key_S)));
    QString holder = QStringLiteral("playback.stop");
    for (const char* next : {"key.up", "key.down", "tempo.up", "tempo.down", "playback.restart"}) {
        const QString id = QString::fromLatin1(next);
        captureShortcut(dialog, id, Qt::Key_S);
        QTRY_COMPARE(openQuestions(), 1);
        // Keys pressed again while the question is open are ignored.
        QTest::keyPress(dialog->shortcutEditor(), Qt::Key_S);
        QTest::keyRelease(dialog->shortcutEditor(), Qt::Key_S);
        emit dialog->shortcutEditor()->editingFinished();
        QCoreApplication::processEvents();
        QCOMPARE(openQuestions(), 1);
        buttonWithText(dialog->shortcutConflictPrompt(), QStringLiteral("Reassign"))->click();
        QTRY_COMPARE(openQuestions(), 0);
        QVERIFY(window.shortcutAction(holder)->shortcut().isEmpty());
        QCOMPARE(window.shortcutAction(id)->shortcut(), QKeySequence(Qt::Key_S));
        QVERIFY(shortcutsAreUnique(window));
        holder = id;
    }
}

void TestSettings::capturingKeysNeverRunsTheirAction()
{
    QTemporaryDir temporary;
    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    SettingsDialog* dialog = window.openSettings();
    QVERIFY(dialog->assignShortcut(QStringLiteral("playback.stop"), QKeySequence(Qt::Key_S)));
    QVERIFY(dialog->assignShortcut(QStringLiteral("key.down"), QKeySequence(Qt::Key_D)));
    QVERIFY(window.openSong(m_songPath));
    window.playButton()->click();
    QCOMPARE(player.state(), KaraokePlayer::State::Playing);
    dialog->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(dialog));
    captureShortcut(dialog, QStringLiteral("key.up"), Qt::Key_S);  // S = Stop elsewhere
    QTRY_COMPARE(openQuestions(), 1);
    QCOMPARE(player.state(), KaraokePlayer::State::Playing);
    buttonWithText(dialog->shortcutConflictPrompt(), QStringLiteral("Cancel"))->click();
    QTRY_COMPARE(openQuestions(), 0);
    captureShortcut(dialog, QStringLiteral("tempo.up"), Qt::Key_D);  // D = Key Down elsewhere
    QTRY_COMPARE(openQuestions(), 1);
    QCOMPARE(player.keySemitones(), 0);
    buttonWithText(dialog->shortcutConflictPrompt(), QStringLiteral("Cancel"))->click();
    QTRY_COMPARE(openQuestions(), 0);
    QCOMPARE(player.state(), KaraokePlayer::State::Playing);
    player.stop();
}

void TestSettings::closingSettingsDuringAQuestionIsSafe()
{
    QTemporaryDir temporary;
    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings);
    for (const bool destroy : {false, true}) {
        QPointer<SettingsDialog> dialog = window.openSettings();
        QVERIFY(dialog->assignShortcut(QStringLiteral("playback.stop"), QKeySequence(Qt::Key_S)));
        captureShortcut(dialog, QStringLiteral("key.up"), Qt::Key_S);
        QTRY_COMPARE(openQuestions(), 1);
        QPointer<QMessageBox> prompt = dialog->shortcutConflictPrompt();
        if (destroy)
            delete dialog;       // e.g. the program closing
        else
            dialog->reject();    // Settings closed
        QTRY_VERIFY(prompt.isNull());
        QTRY_COMPARE(openQuestions(), 0);
        QCOMPARE(window.shortcutAction(QStringLiteral("playback.stop"))->shortcut(),
                 QKeySequence(Qt::Key_S));
        QVERIFY(window.shortcutAction(QStringLiteral("key.up"))->shortcut().isEmpty());
        QTRY_VERIFY(dialog.isNull());  // closed Settings are deleted
    }
}

void TestSettings::openingSettingsDoesNoSlowWork()
{
    QTemporaryDir temporary;
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")));
    prepareLibrary(temporary.filePath(QStringLiteral("music")), controller);
    std::atomic<int> counts{0};
    controller.setReviewSummaryReader([&counts](const QString&) {
        ++counts;
        return std::optional<ReviewSummary>(ReviewSummary{});
    });
    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings, &controller);
    QElapsedTimer timer;
    timer.start();
    SettingsDialog* dialog = window.openSettings();
    QVERIFY2(timer.elapsed() < 500, qPrintable(QString::number(timer.elapsed())));
    // Only the first page is made; nothing slow has started.
    QVERIFY(dialog->isPageBuilt(QStringLiteral("General")));
    for (const char* page : {"Audio", "Library", "Shortcuts", "Metadata", "Advanced"})
        QVERIFY2(!dialog->isPageBuilt(QString::fromLatin1(page)), page);
    QCoreApplication::processEvents();
    QCOMPARE(counts.load(), 0);
    QVERIFY(!controller.isCountingReviewSummary());
}

void TestSettings::slowNameCountsNeverFreezeThePages()
{
    QTemporaryDir temporary;
    auto controller = std::make_unique<LibraryController>(
        temporary.filePath(QStringLiteral("app/library.sqlite")), QString(),
        temporary.filePath(QStringLiteral("app/overrides.sqlite")));
    prepareLibrary(temporary.filePath(QStringLiteral("music")), *controller);
    std::atomic<int> counts{0};
    controller->setReviewSummaryReader([&counts](const QString&) {
        ++counts;
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        ReviewSummary summary;
        summary.high = 7;
        return std::optional<ReviewSummary>(summary);
    });
    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings, controller.get());
    QPointer<SettingsDialog> dialog = window.openSettings();
    QElapsedTimer timer;
    timer.start();
    dialog->showPage(QStringLiteral("Metadata"));
    QVERIFY2(timer.elapsed() < 200, qPrintable(QString::number(timer.elapsed())));  // not 600
    QCOMPARE(dialog->metadataCounts()->text(), QStringLiteral("Counting…"));
    // Moving between pages while it counts stays quick and starts nothing new.
    for (int i = 0; i < 5; ++i) {
        timer.restart();
        dialog->showPage(QStringLiteral("General"));
        dialog->showPage(QStringLiteral("Metadata"));
        QVERIFY2(timer.elapsed() < 200, qPrintable(QString::number(timer.elapsed())));
    }
    QTRY_VERIFY_WITH_TIMEOUT(dialog->metadataCounts()->text().contains(QStringLiteral("Sure: 7")), 5000);
    QCOMPARE(counts.load(), 1);

    // Closing Settings while counting is safe; the result is kept for later.
    QVERIFY(controller->setManualOverride(controller->search(QStringLiteral("First Song"), 1).value(0).songId,
                                          QStringLiteral("A"), QStringLiteral("B")));  // counts now stale
    dialog->showPage(QStringLiteral("Metadata"));
    QTRY_COMPARE(counts.load(), 2);
    dialog->close();
    QTRY_VERIFY(dialog.isNull());
    QTRY_VERIFY_WITH_TIMEOUT(controller->cachedReviewSummary().has_value(), 5000);
    dialog = window.openSettings();
    dialog->showPage(QStringLiteral("Metadata"));
    QVERIFY(dialog->metadataCounts()->text().contains(QStringLiteral("Sure: 7")));  // at once
    QCOMPARE(counts.load(), 2);
    dialog->close();
    // Destroying the library while a count runs waits for it safely.
    QVERIFY(controller->setManualOverride(controller->search(QStringLiteral("Second Song"), 1).value(0).songId,
                                          QStringLiteral("C"), QStringLiteral("D")));
    controller->requestReviewSummary();
    QVERIFY(controller->isCountingReviewSummary());
    // (the window refers to the library, so it goes first)
}

void TestSettings::staleNameCountsAreIgnoredAndCachesRefresh()
{
    QTemporaryDir temporary;
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")), {},
                                 temporary.filePath(QStringLiteral("app/overrides.sqlite")));
    const Songs songs = prepareLibrary(temporary.filePath(QStringLiteral("music")), controller);
    std::atomic<int> counts{0};
    controller.setReviewSummaryReader([&counts](const QString&) {
        const int call = ++counts;
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        ReviewSummary summary;
        summary.high = call * 100;  // tells the counts apart
        return std::optional<ReviewSummary>(summary);
    });
    QList<qint64> delivered;
    connect(&controller, &LibraryController::reviewSummaryReady, this,
            [&delivered](const ReviewSummary& summary) { delivered.append(summary.high); });
    controller.requestReviewSummary();
    controller.requestReviewSummary();  // while counting: nothing new starts
    QTRY_COMPARE(counts.load(), 1);
    // The names change before the first count finishes: it is never shown.
    QVERIFY(controller.setManualOverride(songs.firstId, QStringLiteral("New"), QStringLiteral("Name")));
    QTRY_VERIFY_WITH_TIMEOUT(!delivered.isEmpty(), 5000);
    QCOMPARE(delivered, QList<qint64>{200});
    QCOMPARE(counts.load(), 2);
    // Remembered until the library changes again.
    controller.requestReviewSummary();
    QCOMPARE(delivered, (QList<qint64>{200, 200}));
    QCOMPARE(counts.load(), 2);
    QVERIFY(controller.clearManualOverride(songs.firstId));
    QVERIFY(!controller.cachedReviewSummary());
    controller.requestReviewSummary();
    QTRY_COMPARE_WITH_TIMEOUT(delivered.size(), 3, 5000);
    QCOMPARE(delivered.last(), 300);

    // The real count agrees with the per-filter counts.
    const auto real = Catalogue::readReviewSummary(controller.databasePath());
    QVERIFY(real);
    QCOMPARE(real->all, controller.reviewCount(ReviewFilter::All));
    QCOMPARE(real->unresolved, controller.reviewCount(ReviewFilter::Unresolved));
    QCOMPARE(real->medium, controller.reviewCount(ReviewFilter::Medium));
    QCOMPARE(real->manual, controller.reviewCount(ReviewFilter::Manual));
    QCOMPARE(real->high + real->medium + real->low + real->unresolved, real->all);
}

void TestSettings::findingSoundOutputsNeverBlocks()
{
    audio::OutputFinder* finder = audio::OutputFinder::instance();
    std::atomic<int> searches{0};
    std::atomic<int> running{0};
    std::atomic<int> mostAtOnce{0};
    finder->setLister([&] {
        ++searches;
        const int now = ++running;
        mostAtOnce = std::max(mostAtOnce.load(), now);
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        --running;
        return QList<audio::Output>{{QStringLiteral("id:1"), QStringLiteral("Test Speakers")}};
    });
    finder->forget();
    QTemporaryDir temporary;
    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings);
    QPointer<SettingsDialog> dialog = window.openSettings();
    QElapsedTimer timer;
    timer.start();
    dialog->showPage(QStringLiteral("Audio"));
    QVERIFY2(timer.elapsed() < 200, qPrintable(QString::number(timer.elapsed())));
    auto* output = dialog->findChild<QComboBox*>(QStringLiteral("audioOutput"));
    QVERIFY(output);
    QCOMPARE(output->count(), 1);  // just the usual output while looking
    finder->refresh();             // e.g. Refresh pressed during the search
    QTRY_VERIFY_WITH_TIMEOUT(output->findText(QStringLiteral("Test Speakers")) >= 0, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(searches.load(), 2, 5000);  // one more, never two at once
    QTRY_VERIFY(!finder->isSearching());
    QCOMPARE(mostAtOnce.load(), 1);
    // Kept for the session: reopening Settings does not search again.
    dialog->close();
    QTRY_VERIFY(dialog.isNull());
    dialog = window.openSettings();
    dialog->showPage(QStringLiteral("Audio"));
    QCOMPARE(searches.load(), 2);
    QVERIFY(dialog->findChild<QComboBox*>(QStringLiteral("audioOutput"))->findText(QStringLiteral("Test Speakers")) >= 0);
    // Closing Settings during a search is safe.
    finder->refresh();
    dialog->close();
    QTRY_VERIFY_WITH_TIMEOUT(!finder->isSearching(), 5000);
    finder->setLister({});
    finder->forget();
}

void TestSettings::songCountIsRememberedButStaysCurrent()
{
    QTemporaryDir temporary;
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")));
    const QString root = temporary.filePath(QStringLiteral("music"));
    prepareLibrary(root, controller);
    QCOMPARE(controller.statusText(), QStringLiteral("2 songs"));
    // Asked on every scan progress update: it must be cheap.
    QElapsedTimer timer;
    timer.start();
    for (int i = 0; i < 2000; ++i)
        QVERIFY(!controller.statusText().isEmpty());
    QVERIFY2(timer.elapsed() < 200, qPrintable(QString::number(timer.elapsed())));
    // Remembered: a change made behind the library's back is not re-counted
    // on every question...
    {
        QSqlDatabase side = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("side"));
        side.setDatabaseName(controller.databasePath());
        QVERIFY(side.open());
        QSqlQuery query(side);
        QVERIFY(query.exec(QStringLiteral(
            "UPDATE sources SET playable=0, unplayable_reason='missing' WHERE id="
            "(SELECT min(id) FROM sources WHERE kind='loose_cdg')")));
        side.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("side"));
    QCOMPARE(controller.statusText(), QStringLiteral("2 songs"));
    // ...but every real change of the library is: a new song found by a scan.
    QVERIFY(writeSong(root + QStringLiteral("/ST001-03 - Test Singer - Third Song"), 800));
    QSignalSpy finished(&controller, &LibraryController::scanFinished);
    controller.requestRefreshScan();
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 10000);
    QCOMPARE(controller.statusText(), QStringLiteral("3 songs"));
}

void TestSettings::shortcutMovesAreAllOrNothing()
{
    // With a store that saves several values together.
    QHash<QString, QString> disk;
    bool batchFails = true;
    AppPreferences prefs([&disk](const QString& key) { return disk.value(key); },
                         [&disk](const QString& key, const QString& value) {
                             disk.insert(key, value);
                             return true;
                         });
    prefs.setBatchWriter([&](const QList<QPair<QString, QString>>& values) {
        if (batchFails)
            return false;
        for (const auto& [key, value] : values)
            disk.insert(key, value);
        return true;
    });
    QVERIFY(shortcuts::assign(prefs, QStringLiteral("playback.stop"), QKeySequence(Qt::Key_S)));
    const QHash<QString, QString> before = disk;
    QVERIFY(!shortcuts::move(prefs, QStringLiteral("playback.stop"), QStringLiteral("key.up"), QKeySequence(Qt::Key_S)));
    QCOMPARE(disk, before);
    QCOMPARE(shortcuts::current(prefs, QStringLiteral("playback.stop")), QKeySequence(Qt::Key_S));
    QVERIFY(shortcuts::current(prefs, QStringLiteral("key.up")).isEmpty());
    batchFails = false;
    QVERIFY(shortcuts::move(prefs, QStringLiteral("playback.stop"), QStringLiteral("key.up"), QKeySequence(Qt::Key_S)));
    QVERIFY(shortcuts::current(prefs, QStringLiteral("playback.stop")).isEmpty());
    QCOMPARE(shortcuts::current(prefs, QStringLiteral("key.up")), QKeySequence(Qt::Key_S));

    // Without one, a failure part-way puts back what was already saved.
    QHash<QString, QString> single;
    QString refused;
    AppPreferences oneByOne([&single](const QString& key) { return single.value(key); },
                            [&](const QString& key, const QString& value) {
                                if (key == refused)
                                    return false;
                                single.insert(key, value);
                                return true;
                            });
    QVERIFY(shortcuts::assign(oneByOne, QStringLiteral("playback.stop"), QKeySequence(Qt::Key_S)));
    const QHash<QString, QString> saved = single;
    refused = pref::ShortcutPrefix + QStringLiteral("key.up");
    QVERIFY(!shortcuts::move(oneByOne, QStringLiteral("playback.stop"), QStringLiteral("key.up"), QKeySequence(Qt::Key_S)));
    QCOMPARE(single, saved);
    QCOMPARE(shortcuts::current(oneByOne, QStringLiteral("playback.stop")), QKeySequence(Qt::Key_S));
    // If even putting it back fails, what is shown is what is really saved.
    int writesLeft = 0;  // the storage fails after the first write
    AppPreferences failing([&single](const QString& key) { return single.value(key); },
                           [&](const QString& key, const QString& value) {
                               if (writesLeft-- <= 0)
                                   return false;
                               single.insert(key, value);
                               return true;
                           });
    const QString stopKey = pref::ShortcutPrefix + QStringLiteral("playback.stop");
    QCOMPARE(single.value(stopKey), QStringLiteral("S"));
    writesLeft = 1;
    QVERIFY(!shortcuts::move(failing, QStringLiteral("playback.stop"), QStringLiteral("key.up"), QKeySequence(Qt::Key_S)));
    QVERIFY(single.value(stopKey).isEmpty());  // cleared, and could not be put back
    QVERIFY(shortcuts::current(failing, QStringLiteral("playback.stop")).isEmpty());
    QVERIFY(shortcuts::current(failing, QStringLiteral("key.up")).isEmpty());

    // And in the real store: one transaction.
    QTemporaryDir temporary;
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")), {}, {},
                                 nullptr, {}, temporary.filePath(QStringLiteral("app/user-state.sqlite")));
    QVERIFY(controller.setPreferences({{QStringLiteral("a"), QStringLiteral("1")},
                                       {QStringLiteral("b"), QStringLiteral("2")}}));
    QCOMPARE(controller.preference(QStringLiteral("a")), QStringLiteral("1"));
    QCOMPARE(controller.preference(QStringLiteral("b")), QStringLiteral("2"));
}

void TestSettings::reassignOnlyWhatWasAskedAbout()
{
    QTemporaryDir temporary;
    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings);
    SettingsDialog* dialog = window.openSettings();
    QVERIFY(dialog->assignShortcut(QStringLiteral("playback.stop"), QKeySequence(Qt::Key_S)));
    captureShortcut(dialog, QStringLiteral("key.up"), Qt::Key_S);
    QTRY_COMPARE(openQuestions(), 1);
    // Direct changes wait for the answer.
    QVERIFY(!dialog->assignShortcut(QStringLiteral("tempo.up"), QKeySequence(Qt::Key_T)));
    // The shortcuts change some other way while the question is open.
    QVERIFY(shortcuts::assign(*window.preferences(), QStringLiteral("playback.stop"), QKeySequence(Qt::Key_T)));
    buttonWithText(dialog->shortcutConflictPrompt(), QStringLiteral("Reassign"))->click();
    QTRY_COMPARE(openQuestions(), 0);
    QCOMPARE(window.shortcutAction(QStringLiteral("playback.stop"))->shortcut(), QKeySequence(Qt::Key_T));
    QVERIFY(window.shortcutAction(QStringLiteral("key.up"))->shortcut().isEmpty());
    QVERIFY(dialog->shortcutMessage()->text().contains(QStringLiteral("nothing was reassigned")));

    // Also when the change is hidden behind an earlier action: Restart is
    // given S by hand (a damaged store), while Stop still shows it.
    captureShortcut(dialog, QStringLiteral("key.up"), Qt::Key_T);
    QTRY_COMPARE(openQuestions(), 1);
    QVERIFY(window.preferences()->setText(pref::ShortcutPrefix + QStringLiteral("playback.restart"),
                                          QStringLiteral("T")));
    QCOMPARE(shortcuts::current(*window.preferences(), QStringLiteral("playback.stop")), QKeySequence(Qt::Key_T));
    buttonWithText(dialog->shortcutConflictPrompt(), QStringLiteral("Reassign"))->click();
    QTRY_COMPARE(openQuestions(), 0);
    QCOMPARE(window.shortcutAction(QStringLiteral("playback.stop"))->shortcut(), QKeySequence(Qt::Key_T));
    QVERIFY(window.shortcutAction(QStringLiteral("key.up"))->shortcut().isEmpty());
    QVERIFY(dialog->shortcutMessage()->text().contains(QStringLiteral("nothing was reassigned")));
}

void TestSettings::failedNameCountsSaySo()
{
    QTemporaryDir temporary;
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")));
    prepareLibrary(temporary.filePath(QStringLiteral("music")), controller);
    std::atomic<bool> fail{true};
    controller.setReviewSummaryReader([&fail](const QString&) {
        return fail ? std::optional<ReviewSummary>() : std::optional<ReviewSummary>(ReviewSummary{9, 9});
    });
    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings, &controller);
    SettingsDialog* dialog = window.openSettings();
    dialog->showPage(QStringLiteral("Metadata"));
    QTRY_VERIFY(dialog->metadataCounts()->text().contains(QStringLiteral("could not be worked out")));
    QVERIFY(!controller.isCountingReviewSummary());
    fail = false;  // opening the page again tries again
    dialog->showPage(QStringLiteral("General"));
    dialog->showPage(QStringLiteral("Metadata"));
    QTRY_VERIFY(dialog->metadataCounts()->text().contains(QStringLiteral("Sure: 9")));
}

void TestSettings::quittingNeverWaitsLongForBackgroundWork()
{
    QTemporaryDir temporary;
    auto controller = std::make_unique<LibraryController>(temporary.filePath(QStringLiteral("app/library.sqlite")));
    prepareLibrary(temporary.filePath(QStringLiteral("music")), *controller);
    std::atomic<bool> finished{false};
    controller->setReviewSummaryReader([&finished](const QString&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(4500));  // stuck, for a while
        finished = true;
        return std::optional<ReviewSummary>(ReviewSummary{});
    });
    controller->requestReviewSummary();
    QTRY_VERIFY(controller->isCountingReviewSummary());
    QElapsedTimer timer;
    timer.start();
    controller.reset();  // e.g. the program closing
    QVERIFY2(timer.elapsed() < 4000, qPrintable(QString::number(timer.elapsed())));
    // Its late result arrives after the library has gone, and is dropped.
    QTRY_VERIFY_WITH_TIMEOUT(finished.load(), 5000);
    QTest::qWait(200);
    QVERIFY(background::waitForAll(2000));
    QCOMPARE(background::running(), 0);
}

void TestSettings::theProgramWaitsForStuckWorkOnlyBriefly()
{
    std::atomic<bool> release{false};
    background::run(QStringLiteral("Stuck"), [&release] {
        while (!release)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    });
    QCOMPARE(background::running(), 1);
    QElapsedTimer timer;
    timer.start();
    QVERIFY(!background::waitForAll(200));  // still stuck: the program would end at once
    QVERIFY(timer.elapsed() < 1500);
    release = true;
    QVERIFY(background::waitForAll(2000));  // and tidied away
    QCOMPARE(background::running(), 0);
}

void TestSettings::everyWayOutClosesTheSameWay()
{
    // The title bar's close button, Settings > General > Quit Application and
    // the Exit shortcut all end in the same place: playback stopped, the
    // window's place remembered, the window closed.
    QTemporaryDir temporary;
    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings);
    window.shortcutAction(QStringLiteral("app.exit"))->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Q));
    const QList<std::pair<QString, std::function<void()>>> routes = {
        {QStringLiteral("close button"), [&window] {
             // What the window system sends when the close button is clicked.
             QCloseEvent close;
             QCoreApplication::sendEvent(window.windowHandle(), &close);
         }},
        {QStringLiteral("Settings > Quit Application"), [&window] {
             SettingsDialog* dialog = window.openSettings();
             dialog->showPage(QStringLiteral("General"));
             QPushButton* button = buttonWithText(dialog, QStringLiteral("Quit Application"));
             QVERIFY(button);
             QTest::mouseClick(button, Qt::LeftButton);
         }},
        {QStringLiteral("Exit shortcut"), [&window] {
             window.shortcutAction(QStringLiteral("app.exit"))->trigger();
         }},
    };
    for (const auto& [name, route] : routes) {
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QVERIFY(window.openSong(m_songPath));
        player.play();
        QCOMPARE(player.state(), KaraokePlayer::State::Playing);
        window.preferences()->setText(pref::WindowGeometry, QString());
        route();
        QTRY_VERIFY2(!window.isVisible(), qPrintable(name));
        QVERIFY2(player.state() == KaraokePlayer::State::Stopped, qPrintable(name));
        QVERIFY2(!window.preferences()->text(pref::WindowGeometry).isEmpty(), qPrintable(name));
    }
}

void TestSettings::quittingWhileSettingsWorkIsStillRunning_data()
{
    QTest::addColumn<bool>("findingOutputs");
    QTest::addColumn<bool>("countingNames");
    QTest::newRow("sound outputs") << true << false;
    QTest::newRow("song-name counts") << false << true;
    QTest::newRow("both") << true << true;
}

void TestSettings::quittingWhileSettingsWorkIsStillRunning()
{
    // Settings opened on Audio and Metadata, then the program closes while
    // both background jobs are still working. Settings, the window and the
    // library all go, in the program's order, before the results arrive; the
    // late results are dropped without touching anything that has gone.
    QFETCH(bool, findingOutputs);
    QFETCH(bool, countingNames);
    std::atomic<bool> release{false};
    std::atomic<int> started{0};
    auto hold = [&] {
        ++started;
        while (!release)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    };
    audio::OutputFinder* finder = audio::OutputFinder::instance();
    finder->setLister([&] {
        if (findingOutputs)
            hold();
        return QList<audio::Output>{{QStringLiteral("id:9"), QStringLiteral("Late Speakers")}};
    });
    finder->forget();
    // However the test ends, the workers are let go and waited for before
    // what they use goes, and the shared finder is left as it was found.
    const auto cleanUp = qScopeGuard([&] {
        release = true;
        background::waitForAll(5000);
        finder->setLister({});
        finder->forget();
    });
    QTemporaryDir temporary;
    auto controller = std::make_unique<LibraryController>(temporary.filePath(QStringLiteral("app/library.sqlite")));
    prepareLibrary(temporary.filePath(QStringLiteral("music")), *controller);
    controller->setReviewSummaryReader([&](const QString&) {
        if (countingNames)
            hold();
        return std::optional<ReviewSummary>(ReviewSummary{});
    });
    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    auto window = std::make_unique<MainWindow>(&player, &settings, controller.get());
    QPointer<SettingsDialog> dialog = window->openSettings();
    if (findingOutputs)
        dialog->showPage(QStringLiteral("Audio"));
    if (countingNames)
        dialog->showPage(QStringLiteral("Metadata"));
    QTRY_COMPARE(started.load(), int(findingOutputs) + int(countingNames));

    QElapsedTimer timer;
    timer.start();
    window->close();
    shutdown::stopLibraryOrExit(*controller, 99);
    window.reset();
    QVERIFY(dialog.isNull());
    controller.reset();
    // Promptly, not after the held work: well under the 3 s the program
    // waits for background work at exit (slow machines take about 1 s).
    QVERIFY2(timer.elapsed() < 2500, qPrintable(QString::number(timer.elapsed())));
    QCOMPARE(background::running(), int(findingOutputs) + int(countingNames));

    release = true;
    QVERIFY(background::waitForAll(3000));
    QCoreApplication::processEvents();  // the late results are delivered now, and dropped
    QTest::qWait(50);
    QCOMPARE(background::running(), 0);
    finder->setLister({});
    finder->forget();
}

QTEST_MAIN(TestSettings)
#include "tst_settings.moc"
