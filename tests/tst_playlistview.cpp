#include "BusTestPlayer.h"
#include "LibraryController.h"
#include "LibraryResultsModel.h"
#include "LibraryView.h"
#include "MainWindow.h"
#include "PlaylistView.h"
#include "SongPair.h"
#include "SongSettings.h"
#include "TestMedia.h"
#include "playlist/PlaylistPlayback.h"
#include "playlist/PlaylistStore.h"
#include "ui/Theme.h"

#include <QComboBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QFrame>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QHeaderView>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTreeView>
#include <QtTest>

#include <utility>
#include <memory>

class PlaylistStoreTestAccess {
public:
    static bool setSongId(PlaylistStore& store, qint64 itemId, qint64 songId)
    {
        QSqlQuery query(store.m_database);
        query.prepare(QStringLiteral("UPDATE playlist_items SET song_id=? WHERE id=?"));
        query.addBindValue(songId);
        query.addBindValue(itemId);
        return query.exec();
    }

    static bool setSnapshotMetadata(PlaylistStore& store, qint64 itemId,
                                    const QString& title, const QString& discId,
                                    int track)
    {
        QSqlQuery query(store.m_database);
        query.prepare(QStringLiteral(
            "UPDATE playlist_items SET title=?,disc_id=?,track=? WHERE id=?"));
        query.addBindValue(title);
        query.addBindValue(discId);
        query.addBindValue(track);
        query.addBindValue(itemId);
        return query.exec();
    }

    static bool makeReadOnly(PlaylistStore& store)
    {
        QSqlQuery query(store.m_database);
        return query.exec(QStringLiteral("PRAGMA query_only=ON"));
    }
};

namespace {

struct LibrarySongs {
    QString firstMp3;
    QString secondMp3;
    QString thirdMp3;
    QString fourthMp3;
    qint64 firstId = 0;
    qint64 secondId = 0;
    qint64 thirdId = 0;
    qint64 fourthId = 0;
};

bool writeSong(const QString& base, int durationMs)
{
    return testmedia::writeMp3(base + QStringLiteral(".mp3"), durationMs)
        && testmedia::writeCdg(base + QStringLiteral(".cdg"),
                               testmedia::markerCdg(durationMs, qMax(50, durationMs / 3)));
}

LibrarySongs prepareLibraryAt(const QString& root, LibraryController& controller)
{
    LibrarySongs songs;
    const QString first = root + QStringLiteral("/PV001-01 - Test Singer - First Song");
    const QString second = root + QStringLiteral("/PV001-02 - Test Singer - Second Song");
    const QString third = root + QStringLiteral("/PV001-03 - Test Singer - Third Song");
    const QString fourth = root + QStringLiteral("/PV001-04 - Test Singer - Fourth Song");
    songs.firstMp3 = first + QStringLiteral(".mp3");
    songs.secondMp3 = second + QStringLiteral(".mp3");
    songs.thirdMp3 = third + QStringLiteral(".mp3");
    songs.fourthMp3 = fourth + QStringLiteral(".mp3");
    if (!QDir().mkpath(root)
        || !writeSong(first, 450)
        || !writeSong(second, 1200)
        || !writeSong(third, 450)
        || !writeSong(fourth, 450))
        qFatal("Could not make synthetic playlist-view media");
    QSignalSpy ready(&controller, &LibraryController::libraryReady);
    QSignalSpy finished(&controller, &LibraryController::scanFinished);
    if (!controller.chooseRoot(root))
        qFatal("Could not choose synthetic library root");
    if (ready.count() == 0 && !ready.wait(5000))
        qFatal("Synthetic library scan did not become ready");
    if (finished.count() == 0 && !finished.wait(5000))
        qFatal("Synthetic library scan did not finish");
    const auto firstRows = controller.search(QStringLiteral("First Song"), 10);
    const auto secondRows = controller.search(QStringLiteral("Second Song"), 10);
    const auto thirdRows = controller.search(QStringLiteral("Third Song"), 10);
    const auto fourthRows = controller.search(QStringLiteral("Fourth Song"), 10);
    if (firstRows.size() != 1 || secondRows.size() != 1
        || thirdRows.size() != 1 || fourthRows.size() != 1)
        qFatal("Synthetic playlist-view songs were not searchable");
    songs.firstId = firstRows.first().songId;
    songs.secondId = secondRows.first().songId;
    songs.thirdId = thirdRows.first().songId;
    songs.fourthId = fourthRows.first().songId;
    return songs;
}

LibrarySongs prepareLibrary(QTemporaryDir& temporary, LibraryController& controller)
{
    return prepareLibraryAt(temporary.filePath(QStringLiteral("music")), controller);
}

qint64 addSong(PlaylistStore& store, LibraryController& controller,
               qint64 playlistId, qint64 songId)
{
    const auto ref = controller.songRef(songId);
    if (!ref)
        qFatal("Could not build playlist song snapshot");
    qint64 itemId = 0;
    if (!store.addItem(playlistId, *ref, &itemId))
        qFatal("Could not add synthetic playlist item");
    return itemId;
}

bool sameFile(const QString& first, const QString& second)
{
    return QFileInfo(first).canonicalFilePath() == QFileInfo(second).canonicalFilePath();
}

void setLibraryInUse(LibraryView* library)
{
    QTest::mouseClick(library->searchBox(), Qt::LeftButton);
    QVERIFY(library->isActive());
}

int libraryRowForSongId(const LibraryView* view, qint64 songId)
{
    const QAbstractItemModel* model = view->resultsList()->model();
    for (int row = 0; row < model->rowCount(); ++row) {
        if (model->index(row, 0).data(LibraryResultsModel::SongIdRole).toLongLong()
            == songId)
            return row;
    }
    return -1;
}

QList<qint64> itemIds(const PlaylistStore& store, qint64 playlistId)
{
    QList<qint64> result;
    for (const PlaylistEntry& entry : store.items(playlistId))
        result.append(entry.itemId);
    return result;
}

QList<qint64> songIds(const PlaylistStore& store, qint64 playlistId)
{
    QList<qint64> result;
    for (const PlaylistEntry& entry : store.items(playlistId))
        result.append(entry.songId);
    return result;
}

bool sameEntries(const QList<PlaylistEntry>& first,
                 const QList<PlaylistEntry>& second)
{
    if (first.size() != second.size())
        return false;
    for (int i = 0; i < first.size(); ++i) {
        const PlaylistEntry& a = first.at(i);
        const PlaylistEntry& b = second.at(i);
        if (a.itemId != b.itemId || a.playlistId != b.playlistId
            || a.position != b.position || a.songId != b.songId
            || a.title != b.title || a.artist != b.artist
            || a.discId != b.discId || a.track != b.track
            || a.rootPath != b.rootPath || a.mp3RelPath != b.mp3RelPath)
            return false;
    }
    return true;
}

std::unique_ptr<QMimeData> itemMimeData(QAbstractItemView* list, int row)
{
    return std::unique_ptr<QMimeData>(
        list->model()->mimeData({list->model()->index(row, 0)}));
}

bool sendDrop(QWidget* viewport, const QPoint& position, const QMimeData* mimeData,
              Qt::DropActions actions = Qt::CopyAction)
{
    QDragEnterEvent enter(position, actions, mimeData,
                          Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(viewport, &enter);
    QDragMoveEvent move(position, actions, mimeData,
                        Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(viewport, &move);
    QDropEvent drop(QPointF(position), actions, mimeData,
                    Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(viewport, &drop);
    return enter.isAccepted() && move.isAccepted() && drop.isAccepted()
        && drop.dropAction() == Qt::CopyAction;
}

} // namespace

class TestPlaylistView : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void addAdministrationSwitchAndRestore();
    void removeDeclinedPreservesViewAndStore();
    void removeConfirmsBeforeMutationAndUsesCapturedItem();
    void removeOnlyOneOccurrenceKeepsLibraryFilesAndOtherPlaylist();
    void libraryDropsInsertAtRequestedRows();
    void internalDropsMoveUpAndDown();
    void longPlaylistDragAutoScrollsAndUsesStoreDrop();
    void dropsWhilePlayingDoNotTouchPlayback();
    void dropWithoutDisplayedPlaylistIsRejected();
    void orderingAndDataChangesDoNotTouchPlayback();
    void nonPlaylistOriginsNeverAutoplay();
    void sameSongAutoplayDependsOnPlaylistOrigin();
    void playlistOriginSurvivesLibraryBrowsingAndPlaylistSwitch();
    void explicitPlayAutoplaySettingsAndFailureStates();
    void staleCatalogueIdsResolveBySnapshotAndRepair();
    void movedRootResolvesByUniqueActiveRelativePath();
    void movedRootRejectsRelativePathWithDifferentMetadata();
    void unresolvedAutoplayStopsWithVisibleFeedback();
    void pendingAutoplayCancelledBySynchronousStopAndPlay();
    void rejectedLoadsKeepPlayingContextAndAutoplaySuccessor();
    void resolvedPlaybackIgnoresSongIdCacheWriteFailure();
    void keyboardSelectionAndEscapeKeepPlayback();
    void homeScreenEnterNeverChangesTheSong();
    void presentationFollowsPaneInUseAndPlayback();
    void onlyThePaneInUsePaintsItsSelectionGold();
    void panesStayReadableOnALaptopScreenAtEveryScale();
    void playbackMarkerTracksActualState();
    void movingPastViewportKeepsSelectionFullyVisible();
    void stopAndBusErrorNeverAutoplayWithSuccessors();
    void autoplayOffLeavesFinishedSongLoaded();
    void reorderedSuccessorUsesCurrentOrder();
    void movingPlayingItemUsesItsNewSuccessor();
    void displayedPlaylistDoesNotChangePlaybackOrigin();
    void removedOrDeletedOriginDoesNotAdvance();
    void libraryRefreshPreservesPlaylistPosition();
    void unavailableStoreLeavesLibraryUsable();
};

void TestPlaylistView::initTestCase()
{
    QString error;
    QVERIFY2(KaraokePlayer::initializeGStreamer(&error), qPrintable(error));
}

void TestPlaylistView::addAdministrationSwitchAndRestore()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")));
    const LibrarySongs songs = prepareLibrary(temporary, controller);
    PlaylistStore playlists(temporary.filePath(QStringLiteral("app/playlists.sqlite")));
    QVERIFY(playlists.open(nullptr, controller.libraryRoots()));
    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));

    qint64 restoredId = 0;
    {
        MainWindow window(&player, &settings, &controller, &playlists);
        window.setShowErrorDialogs(false);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        PlaylistView* view = window.playlistView();
        QVERIFY(view->messageLabel()->text().contains(QStringLiteral("No playlists")));
        QVERIFY(!window.libraryView()->addToPlaylistButton()->isEnabled());

        view->setNamePrompt([](QWidget*, const QString&, const QString&) {
            return std::optional<QString>(QStringLiteral("Party"));
        });
        QTest::mouseClick(view->newButton(), Qt::LeftButton);
        QCOMPARE(view->playlistChooser()->count(), 1);
        QCOMPARE(view->playlistChooser()->currentText(), QStringLiteral("Party"));
        const qint64 partyId = view->displayedPlaylistId();

        window.libraryView()->searchBox()->setText(QStringLiteral("First Song"));
        QTRY_COMPARE(window.libraryView()->songResultCount(), 1);
        window.libraryView()->resultsList()->setCurrentIndex(
            window.libraryView()->resultsList()->model()->index(0, 0));
        QVERIFY(window.libraryView()->addToPlaylistButton()->isEnabled());
        QTest::mouseClick(window.libraryView()->addToPlaylistButton(), Qt::LeftButton);
        QCOMPARE(player.state(), KaraokePlayer::State::Empty);
        QCOMPARE(playlists.items(partyId).size(), 1);
        QCOMPARE(view->selectedItemId(), playlists.items(partyId).first().itemId);

        view->setNamePrompt([](QWidget*, const QString&, const QString&) {
            return std::optional<QString>(QStringLiteral("Renamed"));
        });
        QTest::mouseClick(view->renameButton(), Qt::LeftButton);
        QCOMPARE(view->playlistChooser()->currentText(), QStringLiteral("Renamed"));

        qint64 secondPlaylist = 0;
        QVERIFY(playlists.createPlaylist(QStringLiteral("Second"), &secondPlaylist));
        view->refresh();
        view->playlistChooser()->setCurrentIndex(1);
        QCOMPARE(view->displayedPlaylistId(), secondPlaylist);
        QCOMPARE(playlists.lastPlaylistId(), secondPlaylist);
        restoredId = secondPlaylist;

        view->setDeleteConfirmation([](QWidget*, const QString&) { return false; });
        QTest::mouseClick(view->deleteButton(), Qt::LeftButton);
        QVERIFY(playlists.playlist(secondPlaylist));
        view->setDeleteConfirmation([](QWidget*, const QString&) { return true; });
        QTest::mouseClick(view->deleteButton(), Qt::LeftButton);
        QVERIFY(!playlists.playlist(secondPlaylist));
        restoredId = partyId;
        QCOMPARE(view->displayedPlaylistId(), partyId);
        Q_UNUSED(songs)
    }

    QVERIFY(playlists.setLastPlaylistId(restoredId));
    BusTestPlayer secondPlayer;
    MainWindow restored(&secondPlayer, &settings, &controller, &playlists);
    QCOMPARE(restored.playlistView()->displayedPlaylistId(), restoredId);
}

void TestPlaylistView::removeDeclinedPreservesViewAndStore()
{
    QTemporaryDir temporary;
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")));
    const LibrarySongs songs = prepareLibrary(temporary, controller);
    PlaylistStore playlists(temporary.filePath(QStringLiteral("app/playlists.sqlite")));
    QVERIFY(playlists.open(nullptr, controller.libraryRoots()));
    qint64 playlistId = 0;
    QVERIFY(playlists.createPlaylist(QStringLiteral("Keep Everything"), &playlistId));
    for (int i = 0; i < 30; ++i)
        addSong(playlists, controller, playlistId,
                i % 2 ? songs.firstId : songs.secondId);

    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings, &controller, &playlists);
    window.resize(900, 420);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    PlaylistView* view = window.playlistView();
    view->itemList()->setCurrentRow(20);
    view->itemList()->scrollToItem(view->itemList()->currentItem(),
                                   QAbstractItemView::PositionAtTop);
    QCoreApplication::processEvents();
    const QList<PlaylistEntry> beforeRows = playlists.items(playlistId);
    const QList<qint64> beforeIds = itemIds(playlists, playlistId);
    const auto beforePlaylist = playlists.playlist(playlistId);
    QVERIFY(beforePlaylist);
    const qint64 selectedId = view->selectedItemId();
    const int selectedRow = view->itemList()->currentRow();
    const int scroll = view->itemList()->verticalScrollBar()->value();
    QVERIFY(scroll > 0);

    bool asked = false;
    bool hookArgumentsCorrect = false;
    view->setRemoveConfirmation(
        [&](QWidget*, const QString& songText, const QString& playlistName) {
            asked = true;
            hookArgumentsCorrect = playlistName == QStringLiteral("Keep Everything")
                && !songText.startsWith(QStringLiteral("▶ "));
            return false;
        });
    QTest::mouseClick(view->removeButton(), Qt::LeftButton);

    QVERIFY(asked);
    QVERIFY(hookArgumentsCorrect);
    QVERIFY(sameEntries(playlists.items(playlistId), beforeRows));
    QCOMPARE(itemIds(playlists, playlistId), beforeIds);
    const auto afterPlaylist = playlists.playlist(playlistId);
    QVERIFY(afterPlaylist);
    QCOMPARE(afterPlaylist->updatedAt, beforePlaylist->updatedAt);
    QCOMPARE(afterPlaylist->itemCount, beforePlaylist->itemCount);
    QCOMPARE(view->itemList()->count(), beforeIds.size());
    QCOMPARE(view->selectedItemId(), selectedId);
    QCOMPARE(view->itemList()->currentRow(), selectedRow);
    QCOMPARE(view->itemList()->verticalScrollBar()->value(), scroll);
}

void TestPlaylistView::removeConfirmsBeforeMutationAndUsesCapturedItem()
{
    QTemporaryDir temporary;
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")));
    const LibrarySongs songs = prepareLibrary(temporary, controller);
    PlaylistStore playlists(temporary.filePath(QStringLiteral("app/playlists.sqlite")));
    QVERIFY(playlists.open(nullptr, controller.libraryRoots()));
    qint64 playlistId = 0;
    QVERIFY(playlists.createPlaylist(QStringLiteral("Confirm First"), &playlistId));
    addSong(playlists, controller, playlistId, songs.firstId);
    const qint64 removeId = addSong(playlists, controller, playlistId, songs.secondId);
    addSong(playlists, controller, playlistId, songs.thirdId);

    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings, &controller, &playlists);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    PlaylistView* view = window.playlistView();
    view->itemList()->setCurrentRow(1);
    const QList<qint64> beforeIds = itemIds(playlists, playlistId);

    bool checkedBeforeConfirmation = false;
    view->setRemoveConfirmation(
        [&](QWidget*, const QString& songText, const QString& playlistName) {
            checkedBeforeConfirmation = playlists.item(removeId).has_value()
                && itemIds(playlists, playlistId) == beforeIds
                && view->itemList()->count() == beforeIds.size()
                && view->selectedItemId() == removeId
                && playlistName == QStringLiteral("Confirm First")
                && songText.contains(QStringLiteral("Second Song"))
                && !songText.startsWith(QStringLiteral("▶ "));
            return true;
        });
    QTest::mouseClick(view->removeButton(), Qt::LeftButton);

    QVERIFY(checkedBeforeConfirmation);
    QVERIFY(!playlists.item(removeId));
    QCOMPARE(itemIds(playlists, playlistId),
             QList<qint64>({beforeIds.at(0), beforeIds.at(2)}));
    QCOMPARE(view->itemList()->count(), 2);
    QCOMPARE(view->itemList()->currentRow(), 1);
    QCOMPARE(view->selectedItemId(), beforeIds.at(2));
}

void TestPlaylistView::removeOnlyOneOccurrenceKeepsLibraryFilesAndOtherPlaylist()
{
    QTemporaryDir temporary;
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")));
    const LibrarySongs songs = prepareLibrary(temporary, controller);
    PlaylistStore playlists(temporary.filePath(QStringLiteral("app/playlists.sqlite")));
    QVERIFY(playlists.open(nullptr, controller.libraryRoots()));
    qint64 firstPlaylist = 0;
    qint64 otherPlaylist = 0;
    QVERIFY(playlists.createPlaylist(QStringLiteral("Duplicates"), &firstPlaylist));
    QVERIFY(playlists.createPlaylist(QStringLiteral("Elsewhere"), &otherPlaylist));
    const qint64 removed = addSong(playlists, controller, firstPlaylist, songs.firstId);
    const qint64 remainingDuplicate =
        addSong(playlists, controller, firstPlaylist, songs.firstId);
    const qint64 otherOccurrence =
        addSong(playlists, controller, otherPlaylist, songs.firstId);

    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings, &controller, &playlists);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    PlaylistView* view = window.playlistView();
    view->playlistChooser()->setCurrentIndex(0);
    view->itemList()->setCurrentRow(0);
    view->setRemoveConfirmation(
        [](QWidget*, const QString&, const QString&) { return true; });
    QTest::mouseClick(view->removeButton(), Qt::LeftButton);

    QVERIFY(!playlists.item(removed));
    QVERIFY(playlists.item(remainingDuplicate));
    QVERIFY(playlists.item(otherOccurrence));
    QCOMPARE(playlists.items(firstPlaylist).size(), 1);
    QCOMPARE(playlists.items(otherPlaylist).size(), 1);
    QVERIFY(controller.songRef(songs.firstId));
    QCOMPARE(controller.search(QStringLiteral("First Song"), 10).size(), 1);
    QVERIFY(QFileInfo::exists(songs.firstMp3));
    QVERIFY(QFileInfo::exists(QFileInfo(songs.firstMp3).path()
                              + QStringLiteral("/PV001-01 - Test Singer - First Song.cdg")));
}

void TestPlaylistView::libraryDropsInsertAtRequestedRows()
{
    QTemporaryDir temporary;
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")));
    const LibrarySongs songs = prepareLibrary(temporary, controller);
    PlaylistStore playlists(temporary.filePath(QStringLiteral("app/playlists.sqlite")));
    QVERIFY(playlists.open(nullptr, controller.libraryRoots()));
    qint64 playlistId = 0;
    QVERIFY(playlists.createPlaylist(QStringLiteral("Drop Targets"), &playlistId));
    addSong(playlists, controller, playlistId, songs.secondId);
    addSong(playlists, controller, playlistId, songs.fourthId);

    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings, &controller, &playlists);
    window.resize(1000, 700);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    LibraryView* library = window.libraryView();
    QVERIFY(library->resultsList()->dragEnabled());
    QCOMPARE(library->resultsList()->dragDropMode(), QAbstractItemView::DragOnly);
    QCOMPARE(library->resultsList()->defaultDropAction(), Qt::CopyAction);
    QVERIFY(!library->resultsList()->acceptDrops());
    PlaylistView* view = window.playlistView();
    QListWidget* list = view->itemList();
    QCoreApplication::processEvents();

    library->searchBox()->setText(QStringLiteral("First Song"));
    library->refreshSearch();
    QCOMPARE(library->songResultCount(), 1);
    auto firstTop = itemMimeData(library->resultsList(), 0);
    QVERIFY(firstTop);
    QVERIFY(firstTop->hasFormat(QStringLiteral("application/x-fks-song-id")));
    QVERIFY(!firstTop->hasFormat(
        QStringLiteral("application/x-qabstractitemmodeldatalist")));
    QRect target = list->visualItemRect(list->item(0));

    QMimeData genericModelData;
    genericModelData.setData(QStringLiteral("application/x-qabstractitemmodeldatalist"),
                             QByteArray("not accepted"));
    QVERIFY(!sendDrop(list->viewport(), target.center(), &genericModelData));
    QCOMPARE(songIds(playlists, playlistId),
             QList<qint64>({songs.secondId, songs.fourthId}));

    QVERIFY(sendDrop(list->viewport(), QPoint(target.center().x(), target.top() + 1),
                     firstTop.get()));
    QCOMPARE(songIds(playlists, playlistId),
             QList<qint64>({songs.firstId, songs.secondId, songs.fourthId}));
    QCOMPARE(playlists.item(view->selectedItemId())->songId, songs.firstId);

    library->searchBox()->setText(QStringLiteral("Third Song"));
    library->refreshSearch();
    auto thirdUpper = itemMimeData(library->resultsList(), 0);
    QVERIFY(thirdUpper);
    target = list->visualItemRect(list->item(2));
    QVERIFY(sendDrop(list->viewport(), QPoint(target.center().x(), target.top() + 1),
                     thirdUpper.get()));
    QCOMPARE(songIds(playlists, playlistId),
             QList<qint64>({songs.firstId, songs.secondId,
                            songs.thirdId, songs.fourthId}));
    QCOMPARE(playlists.item(view->selectedItemId())->songId, songs.thirdId);

    library->searchBox()->setText(QStringLiteral("First Song"));
    library->refreshSearch();
    auto firstLower = itemMimeData(library->resultsList(), 0);
    QVERIFY(firstLower);
    target = list->visualItemRect(list->item(1));
    QVERIFY(sendDrop(list->viewport(), QPoint(target.center().x(), target.bottom() - 1),
                     firstLower.get()));
    QCOMPARE(songIds(playlists, playlistId),
             QList<qint64>({songs.firstId, songs.secondId, songs.firstId,
                            songs.thirdId, songs.fourthId}));
    QCOMPARE(playlists.item(view->selectedItemId())->songId, songs.firstId);

    library->searchBox()->setText(QStringLiteral("Second Song"));
    library->refreshSearch();
    auto secondEnd = itemMimeData(library->resultsList(), 0);
    QVERIFY(secondEnd);
    const int endY = list->visualItemRect(list->item(list->count() - 1)).bottom() + 8;
    const QPoint emptyPoint(list->viewport()->width() / 2,
                            qMin(endY, list->viewport()->height() - 2));
    QVERIFY(!list->indexAt(emptyPoint).isValid());
    QVERIFY(sendDrop(list->viewport(), emptyPoint, secondEnd.get()));
    QCOMPARE(songIds(playlists, playlistId),
             QList<qint64>({songs.firstId, songs.secondId, songs.firstId,
                            songs.thirdId, songs.fourthId, songs.secondId}));
    QCOMPARE(playlists.item(view->selectedItemId())->songId, songs.secondId);
}

void TestPlaylistView::internalDropsMoveUpAndDown()
{
    QTemporaryDir temporary;
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")));
    const LibrarySongs songs = prepareLibrary(temporary, controller);
    PlaylistStore playlists(temporary.filePath(QStringLiteral("app/playlists.sqlite")));
    QVERIFY(playlists.open(nullptr, controller.libraryRoots()));
    qint64 playlistId = 0;
    QVERIFY(playlists.createPlaylist(QStringLiteral("Reorder Drops"), &playlistId));
    const qint64 first = addSong(playlists, controller, playlistId, songs.firstId);
    const qint64 second = addSong(playlists, controller, playlistId, songs.secondId);
    const qint64 third = addSong(playlists, controller, playlistId, songs.thirdId);
    const qint64 fourth = addSong(playlists, controller, playlistId, songs.fourthId);

    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings, &controller, &playlists);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    PlaylistView* view = window.playlistView();
    QListWidget* list = view->itemList();
    QCoreApplication::processEvents();

    auto moveUp = itemMimeData(list, 2);
    QVERIFY(moveUp);
    QVERIFY(moveUp->hasFormat(QStringLiteral("application/x-fks-playlist-item-id")));
    QRect target = list->visualItemRect(list->item(0));
    QVERIFY(sendDrop(list->viewport(), QPoint(target.center().x(), target.top() + 1),
                     moveUp.get(), Qt::CopyAction | Qt::MoveAction));
    QCOMPARE(itemIds(playlists, playlistId),
             QList<qint64>({third, first, second, fourth}));
    QCOMPARE(view->selectedItemId(), third);

    auto moveDown = itemMimeData(list, 0);
    QVERIFY(moveDown);
    target = list->visualItemRect(list->item(3));
    QVERIFY(sendDrop(list->viewport(), QPoint(target.center().x(), target.bottom() - 1),
                     moveDown.get(), Qt::CopyAction | Qt::MoveAction));
    QCOMPARE(itemIds(playlists, playlistId),
             QList<qint64>({first, second, fourth, third}));
    QCOMPARE(view->selectedItemId(), third);
    QCOMPARE(list->count(), 4);
}

void TestPlaylistView::longPlaylistDragAutoScrollsAndUsesStoreDrop()
{
    QTemporaryDir temporary;
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")));
    const LibrarySongs songs = prepareLibrary(temporary, controller);
    PlaylistStore playlists(temporary.filePath(QStringLiteral("app/playlists.sqlite")));
    QVERIFY(playlists.open(nullptr, controller.libraryRoots()));
    qint64 playlistId = 0;
    QVERIFY(playlists.createPlaylist(QStringLiteral("Long Reorder"), &playlistId));
    for (int i = 0; i < 60; ++i)
        addSong(playlists, controller, playlistId, songs.firstId);

    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings, &controller, &playlists);
    window.resize(900, 600);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    PlaylistView* view = window.playlistView();
    QListWidget* list = view->itemList();
    QCoreApplication::processEvents();
    QCOMPARE(list->count(), 60);
    QVERIFY(list->showDropIndicator());
    QVERIFY(list->verticalScrollBar()->maximum() > 0);
    list->verticalScrollBar()->setValue(0);

    const QList<qint64> before = itemIds(playlists, playlistId);
    auto dragged = itemMimeData(list, 0);
    QVERIFY(dragged);
    QVERIFY(dragged->hasFormat(QStringLiteral("application/x-fks-playlist-item-id")));
    const Qt::DropActions actions = Qt::CopyAction | Qt::MoveAction;
    const QPoint edge(list->viewport()->width() / 2,
                      list->viewport()->height() - 3);
    QDragEnterEvent enter(edge, actions, dragged.get(),
                          Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(list->viewport(), &enter);
    QVERIFY(enter.isAccepted());
    QCOMPARE(enter.dropAction(), Qt::CopyAction);

    QElapsedTimer autoScrollTimer;
    autoScrollTimer.start();
    while (autoScrollTimer.elapsed() < 1000) {
        QDragMoveEvent move(edge, actions, dragged.get(),
                            Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(list->viewport(), &move);
        QVERIFY(move.isAccepted());
        QCOMPARE(move.dropAction(), Qt::CopyAction);
        QCoreApplication::processEvents();
        QTest::qWait(20);
    }
    QVERIFY2(list->verticalScrollBar()->value() > 0,
             "Dragging at the viewport edge did not auto-scroll the playlist");

    QDragMoveEvent finalMove(edge, actions, dragged.get(),
                             Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(list->viewport(), &finalMove);
    QVERIFY(finalMove.isAccepted());
    const QModelIndex targetIndex = list->indexAt(edge);
    QVERIFY(targetIndex.isValid());
    const QRect targetRect = list->visualRect(targetIndex);
    int targetRow = targetIndex.row()
        + (edge.y() < targetRect.top() + targetRect.height() / 2.0 ? 0 : 1);
    if (targetRow > 0)
        --targetRow;
    targetRow = qBound(0, targetRow, list->count() - 1);
    QList<qint64> expected = before;
    const qint64 draggedId = expected.takeFirst();
    expected.insert(targetRow, draggedId);

    QDropEvent drop(QPointF(edge), actions, dragged.get(),
                    Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(list->viewport(), &drop);
    QVERIFY(drop.isAccepted());
    QCOMPARE(drop.dropAction(), Qt::CopyAction);
    QCOMPARE(itemIds(playlists, playlistId), expected);
    QCOMPARE(list->count(), before.size());
    QCOMPARE(view->selectedItemId(), draggedId);
}

void TestPlaylistView::dropsWhilePlayingDoNotTouchPlayback()
{
    QTemporaryDir temporary;
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")));
    const LibrarySongs songs = prepareLibrary(temporary, controller);
    PlaylistStore playlists(temporary.filePath(QStringLiteral("app/playlists.sqlite")));
    QVERIFY(playlists.open(nullptr, controller.libraryRoots()));
    qint64 playlistId = 0;
    QVERIFY(playlists.createPlaylist(QStringLiteral("Playing Drops"), &playlistId));
    const qint64 playingItem = addSong(playlists, controller, playlistId, songs.secondId);
    addSong(playlists, controller, playlistId, songs.firstId);

    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings, &controller, &playlists);
    window.resize(1000, 700);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    LibraryView* library = window.libraryView();
    library->searchBox()->setText(QStringLiteral("Third Song"));
    library->refreshSearch();
    auto libraryMime = itemMimeData(library->resultsList(), 0);
    QVERIFY(libraryMime);
    PlaylistView* view = window.playlistView();
    QListWidget* list = view->itemList();
    view->itemList()->setCurrentRow(0);
    QTest::mouseClick(view->playButton(), Qt::LeftButton);
    QCOMPARE(player.state(), KaraokePlayer::State::Playing);
    QCOMPARE(window.playlistPlayback()->context()->itemId, playingItem);
    player.setKeySemitones(2);
    player.setTempoPercent(104);
    QTest::qWait(100);
    const qint64 beforeLibraryDrop = player.positionMs();

    const QPoint emptyPoint(list->viewport()->width() / 2,
        list->visualItemRect(list->item(list->count() - 1)).bottom() + 8);
    QVERIFY(!list->indexAt(emptyPoint).isValid());
    QVERIFY(sendDrop(list->viewport(), emptyPoint, libraryMime.get()));
    QCOMPARE(player.state(), KaraokePlayer::State::Playing);
    QCOMPARE(window.playlistPlayback()->context()->itemId, playingItem);
    QVERIFY(player.positionMs() >= beforeLibraryDrop);
    QCOMPARE(player.keySemitones(), 2);
    QCOMPARE(player.tempoPercent(), 104);
    QVERIFY(sameFile(player.song().mp3Path, songs.secondMp3));

    auto internalMime = itemMimeData(list, 0);
    QVERIFY(internalMime);
    const qint64 beforeInternalDrop = player.positionMs();
    const QPoint newEnd(list->viewport()->width() / 2,
        list->visualItemRect(list->item(list->count() - 1)).bottom() + 8);
    QVERIFY(sendDrop(list->viewport(), newEnd, internalMime.get(),
                     Qt::CopyAction | Qt::MoveAction));
    QCOMPARE(itemIds(playlists, playlistId).last(), playingItem);
    QCOMPARE(view->selectedItemId(), playingItem);
    QCOMPARE(player.state(), KaraokePlayer::State::Playing);
    QCOMPARE(window.playlistPlayback()->context()->itemId, playingItem);
    QVERIFY(player.positionMs() >= beforeInternalDrop);
    QCOMPARE(player.keySemitones(), 2);
    QCOMPARE(player.tempoPercent(), 104);
    QVERIFY(sameFile(player.song().mp3Path, songs.secondMp3));
    player.stop();
}

void TestPlaylistView::dropWithoutDisplayedPlaylistIsRejected()
{
    QTemporaryDir temporary;
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")));
    prepareLibrary(temporary, controller);
    PlaylistStore playlists(temporary.filePath(QStringLiteral("app/playlists.sqlite")));
    QVERIFY(playlists.open(nullptr, controller.libraryRoots()));
    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings, &controller, &playlists);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    LibraryView* library = window.libraryView();
    library->searchBox()->setText(QStringLiteral("First Song"));
    library->refreshSearch();
    auto mimeData = itemMimeData(library->resultsList(), 0);
    QVERIFY(mimeData);
    PlaylistView* view = window.playlistView();
    QCOMPARE(view->displayedPlaylistId(), 0);

    QVERIFY(!sendDrop(view->itemList()->viewport(), QPoint(4, 4), mimeData.get()));
    QCOMPARE(playlists.playlists().size(), 0);
    QCOMPARE(view->itemList()->count(), 0);
}

void TestPlaylistView::orderingAndDataChangesDoNotTouchPlayback()
{
    QTemporaryDir temporary;
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")));
    const LibrarySongs songs = prepareLibrary(temporary, controller);
    PlaylistStore playlists(temporary.filePath(QStringLiteral("app/playlists.sqlite")));
    QVERIFY(playlists.open(nullptr, controller.libraryRoots()));
    qint64 firstPlaylist = 0;
    qint64 secondPlaylist = 0;
    QVERIFY(playlists.createPlaylist(QStringLiteral("Long"), &firstPlaylist));
    QVERIFY(playlists.createPlaylist(QStringLiteral("Other"), &secondPlaylist));
    QList<qint64> itemIds;
    for (int i = 0; i < 18; ++i)
        itemIds.append(addSong(playlists, controller, firstPlaylist,
                               i % 2 ? songs.secondId : songs.firstId));

    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings, &controller, &playlists);
    window.setShowErrorDialogs(false);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    PlaylistView* view = window.playlistView();
    view->itemList()->setCurrentRow(1);
    QTest::mouseClick(view->playButton(), Qt::LeftButton);
    QCOMPARE(player.state(), KaraokePlayer::State::Playing);
    const QString playingPath = player.song().mp3Path;
    player.setKeySemitones(2);
    player.setTempoPercent(104);
    QTest::qWait(150);
    const qint64 before = player.positionMs();

    QTest::keyClick(&window, Qt::Key_Escape);
    view->itemList()->setCurrentRow(12);
    const qint64 selected = view->selectedItemId();
    view->itemList()->setFocus();
    QTest::keyClick(view->itemList(), Qt::Key_Up);
    QCOMPARE(view->selectedItemId(), selected);
    QCOMPARE(playlists.item(selected)->position, 11);
    QVERIFY(view->itemList()->visualItemRect(view->itemList()->currentItem())
                .intersects(view->itemList()->viewport()->rect()));
    QTest::keyClick(view->itemList(), Qt::Key_Down);
    QCOMPARE(playlists.item(selected)->position, 12);
    QTest::mouseClick(view->moveUpButton(), Qt::LeftButton);
    QCOMPARE(playlists.item(selected)->position, 11);
    QTest::mouseClick(view->moveDownButton(), Qt::LeftButton);
    QCOMPARE(playlists.item(selected)->position, 12);

    view->playlistChooser()->setCurrentIndex(1);
    view->playlistChooser()->setCurrentIndex(0);
    window.libraryView()->searchBox()->setText(QStringLiteral("First Song"));
    QTRY_COMPARE(window.libraryView()->songResultCount(), 1);
    window.libraryView()->resultsList()->setCurrentIndex(
        window.libraryView()->resultsList()->model()->index(0, 0));
    QTest::mouseClick(window.libraryView()->addToPlaylistButton(), Qt::LeftButton);
    QCOMPARE(player.state(), KaraokePlayer::State::Playing);
    QCOMPARE(player.song().mp3Path, playingPath);
    QVERIFY(player.positionMs() >= before);
    QCOMPARE(player.keySemitones(), 2);
    QCOMPARE(player.tempoPercent(), 104);

    view->itemList()->setCurrentRow(1);
    QCOMPARE(window.playlistPlayback()->context()->itemId, itemIds.at(1));
    view->setRemoveConfirmation(
        [](QWidget*, const QString&, const QString&) { return true; });
    QTest::mouseClick(view->removeButton(), Qt::LeftButton);
    QCOMPARE(player.state(), KaraokePlayer::State::Playing);
    QCOMPARE(player.song().mp3Path, playingPath);
}

void TestPlaylistView::nonPlaylistOriginsNeverAutoplay()
{
    QTemporaryDir temporary;
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")));
    const LibrarySongs songs = prepareLibrary(temporary, controller);
    PlaylistStore playlists(temporary.filePath(QStringLiteral("app/playlists.sqlite")));
    QVERIFY(playlists.open(nullptr, controller.libraryRoots()));
    QVERIFY(playlists.setAutoplay(true));
    qint64 playlistId = 0;
    QVERIFY(playlists.createPlaylist(QStringLiteral("S then T"), &playlistId));
    addSong(playlists, controller, playlistId, songs.firstId);
    addSong(playlists, controller, playlistId, songs.secondId);

    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings, &controller, &playlists);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    LibraryView* library = window.libraryView();
    library->searchBox()->clear();
    library->refreshSearch();
    const int firstRow = libraryRowForSongId(library, songs.firstId);
    QVERIFY(firstRow >= 0);
    library->resultsList()->setCurrentIndex(
        library->resultsList()->model()->index(firstRow, 0));
    QTest::mouseClick(library->singButton(), Qt::LeftButton);
    QVERIFY(!window.playlistPlayback()->context());
    QTest::mouseClick(window.playButton(), Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(player.state(), KaraokePlayer::State::Finished, 4000);
    QTest::qWait(50);
    QVERIFY(sameFile(player.song().mp3Path, songs.firstMp3));
    QVERIFY(!window.playlistPlayback()->context());

    QVERIFY(window.openSong(songs.firstMp3));
    QVERIFY(!window.playlistPlayback()->context());
    QTest::mouseClick(window.playButton(), Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(player.state(), KaraokePlayer::State::Finished, 4000);
    QTest::qWait(50);
    QVERIFY(sameFile(player.song().mp3Path, songs.firstMp3));
    QVERIFY(!window.playlistPlayback()->context());
}

void TestPlaylistView::sameSongAutoplayDependsOnPlaylistOrigin()
{
    QTemporaryDir temporary;
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")));
    const LibrarySongs songs = prepareLibrary(temporary, controller);
    PlaylistStore playlists(temporary.filePath(QStringLiteral("app/playlists.sqlite")));
    QVERIFY(playlists.open(nullptr, controller.libraryRoots()));
    QVERIFY(playlists.setAutoplay(true));
    qint64 playlistId = 0;
    QVERIFY(playlists.createPlaylist(QStringLiteral("Same S then T"), &playlistId));
    const qint64 firstItem = addSong(playlists, controller, playlistId, songs.firstId);
    const qint64 secondItem = addSong(playlists, controller, playlistId, songs.secondId);

    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings, &controller, &playlists);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    LibraryView* library = window.libraryView();
    library->searchBox()->clear();
    library->refreshSearch();
    const int firstRow = libraryRowForSongId(library, songs.firstId);
    QVERIFY(firstRow >= 0);
    library->resultsList()->setCurrentIndex(
        library->resultsList()->model()->index(firstRow, 0));
    QTest::mouseClick(library->singButton(), Qt::LeftButton);
    QTest::mouseClick(window.playButton(), Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(player.state(), KaraokePlayer::State::Finished, 4000);
    QVERIFY(sameFile(player.song().mp3Path, songs.firstMp3));
    QVERIFY(!window.playlistPlayback()->context());

    PlaylistView* playlist = window.playlistView();
    playlist->itemList()->setCurrentRow(0);
    QTest::mouseClick(playlist->playButton(), Qt::LeftButton);
    QVERIFY(window.playlistPlayback()->context());
    QCOMPARE(window.playlistPlayback()->context()->itemId, firstItem);
    QTRY_VERIFY_WITH_TIMEOUT(window.playlistPlayback()->context()
                                 && window.playlistPlayback()->context()->itemId == secondItem,
                             4000);
    QTRY_COMPARE_WITH_TIMEOUT(player.state(), KaraokePlayer::State::Playing, 2000);
    QVERIFY(sameFile(player.song().mp3Path, songs.secondMp3));
    player.stop();
}

void TestPlaylistView::playlistOriginSurvivesLibraryBrowsingAndPlaylistSwitch()
{
    QTemporaryDir temporary;
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")));
    const LibrarySongs songs = prepareLibrary(temporary, controller);
    PlaylistStore playlists(temporary.filePath(QStringLiteral("app/playlists.sqlite")));
    QVERIFY(playlists.open(nullptr, controller.libraryRoots()));
    QVERIFY(playlists.setAutoplay(true));
    qint64 originPlaylist = 0;
    qint64 browsedPlaylist = 0;
    QVERIFY(playlists.createPlaylist(QStringLiteral("Origin S then T"), &originPlaylist));
    QVERIFY(playlists.createPlaylist(QStringLiteral("Browsed"), &browsedPlaylist));
    const qint64 originItem = addSong(
        playlists, controller, originPlaylist, songs.secondId);
    const qint64 successorItem = addSong(
        playlists, controller, originPlaylist, songs.thirdId);
    addSong(playlists, controller, browsedPlaylist, songs.fourthId);
    addSong(playlists, controller, browsedPlaylist, songs.firstId);

    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings, &controller, &playlists);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    PlaylistView* playlist = window.playlistView();
    playlist->playlistChooser()->setCurrentIndex(0);
    playlist->itemList()->setCurrentRow(0);
    QTest::mouseClick(playlist->playButton(), Qt::LeftButton);
    QVERIFY(window.playlistPlayback()->context());
    QCOMPARE(window.playlistPlayback()->context()->itemId, originItem);

    QTest::keyClick(&window, Qt::Key_Escape);
    playlist->playlistChooser()->setCurrentIndex(1);
    playlist->itemList()->setCurrentRow(1);
    LibraryView* library = window.libraryView();
    library->searchBox()->clear();
    library->refreshSearch();
    const int otherRow = libraryRowForSongId(library, songs.fourthId);
    QVERIFY(otherRow >= 0);
    library->resultsList()->setCurrentIndex(
        library->resultsList()->model()->index(otherRow, 0));
    QVERIFY(window.playlistPlayback()->context());
    QCOMPARE(window.playlistPlayback()->context()->playlistId, originPlaylist);
    QCOMPARE(window.playlistPlayback()->context()->itemId, originItem);

    QTRY_VERIFY_WITH_TIMEOUT(window.playlistPlayback()->context()
                                 && window.playlistPlayback()->context()->itemId == successorItem,
                             5000);
    QTRY_COMPARE_WITH_TIMEOUT(player.state(), KaraokePlayer::State::Playing, 2000);
    QCOMPARE(window.playlistPlayback()->context()->playlistId, originPlaylist);
    QCOMPARE(playlist->displayedPlaylistId(), browsedPlaylist);
    QVERIFY(sameFile(player.song().mp3Path, songs.thirdMp3));
    player.stop();
}

void TestPlaylistView::explicitPlayAutoplaySettingsAndFailureStates()
{
    QTemporaryDir temporary;
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")));
    const LibrarySongs songs = prepareLibrary(temporary, controller);
    PlaylistStore playlists(temporary.filePath(QStringLiteral("app/playlists.sqlite")));
    QVERIFY(playlists.open(nullptr, controller.libraryRoots()));
    QVERIFY(playlists.setAutoplay(true));
    qint64 playlist = 0;
    QVERIFY(playlists.createPlaylist(QStringLiteral("Autoplay"), &playlist));
    const qint64 firstItem = addSong(playlists, controller, playlist, songs.firstId);
    const qint64 secondItem = addSong(playlists, controller, playlist, songs.secondId);

    const SongPairResult secondPair = resolveSongPair(songs.secondMp3);
    QVERIFY(secondPair.pair.isValid());
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    QVERIFY(settings.store(songIdentity(secondPair.pair), SongSettings{3, 112},
                           secondPair.pair));
    BusTestPlayer player;
    MainWindow window(&player, &settings, &controller, &playlists);
    window.setShowErrorDialogs(false);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    PlaylistView* view = window.playlistView();
    view->itemList()->setCurrentRow(0);

    bool secondEnteredPlayingWithSettings = false;
    connect(&player, &KaraokePlayer::stateChanged, this,
            [&](KaraokePlayer::State state) {
        if (state == KaraokePlayer::State::Playing
            && QFileInfo(player.song().mp3Path).canonicalFilePath()
                == QFileInfo(songs.secondMp3).canonicalFilePath()) {
            secondEnteredPlayingWithSettings = player.keySemitones() == 3
                && player.tempoPercent() == 112;
        }
    });
    QTest::mouseClick(view->playButton(), Qt::LeftButton);
    QCOMPARE(player.state(), KaraokePlayer::State::Playing);
    QCOMPARE(window.playlistPlayback()->context()->itemId, firstItem);
    QTRY_COMPARE_WITH_TIMEOUT(window.playlistPlayback()->context()->itemId,
                              secondItem, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(player.state(), KaraokePlayer::State::Playing, 2000);
    QVERIFY(secondEnteredPlayingWithSettings);
    QCOMPARE(player.keySemitones(), 3);
    QCOMPARE(player.tempoPercent(), 112);

    player.stop();

    // The same song in another playlist uses the same fingerprint settings.
    qint64 otherPlaylist = 0;
    QVERIFY(playlists.createPlaylist(QStringLiteral("Other"), &otherPlaylist));
    const qint64 duplicate = addSong(playlists, controller, otherPlaylist, songs.secondId);
    view->refresh();
    view->playlistChooser()->setCurrentIndex(1);
    view->itemList()->setCurrentRow(0);
    QTest::mouseClick(view->playButton(), Qt::LeftButton);
    QCOMPARE(window.playlistPlayback()->context()->itemId, duplicate);
    QCOMPARE(player.keySemitones(), 3);
    QCOMPARE(player.tempoPercent(), 112);

    QVERIFY(window.openSong(songs.firstMp3));
    QVERIFY(!window.playlistPlayback()->context());
}

void TestPlaylistView::staleCatalogueIdsResolveBySnapshotAndRepair()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("music"));
    const QString cataloguePath = temporary.filePath(QStringLiteral("app/library.sqlite"));
    PlaylistStore playlists(temporary.filePath(QStringLiteral("app/playlists.sqlite")));
    qint64 playlistId = 0;
    QList<PlaylistEntry> staleEntries;

    {
        LibraryController original(cataloguePath);
        const LibrarySongs songs = prepareLibraryAt(root, original);
        QVERIFY(playlists.open(nullptr, original.libraryRoots()));
        QVERIFY(playlists.createPlaylist(QStringLiteral("Rebuilt"), &playlistId));
        addSong(playlists, original, playlistId, songs.firstId);
        addSong(playlists, original, playlistId, songs.secondId);
        addSong(playlists, original, playlistId, songs.thirdId);
        addSong(playlists, original, playlistId, songs.fourthId);
        staleEntries = playlists.items(playlistId);
    }

    QVERIFY(QFile::remove(cataloguePath));
    QFile::remove(cataloguePath + QStringLiteral("-wal"));
    QFile::remove(cataloguePath + QStringLiteral("-shm"));
    QVERIFY(QDir(root).removeRecursively());
    QVERIFY(QDir().mkpath(root));
    QVERIFY(writeSong(root + QStringLiteral("/AA000-00 - Other Singer - Extra Song"), 450));
    QVERIFY(writeSong(root + QStringLiteral("/PV001-04 - Test Singer - Fourth Song"), 450));
    QVERIFY(writeSong(root + QStringLiteral("/PV001-03 - Test Singer - Third Song"), 450));
    QVERIFY(writeSong(root + QStringLiteral("/PV001-02 - Test Singer - Second Song"), 1200));
    QVERIFY(writeSong(root + QStringLiteral("/PV001-01 - Test Singer - First Song"), 450));

    LibraryController rebuilt(cataloguePath);
    QSignalSpy ready(&rebuilt, &LibraryController::libraryReady);
    QSignalSpy finished(&rebuilt, &LibraryController::scanFinished);
    QVERIFY(rebuilt.chooseRoot(root));
    if (ready.count() == 0)
        QVERIFY(ready.wait(5000));
    if (finished.count() == 0)
        QVERIFY(finished.wait(5000));

    int collisionRow = -1;
    std::optional<SongRef> collidedSong;
    PlaylistSongResolution expected;
    for (int row = 0; row < staleEntries.size(); ++row) {
        const PlaylistEntry& entry = staleEntries.at(row);
        const auto currentAtOldId = rebuilt.songRef(entry.songId);
        if (currentAtOldId
            && !Catalogue::playlistSnapshotPathsMatch(
                entry.rootPath, entry.mp3RelPath,
                currentAtOldId->rootPath, currentAtOldId->mp3RelPath)) {
            collisionRow = row;
            collidedSong = currentAtOldId;
            expected = rebuilt.resolvePlaylistSong(entry);
            break;
        }
    }
    QVERIFY2(collisionRow >= 0,
             "Fresh scan must reuse an old song ID for a different song");
    QVERIFY(collidedSong);
    QVERIFY(expected);
    QVERIFY(expected.updateStoredSongId);
    QVERIFY(expected.songId != staleEntries.at(collisionRow).songId);

    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings, &rebuilt, &playlists);
    window.setShowErrorDialogs(false);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    PlaylistView* view = window.playlistView();
    QCOMPARE(playlists.item(staleEntries.at(collisionRow).itemId)->songId,
             expected.songId);
    const QString displayed = view->itemList()->item(collisionRow)->text();
    QVERIFY(displayed.contains(staleEntries.at(collisionRow).title));
    QVERIFY(!displayed.contains(collidedSong->title));

    view->itemList()->setCurrentRow(collisionRow);
    QTest::mouseClick(view->playButton(), Qt::LeftButton);
    QCOMPARE(player.state(), KaraokePlayer::State::Playing);
    QVERIFY(sameFile(player.song().mp3Path,
                     QDir(root).filePath(staleEntries.at(collisionRow).mp3RelPath)));
    QCOMPARE(window.playlistPlayback()->context()->itemId,
             staleEntries.at(collisionRow).itemId);
    player.stop();
}

void TestPlaylistView::movedRootResolvesByUniqueActiveRelativePath()
{
    QTemporaryDir temporary;
    const QString originalRoot = temporary.filePath(QStringLiteral("old-drive"));
    const QString movedRoot = temporary.filePath(QStringLiteral("new-drive"));
    const QString cataloguePath = temporary.filePath(QStringLiteral("app/library.sqlite"));
    PlaylistStore playlists(temporary.filePath(QStringLiteral("app/playlists.sqlite")));
    qint64 playlistId = 0;
    qint64 itemId = 0;
    PlaylistEntry snapshot;

    {
        LibraryController original(cataloguePath);
        const LibrarySongs songs = prepareLibraryAt(originalRoot, original);
        QVERIFY(playlists.open(nullptr, original.libraryRoots()));
        QVERIFY(playlists.createPlaylist(QStringLiteral("Moved"), &playlistId));
        itemId = addSong(playlists, original, playlistId, songs.secondId);
        snapshot = *playlists.item(itemId);
    }

    QVERIFY(QFile::remove(cataloguePath));
    QFile::remove(cataloguePath + QStringLiteral("-wal"));
    QFile::remove(cataloguePath + QStringLiteral("-shm"));
    LibraryController moved(cataloguePath);
    QVERIFY(QDir().mkpath(movedRoot));
    QVERIFY(writeSong(movedRoot + QStringLiteral("/AA000-00 - Other Singer - Extra Song"),
                      450));
    const LibrarySongs movedSongs = prepareLibraryAt(movedRoot, moved);
    const PlaylistSongResolution resolution = moved.resolvePlaylistSong(snapshot);
    QCOMPARE(resolution.songId, movedSongs.secondId);
    QVERIFY(resolution.songId != snapshot.songId);
    QVERIFY(resolution.updateStoredSongId);

    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings, &moved, &playlists);
    window.setShowErrorDialogs(false);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QCOMPARE(playlists.item(itemId)->songId, movedSongs.secondId);
    QCOMPARE(playlists.item(itemId)->rootPath, snapshot.rootPath);
    window.playlistView()->itemList()->setCurrentRow(0);
    QTest::mouseClick(window.playlistView()->playButton(), Qt::LeftButton);
    QCOMPARE(player.state(), KaraokePlayer::State::Playing);
    QVERIFY(sameFile(player.song().mp3Path, movedSongs.secondMp3));
    player.stop();
}

void TestPlaylistView::movedRootRejectsRelativePathWithDifferentMetadata()
{
    QTemporaryDir temporary;
    const QString originalRoot = temporary.filePath(QStringLiteral("old-drive"));
    const QString movedRoot = temporary.filePath(QStringLiteral("new-drive"));
    const QString cataloguePath = temporary.filePath(QStringLiteral("app/library.sqlite"));
    PlaylistStore playlists(temporary.filePath(QStringLiteral("app/playlists.sqlite")));
    qint64 playlistId = 0;
    qint64 itemId = 0;
    PlaylistEntry snapshot;

    {
        LibraryController original(cataloguePath);
        const LibrarySongs songs = prepareLibraryAt(originalRoot, original);
        QVERIFY(playlists.open(nullptr, original.libraryRoots()));
        QVERIFY(playlists.createPlaylist(QStringLiteral("Moved Different"), &playlistId));
        itemId = addSong(playlists, original, playlistId, songs.secondId);
        QVERIFY(PlaylistStoreTestAccess::setSnapshotMetadata(
            playlists, itemId, QStringLiteral("Snapshot Different Song"),
            QStringLiteral("OTHER"), 99));
        snapshot = *playlists.item(itemId);
    }

    QVERIFY(QFile::remove(cataloguePath));
    QFile::remove(cataloguePath + QStringLiteral("-wal"));
    QFile::remove(cataloguePath + QStringLiteral("-shm"));
    LibraryController moved(cataloguePath);
    const LibrarySongs movedSongs = prepareLibraryAt(movedRoot, moved);
    Q_UNUSED(movedSongs)
    QVERIFY(!moved.resolvePlaylistSong(snapshot));

    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings, &moved, &playlists);
    window.setShowErrorDialogs(false);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    PlaylistView* view = window.playlistView();
    QCOMPARE(view->itemList()->count(), 1);
    QVERIFY(view->itemList()->item(0)->text().contains(
        QStringLiteral("Snapshot Different Song")));
    view->itemList()->setCurrentRow(0);
    QTest::mouseClick(view->playButton(), Qt::LeftButton);
    QCOMPARE(player.state(), KaraokePlayer::State::Empty);
    QVERIFY(!window.playlistPlayback()->context());
    QCOMPARE(window.statusText(), QStringLiteral("This song can't be found right now."));
}

void TestPlaylistView::resolvedPlaybackIgnoresSongIdCacheWriteFailure()
{
    QTemporaryDir temporary;
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")));
    const LibrarySongs songs = prepareLibrary(temporary, controller);
    PlaylistStore playlists(temporary.filePath(QStringLiteral("app/playlists.sqlite")));
    QVERIFY(playlists.open(nullptr, controller.libraryRoots()));
    qint64 playlistId = 0;
    QVERIFY(playlists.createPlaylist(QStringLiteral("Read Only Cache"), &playlistId));
    const qint64 itemId = addSong(playlists, controller, playlistId, songs.secondId);
    QVERIFY(PlaylistStoreTestAccess::setSongId(playlists, itemId, 999999));
    QVERIFY(PlaylistStoreTestAccess::makeReadOnly(playlists));

    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings, &controller, &playlists);
    window.setShowErrorDialogs(false);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    PlaylistView* view = window.playlistView();
    QCOMPARE(playlists.item(itemId)->songId, 999999LL);
    view->itemList()->setCurrentRow(0);
    QTest::mouseClick(view->playButton(), Qt::LeftButton);
    QCOMPARE(player.state(), KaraokePlayer::State::Playing);
    QVERIFY(sameFile(player.song().mp3Path, songs.secondMp3));
    QCOMPARE(window.playlistPlayback()->context()->itemId, itemId);
    QCOMPARE(playlists.item(itemId)->songId, 999999LL);
    player.stop();
}

void TestPlaylistView::unresolvedAutoplayStopsWithVisibleFeedback()
{
    QTemporaryDir temporary;
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")));
    const LibrarySongs songs = prepareLibrary(temporary, controller);
    PlaylistStore playlists(temporary.filePath(QStringLiteral("app/playlists.sqlite")));
    QVERIFY(playlists.open(nullptr, controller.libraryRoots()));
    QVERIFY(playlists.setAutoplay(true));
    qint64 playlistId = 0;
    QVERIFY(playlists.createPlaylist(QStringLiteral("Missing"), &playlistId));
    const qint64 firstItem = addSong(playlists, controller, playlistId, songs.firstId);
    const SongRef missing{songs.secondId, QStringLiteral("Snapshot Missing Song"),
                          QStringLiteral("Snapshot Singer"), {}, 0,
                          controller.activeRoot().path,
                          QStringLiteral("missing/Snapshot Missing Song.mp3")};
    qint64 missingItem = 0;
    QVERIFY(playlists.addItem(playlistId, missing, &missingItem));
    addSong(playlists, controller, playlistId, songs.thirdId);

    BusTestPlayer player;
    QStringList playedPaths;
    connect(&player, &KaraokePlayer::stateChanged, this,
            [&](KaraokePlayer::State state) {
        if (state == KaraokePlayer::State::Playing)
            playedPaths.append(player.song().mp3Path);
    });
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings, &controller, &playlists);
    window.setShowErrorDialogs(false);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    PlaylistView* view = window.playlistView();
    QCOMPARE(view->itemList()->count(), 3);
    QVERIFY(view->itemList()->item(1)->text().contains(QStringLiteral("Snapshot Missing Song")));
    QVERIFY(!view->itemList()->item(1)->text().contains(QStringLiteral("Second Song")));

    view->itemList()->setCurrentRow(0);
    QTest::mouseClick(view->playButton(), Qt::LeftButton);
    QCOMPARE(window.playlistPlayback()->context()->itemId, firstItem);
    QTRY_COMPARE_WITH_TIMEOUT(player.state(), KaraokePlayer::State::Finished, 4000);
    QTRY_COMPARE(window.statusText(), QStringLiteral("This song can't be found right now."));
    QVERIFY(!window.lyricsVisible());
    QVERIFY(window.libraryVisible());
    QVERIFY(window.playlistPlayback()->context());
    QCOMPARE(window.playlistPlayback()->context()->itemId, firstItem);
    QVERIFY(sameFile(player.song().mp3Path, songs.firstMp3));
    QCOMPARE(playlists.item(missingItem)->songId, songs.secondId);
    for (const QString& path : std::as_const(playedPaths)) {
        QVERIFY(!sameFile(path, songs.secondMp3));
        QVERIFY(!sameFile(path, songs.thirdMp3));
    }
}

void TestPlaylistView::stopAndBusErrorNeverAutoplayWithSuccessors()
{
    QTemporaryDir temporary;
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")));
    const LibrarySongs songs = prepareLibrary(temporary, controller);
    PlaylistStore playlists(temporary.filePath(QStringLiteral("app/playlists.sqlite")));
    QVERIFY(playlists.open(nullptr, controller.libraryRoots()));
    QVERIFY(playlists.setAutoplay(true));
    qint64 playlistId = 0;
    QVERIFY(playlists.createPlaylist(QStringLiteral("Three"), &playlistId));
    const qint64 firstItem = addSong(playlists, controller, playlistId, songs.firstId);
    addSong(playlists, controller, playlistId, songs.secondId);
    addSong(playlists, controller, playlistId, songs.thirdId);

    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings, &controller, &playlists);
    window.setShowErrorDialogs(false);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    PlaylistView* view = window.playlistView();
    view->itemList()->setCurrentRow(0);
    QTest::mouseClick(view->playButton(), Qt::LeftButton);
    QCOMPARE(player.state(), KaraokePlayer::State::Playing);
    QVERIFY(window.lyricsVisible());
    QTest::keyClick(&window, Qt::Key_Escape);
    QVERIFY(!window.lyricsVisible());
    QCOMPARE(player.state(), KaraokePlayer::State::Playing);
    QTest::keyClick(&window, Qt::Key_Return);
    QVERIFY(window.lyricsVisible());
    QCOMPARE(player.state(), KaraokePlayer::State::Playing);
    QTest::keyClick(&window, Qt::Key_Escape);
    QTest::mouseClick(window.stopButton(), Qt::LeftButton);
    QCOMPARE(player.state(), KaraokePlayer::State::Stopped);
    const int pastDuration = qMax(700, int(player.durationMs() + 250));
    QTest::qWait(pastDuration);
    QCOMPARE(player.state(), KaraokePlayer::State::Stopped);
    QVERIFY(sameFile(player.song().mp3Path, songs.firstMp3));
    QCOMPARE(window.playlistPlayback()->context()->playlistId, playlistId);
    QCOMPARE(window.playlistPlayback()->context()->itemId, firstItem);

    view->itemList()->setCurrentRow(0);
    QTest::mouseClick(view->playButton(), Qt::LeftButton);
    QCOMPARE(player.state(), KaraokePlayer::State::Playing);
    QVERIFY(player.postError());
    player.tick();
    QCOMPARE(player.state(), KaraokePlayer::State::Error);
    QTest::qWait(pastDuration);
    QCOMPARE(player.state(), KaraokePlayer::State::Error);
    QVERIFY(sameFile(player.song().mp3Path, songs.firstMp3));
    QCOMPARE(window.playlistPlayback()->context()->playlistId, playlistId);
    QCOMPARE(window.playlistPlayback()->context()->itemId, firstItem);
}

void TestPlaylistView::pendingAutoplayCancelledBySynchronousStopAndPlay()
{
    QTemporaryDir temporary;
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")));
    const LibrarySongs songs = prepareLibrary(temporary, controller);
    PlaylistStore playlists(temporary.filePath(QStringLiteral("app/playlists.sqlite")));
    QVERIFY(playlists.open(nullptr, controller.libraryRoots()));
    QVERIFY(playlists.setAutoplay(true));
    qint64 playlistId = 0;
    QVERIFY(playlists.createPlaylist(QStringLiteral("Cancel Pending"), &playlistId));
    const qint64 firstItem = addSong(playlists, controller, playlistId, songs.firstId);
    addSong(playlists, controller, playlistId, songs.secondId);
    addSong(playlists, controller, playlistId, songs.thirdId);

    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings, &controller, &playlists);
    window.setShowErrorDialogs(false);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    PlaylistView* view = window.playlistView();

    bool stoppedAtFinished = false;
    const QMetaObject::Connection stopConnection = connect(
        &player, &KaraokePlayer::stateChanged, this,
        [&](KaraokePlayer::State state) {
            if (state == KaraokePlayer::State::Finished && !stoppedAtFinished) {
                stoppedAtFinished = true;
                // Stop is disabled at Finished, so model a stop arriving from any
                // other path before the queued autoplay request runs.
                player.stop();
            }
        });
    view->itemList()->setCurrentRow(0);
    QTest::mouseClick(view->playButton(), Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(stoppedAtFinished, 4000);
    QTRY_COMPARE(player.state(), KaraokePlayer::State::Stopped);
    QTest::qWait(100);
    QVERIFY(sameFile(player.song().mp3Path, songs.firstMp3));
    QCOMPARE(window.playlistPlayback()->context()->itemId, firstItem);
    disconnect(stopConnection);

    view->itemList()->setCurrentRow(0);
    bool replayedAtFinished = false;
    const QMetaObject::Connection playConnection = connect(
        &player, &KaraokePlayer::stateChanged, this,
        [&](KaraokePlayer::State state) {
            if (state == KaraokePlayer::State::Finished && !replayedAtFinished) {
                replayedAtFinished = true;
                window.playButton()->click();
            }
        });
    QTest::mouseClick(view->playButton(), Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(replayedAtFinished, 4000);
    QTRY_COMPARE(player.state(), KaraokePlayer::State::Playing);
    QTest::qWait(100);
    QVERIFY(sameFile(player.song().mp3Path, songs.firstMp3));
    QCOMPARE(window.playlistPlayback()->context()->itemId, firstItem);
    disconnect(playConnection);
    player.stop();
}

void TestPlaylistView::rejectedLoadsKeepPlayingContextAndAutoplaySuccessor()
{
    QTemporaryDir temporary;
    const QString root = temporary.filePath(QStringLiteral("music"));
    const QString disconnected = temporary.filePath(QStringLiteral("music-disconnected"));
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")));
    const LibrarySongs songs = prepareLibraryAt(root, controller);
    PlaylistStore playlists(temporary.filePath(QStringLiteral("app/playlists.sqlite")));
    QVERIFY(playlists.open(nullptr, controller.libraryRoots()));
    QVERIFY(playlists.setAutoplay(true));
    qint64 playingPlaylist = 0;
    qint64 missingPlaylist = 0;
    QVERIFY(playlists.createPlaylist(QStringLiteral("Playing"), &playingPlaylist));
    QVERIFY(playlists.createPlaylist(QStringLiteral("Missing"), &missingPlaylist));
    const qint64 playingItem = addSong(
        playlists, controller, playingPlaylist, songs.secondId);
    const qint64 successorItem = addSong(
        playlists, controller, playingPlaylist, songs.thirdId);
    const SongRef missing{999999, QStringLiteral("Unresolvable B"),
                          QStringLiteral("Missing Singer"), {}, 0,
                          root, QStringLiteral("missing/Unresolvable B.mp3")};
    QVERIFY(playlists.addItem(missingPlaylist, missing));
    // Present in the catalogue with both files on disk, but its lyrics file is
    // damaged, so KaraokePlayer::load() rejects it and keeps the current song.
    addSong(playlists, controller, missingPlaylist, songs.fourthId);
    {
        QFile damaged(QFileInfo(songs.fourthMp3).path() + QStringLiteral("/")
                      + QFileInfo(songs.fourthMp3).completeBaseName() + QStringLiteral(".cdg"));
        QVERIFY(damaged.open(QIODevice::WriteOnly | QIODevice::Truncate));
        damaged.write(QByteArray(96, char(0)));
    }

    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings, &controller, &playlists);
    window.setShowErrorDialogs(false);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    PlaylistView* view = window.playlistView();
    view->playlistChooser()->setCurrentIndex(0);
    view->itemList()->setCurrentRow(0);
    QTest::mouseClick(view->playButton(), Qt::LeftButton);
    QCOMPARE(player.state(), KaraokePlayer::State::Playing);
    player.pause();
    QCOMPARE(player.state(), KaraokePlayer::State::Paused);
    QCOMPARE(window.playlistPlayback()->context()->itemId, playingItem);

    view->playlistChooser()->setCurrentIndex(1);
    view->itemList()->setCurrentRow(0);
    QTest::mouseClick(view->playButton(), Qt::LeftButton);
    QCOMPARE(player.state(), KaraokePlayer::State::Paused);
    QVERIFY(sameFile(player.song().mp3Path, songs.secondMp3));
    QCOMPARE(window.playlistPlayback()->context()->itemId, playingItem);

    view->itemList()->setCurrentRow(1);
    QTest::mouseClick(view->playButton(), Qt::LeftButton);
    QCOMPARE(player.state(), KaraokePlayer::State::Paused);
    QVERIFY(sameFile(player.song().mp3Path, songs.secondMp3));
    QCOMPARE(window.playlistPlayback()->context()->playlistId, playingPlaylist);
    QCOMPARE(window.playlistPlayback()->context()->itemId, playingItem);

    const QString lonely = temporary.filePath(QStringLiteral("outside/Lonely.mp3"));
    QVERIFY(QDir().mkpath(QFileInfo(lonely).absolutePath()));
    QVERIFY(testmedia::writeMp3(lonely, 500));
    QVERIFY(!window.openSong(lonely));
    QCOMPARE(player.state(), KaraokePlayer::State::Paused);
    QVERIFY(sameFile(player.song().mp3Path, songs.secondMp3));
    QCOMPARE(window.playlistPlayback()->context()->itemId, playingItem);

    LibraryView* library = window.libraryView();
    library->searchBox()->setText(QStringLiteral("First Song"));
    QTRY_COMPARE(library->songResultCount(), 1);
    library->resultsList()->setCurrentIndex(
        library->resultsList()->model()->index(0, 0));
    // The music is gone when Sing is pressed. Windows cannot rename a folder
    // while a song in it is open (the paused one), so there the song itself
    // goes; elsewhere the whole folder does, as when a drive is unplugged.
#ifdef Q_OS_WIN
    const QString gone = songs.firstMp3 + QStringLiteral(".away");
    QVERIFY(QFile::rename(songs.firstMp3, gone));
#else
    QVERIFY(QDir().rename(root, disconnected));
#endif
    QTest::mouseClick(library->singButton(), Qt::LeftButton);
    QCOMPARE(player.state(), KaraokePlayer::State::Paused);
    QCOMPARE(window.playlistPlayback()->context()->itemId, playingItem);
#ifdef Q_OS_WIN
    QVERIFY(QFile::rename(gone, songs.firstMp3));
#else
    QVERIFY(QDir().rename(disconnected, root));
#endif

    player.play();
    QTRY_COMPARE_WITH_TIMEOUT(window.playlistPlayback()->context()->itemId,
                              successorItem, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(player.state(), KaraokePlayer::State::Playing, 2000);
    QVERIFY(sameFile(player.song().mp3Path, songs.thirdMp3));
    player.stop();
}

void TestPlaylistView::autoplayOffLeavesFinishedSongLoaded()
{
    QTemporaryDir temporary;
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")));
    const LibrarySongs songs = prepareLibrary(temporary, controller);
    PlaylistStore playlists(temporary.filePath(QStringLiteral("app/playlists.sqlite")));
    QVERIFY(playlists.open(nullptr, controller.libraryRoots()));
    QVERIFY(playlists.setAutoplay(false));
    qint64 playlistId = 0;
    QVERIFY(playlists.createPlaylist(QStringLiteral("No Autoplay"), &playlistId));
    const qint64 firstItem = addSong(playlists, controller, playlistId, songs.firstId);
    addSong(playlists, controller, playlistId, songs.secondId);
    addSong(playlists, controller, playlistId, songs.thirdId);
    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings, &controller, &playlists);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    window.playlistView()->itemList()->setCurrentRow(0);
    QTest::mouseClick(window.playlistView()->playButton(), Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(player.state(), KaraokePlayer::State::Finished, 4000);
    QVERIFY(sameFile(player.song().mp3Path, songs.firstMp3));
    QCOMPARE(window.playlistPlayback()->context()->itemId, firstItem);
    QVERIFY(!window.lyricsVisible());
    QVERIFY(window.libraryVisible());
}

void TestPlaylistView::reorderedSuccessorUsesCurrentOrder()
{
    QTemporaryDir temporary;
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")));
    const LibrarySongs songs = prepareLibrary(temporary, controller);
    PlaylistStore playlists(temporary.filePath(QStringLiteral("app/playlists.sqlite")));
    QVERIFY(playlists.open(nullptr, controller.libraryRoots()));
    QVERIFY(playlists.setAutoplay(true));
    qint64 playlistId = 0;
    QVERIFY(playlists.createPlaylist(QStringLiteral("A B C D"), &playlistId));
    addSong(playlists, controller, playlistId, songs.firstId);
    const qint64 b = addSong(playlists, controller, playlistId, songs.secondId);
    const qint64 c = addSong(playlists, controller, playlistId, songs.thirdId);
    const qint64 d = addSong(playlists, controller, playlistId, songs.fourthId);
    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings, &controller, &playlists);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    PlaylistView* view = window.playlistView();
    view->itemList()->setCurrentRow(1);
    QTest::mouseClick(view->playButton(), Qt::LeftButton);
    QCOMPARE(window.playlistPlayback()->context()->itemId, b);
    QTest::keyClick(&window, Qt::Key_Escape);
    view->itemList()->setCurrentRow(2);
    view->itemList()->setFocus();
    QTest::keyClick(view->itemList(), Qt::Key_Down);
    QCOMPARE(playlists.item(c)->position, 3);
    QCOMPARE(playlists.item(d)->position, 2);
    QCOMPARE(player.state(), KaraokePlayer::State::Playing);
    QTRY_COMPARE_WITH_TIMEOUT(window.playlistPlayback()->context()->itemId, d, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(player.state(), KaraokePlayer::State::Playing, 2000);
    QVERIFY(sameFile(player.song().mp3Path, songs.fourthMp3));
    player.stop();
}

void TestPlaylistView::movingPlayingItemUsesItsNewSuccessor()
{
    QTemporaryDir temporary;
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")));
    const LibrarySongs songs = prepareLibrary(temporary, controller);
    PlaylistStore playlists(temporary.filePath(QStringLiteral("app/playlists.sqlite")));
    QVERIFY(playlists.open(nullptr, controller.libraryRoots()));
    QVERIFY(playlists.setAutoplay(true));
    qint64 playlistId = 0;
    QVERIFY(playlists.createPlaylist(QStringLiteral("Move Playing"), &playlistId));
    addSong(playlists, controller, playlistId, songs.firstId);
    const qint64 b = addSong(playlists, controller, playlistId, songs.secondId);
    addSong(playlists, controller, playlistId, songs.thirdId);
    const qint64 d = addSong(playlists, controller, playlistId, songs.fourthId);
    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings, &controller, &playlists);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    PlaylistView* view = window.playlistView();
    view->itemList()->setCurrentRow(1);
    QTest::mouseClick(view->playButton(), Qt::LeftButton);
    QCOMPARE(window.playlistPlayback()->context()->itemId, b);
    QTest::keyClick(&window, Qt::Key_Escape);
    view->itemList()->setCurrentRow(1);
    view->itemList()->setFocus();
    QTest::keyClick(view->itemList(), Qt::Key_Down);
    QCOMPARE(playlists.item(b)->position, 2);
    QTRY_COMPARE_WITH_TIMEOUT(window.playlistPlayback()->context()->itemId, d, 5000);
    QVERIFY(sameFile(player.song().mp3Path, songs.fourthMp3));
    player.stop();
}

void TestPlaylistView::displayedPlaylistDoesNotChangePlaybackOrigin()
{
    QTemporaryDir temporary;
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")));
    const LibrarySongs songs = prepareLibrary(temporary, controller);
    PlaylistStore playlists(temporary.filePath(QStringLiteral("app/playlists.sqlite")));
    QVERIFY(playlists.open(nullptr, controller.libraryRoots()));
    QVERIFY(playlists.setAutoplay(true));
    qint64 p1 = 0;
    qint64 p2 = 0;
    QVERIFY(playlists.createPlaylist(QStringLiteral("Origin"), &p1));
    QVERIFY(playlists.createPlaylist(QStringLiteral("Browsed"), &p2));
    addSong(playlists, controller, p1, songs.firstId);
    const qint64 b = addSong(playlists, controller, p1, songs.secondId);
    const qint64 c = addSong(playlists, controller, p1, songs.thirdId);
    addSong(playlists, controller, p2, songs.fourthId);
    addSong(playlists, controller, p2, songs.firstId);
    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings, &controller, &playlists);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    PlaylistView* view = window.playlistView();
    view->playlistChooser()->setCurrentIndex(0);
    view->itemList()->setCurrentRow(1);
    QTest::mouseClick(view->playButton(), Qt::LeftButton);
    QCOMPARE(window.playlistPlayback()->context()->itemId, b);
    QTest::keyClick(&window, Qt::Key_Escape);
    view->playlistChooser()->setCurrentIndex(1);
    QCOMPARE(view->displayedPlaylistId(), p2);
    QTRY_COMPARE_WITH_TIMEOUT(window.playlistPlayback()->context()->itemId, c, 5000);
    QCOMPARE(window.playlistPlayback()->context()->playlistId, p1);
    QCOMPARE(view->displayedPlaylistId(), p2);
    QVERIFY(sameFile(player.song().mp3Path, songs.thirdMp3));
    player.stop();
}

void TestPlaylistView::removedOrDeletedOriginDoesNotAdvance()
{
    QTemporaryDir temporary;
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")));
    const LibrarySongs songs = prepareLibrary(temporary, controller);
    PlaylistStore playlists(temporary.filePath(QStringLiteral("app/playlists.sqlite")));
    QVERIFY(playlists.open(nullptr, controller.libraryRoots()));
    QVERIFY(playlists.setAutoplay(true));
    qint64 removedPlaylist = 0;
    qint64 deletedPlaylist = 0;
    QVERIFY(playlists.createPlaylist(QStringLiteral("Remove Origin"), &removedPlaylist));
    QVERIFY(playlists.createPlaylist(QStringLiteral("Delete Origin"), &deletedPlaylist));
    addSong(playlists, controller, removedPlaylist, songs.firstId);
    const qint64 removedItem = addSong(playlists, controller, removedPlaylist, songs.secondId);
    addSong(playlists, controller, removedPlaylist, songs.thirdId);
    const qint64 deletedItem = addSong(playlists, controller, deletedPlaylist, songs.secondId);
    addSong(playlists, controller, deletedPlaylist, songs.thirdId);
    addSong(playlists, controller, deletedPlaylist, songs.fourthId);
    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings, &controller, &playlists);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    PlaylistView* view = window.playlistView();
    view->playlistChooser()->setCurrentIndex(0);
    view->itemList()->setCurrentRow(1);
    QTest::mouseClick(view->playButton(), Qt::LeftButton);
    QTest::keyClick(&window, Qt::Key_Escape);
    view->itemList()->setCurrentRow(1);
    view->setRemoveConfirmation(
        [](QWidget*, const QString&, const QString&) { return true; });
    QTest::mouseClick(view->removeButton(), Qt::LeftButton);
    QVERIFY(!playlists.item(removedItem));
    QCOMPARE(player.state(), KaraokePlayer::State::Playing);
    QVERIFY(sameFile(player.song().mp3Path, songs.secondMp3));
    QTRY_COMPARE_WITH_TIMEOUT(player.state(), KaraokePlayer::State::Finished, 5000);
    QVERIFY(sameFile(player.song().mp3Path, songs.secondMp3));

    view->playlistChooser()->setCurrentIndex(1);
    view->itemList()->setCurrentRow(0);
    QTest::mouseClick(view->playButton(), Qt::LeftButton);
    QCOMPARE(window.playlistPlayback()->context()->itemId, deletedItem);
    QTest::keyClick(&window, Qt::Key_Escape);
    view->setDeleteConfirmation([](QWidget*, const QString&) { return true; });
    QTest::mouseClick(view->deleteButton(), Qt::LeftButton);
    QVERIFY(!playlists.playlist(deletedPlaylist));
    QCOMPARE(player.state(), KaraokePlayer::State::Playing);
    QVERIFY(sameFile(player.song().mp3Path, songs.secondMp3));
    QTRY_COMPARE_WITH_TIMEOUT(player.state(), KaraokePlayer::State::Finished, 5000);
    QVERIFY(sameFile(player.song().mp3Path, songs.secondMp3));
}

void TestPlaylistView::libraryRefreshPreservesPlaylistPosition()
{
    QTemporaryDir temporary;
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")));
    const LibrarySongs songs = prepareLibrary(temporary, controller);
    PlaylistStore playlists(temporary.filePath(QStringLiteral("app/playlists.sqlite")));
    QVERIFY(playlists.open(nullptr, controller.libraryRoots()));
    qint64 playlistId = 0;
    QVERIFY(playlists.createPlaylist(QStringLiteral("Long"), &playlistId));
    for (int i = 0; i < 24; ++i)
        addSong(playlists, controller, playlistId,
                i % 2 ? songs.firstId : songs.secondId);
    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings, &controller, &playlists);
    window.resize(1000, 600);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    PlaylistView* view = window.playlistView();
    view->itemList()->setCurrentRow(16);
    view->itemList()->scrollToItem(view->itemList()->currentItem(),
                                   QAbstractItemView::PositionAtTop);
    const qint64 selected = view->selectedItemId();
    const int scroll = view->itemList()->verticalScrollBar()->value();
    QVERIFY(scroll > 0);
    QSignalSpy finished(&controller, &LibraryController::scanFinished);
    controller.requestRefreshScan();
    if (finished.count() == 0)
        QVERIFY(finished.wait(5000));
    QTRY_COMPARE(view->selectedItemId(), selected);
    QCOMPARE(view->itemList()->verticalScrollBar()->value(), scroll);
    QCOMPARE(player.state(), KaraokePlayer::State::Empty);
}

void TestPlaylistView::keyboardSelectionAndEscapeKeepPlayback()
{
    QTemporaryDir temporary;
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")));
    const LibrarySongs songs = prepareLibrary(temporary, controller);
    PlaylistStore playlists(temporary.filePath(QStringLiteral("app/playlists.sqlite")));
    QVERIFY(playlists.open(nullptr, controller.libraryRoots()));
    qint64 playlistId = 0;
    QVERIFY(playlists.createPlaylist(QStringLiteral("Keyboard"), &playlistId));
    const qint64 firstItem = addSong(playlists, controller, playlistId, songs.secondId);
    addSong(playlists, controller, playlistId, songs.firstId);

    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings, &controller, &playlists);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    PlaylistView* view = window.playlistView();
    QCOMPARE(view->itemList()->currentRow(), 0);
    QCOMPARE(view->selectedItemId(), firstItem);
    view->itemList()->setFocus();
    QTest::keyClick(view->itemList(), Qt::Key_Down);
    QCOMPARE(view->selectedItemId(), firstItem);
    QCOMPARE(view->itemList()->currentRow(), 1);

    QTest::mouseClick(view->playButton(), Qt::LeftButton);
    QCOMPARE(player.state(), KaraokePlayer::State::Playing);
    QVERIFY(window.lyricsVisible());
    QTest::keyClick(&window, Qt::Key_Escape);
    QVERIFY(window.libraryVisible());
    QVERIFY(!window.lyricsVisible());
    // Escape in the playlist only returns to the search box.
    view->itemList()->setFocus();
    QTest::keyClick(view->itemList(), Qt::Key_Escape);
    QCOMPARE(window.focusWidget(), window.libraryView()->searchBox());
    QVERIFY(window.libraryVisible());
    QVERIFY(!window.lyricsVisible());
    QCOMPARE(player.state(), KaraokePlayer::State::Playing);
    QVERIFY(sameFile(player.song().mp3Path, songs.secondMp3));
    player.stop();
}

void TestPlaylistView::homeScreenEnterNeverChangesTheSong()
{
    QTemporaryDir temporary;
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")));
    const LibrarySongs songs = prepareLibrary(temporary, controller);
    PlaylistStore playlists(temporary.filePath(QStringLiteral("app/playlists.sqlite")));
    QVERIFY(playlists.open(nullptr, controller.libraryRoots()));
    qint64 playlistId = 0;
    QVERIFY(playlists.createPlaylist(QStringLiteral("Enter"), &playlistId));
    addSong(playlists, controller, playlistId, songs.secondId);
    addSong(playlists, controller, playlistId, songs.firstId);

    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings, &controller, &playlists);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    PlaylistView* view = window.playlistView();
    LibraryView* library = window.libraryView();
    QVERIFY(!window.lyricsButton()->isEnabled());

    // Idle, nothing searched and nothing chosen: Enter does not pick a song.
    QVERIFY(library->searchBox()->text().isEmpty());
    QCOMPARE(library->selectedSongId(), 0LL);
    QTest::keyClick(library->searchBox(), Qt::Key_Return);
    QCOMPARE(player.state(), KaraokePlayer::State::Empty);
    QVERIFY(!window.lyricsVisible());

    view->itemList()->setCurrentRow(0);
    QTest::mouseClick(view->playButton(), Qt::LeftButton);
    QCOMPARE(player.state(), KaraokePlayer::State::Playing);
    QTest::keyClick(&window, Qt::Key_Escape);
    // While playing: Enter in the search box with another song chosen.
    library->resultsList()->setCurrentIndex(library->resultsList()->model()->index(0, 0));
    QTest::keyClick(library->searchBox(), Qt::Key_Return);
    QVERIFY(window.lyricsVisible());
    QVERIFY(sameFile(player.song().mp3Path, songs.secondMp3));
    QCOMPARE(player.state(), KaraokePlayer::State::Playing);
    QTest::keyClick(&window, Qt::Key_Escape);
    library->resultsList()->setCurrentIndex({});
    QTest::mouseClick(window.pauseButton(), Qt::LeftButton);
    QCOMPARE(player.state(), KaraokePlayer::State::Paused);
    QVERIFY(window.libraryVisible());
    QVERIFY(window.lyricsButton()->isEnabled());
    QSignalSpy states(&player, &KaraokePlayer::stateChanged);

    // A different library song is chosen: Enter returns to the lyrics only.
    library->resultsList()->setCurrentIndex(library->resultsList()->model()->index(0, 0));
    QVERIFY(library->selectedSongId() != 0);
    QTest::keyClick(library->searchBox(), Qt::Key_Return);
    QVERIFY(window.lyricsVisible());
    QVERIFY(sameFile(player.song().mp3Path, songs.secondMp3));

    // The same from the playlist, with another item selected.
    QTest::keyClick(&window, Qt::Key_Escape);
    view->itemList()->setCurrentRow(1);
    view->itemList()->setFocus();
    QTest::keyClick(view->itemList(), Qt::Key_Enter);
    QVERIFY(window.lyricsVisible());
    QVERIFY(sameFile(player.song().mp3Path, songs.secondMp3));

    // The Lyrics button does the same.
    QTest::keyClick(&window, Qt::Key_Escape);
    QTest::mouseClick(window.lyricsButton(), Qt::LeftButton);
    QVERIFY(window.lyricsVisible());
    QCOMPARE(states.count(), 0);
    QCOMPARE(player.state(), KaraokePlayer::State::Paused);
    player.stop();
    QVERIFY(!window.lyricsButton()->isEnabled());
}

void TestPlaylistView::presentationFollowsPaneInUseAndPlayback()
{
    QTemporaryDir temporary;
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")), {},
                                 temporary.filePath(QStringLiteral("app/overrides.sqlite")),
                                 nullptr, {},
                                 temporary.filePath(QStringLiteral("app/user-state.sqlite")));
    const LibrarySongs songs = prepareLibrary(temporary, controller);
    PlaylistStore playlists(temporary.filePath(QStringLiteral("app/playlists.sqlite")));
    QVERIFY(playlists.open(nullptr, controller.libraryRoots()));
    qint64 firstPlaylist = 0;
    qint64 secondPlaylist = 0;
    QVERIFY(playlists.createPlaylist(QStringLiteral("First"), &firstPlaylist));
    QVERIFY(playlists.createPlaylist(QStringLiteral("Second"), &secondPlaylist));
    addSong(playlists, controller, firstPlaylist, songs.secondId);
    addSong(playlists, controller, firstPlaylist, songs.firstId);
    addSong(playlists, controller, secondPlaylist, songs.thirdId);
    QVERIFY(playlists.setLastPlaylistId(firstPlaylist));

    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings, &controller, &playlists);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    PlaylistView* view = window.playlistView();
    LibraryView* library = window.libraryView();
    QTreeView* results = library->resultsList();

    // Tabs show the playlists and switch between them.
    QCOMPARE(view->playlistTabs()->count(), 2);
    QCOMPARE(view->playlistTabs()->currentIndex(), view->playlistChooser()->currentIndex());
    QCOMPARE(view->displayedPlaylistId(), firstPlaylist);
    view->playlistTabs()->setCurrentIndex(1);
    QCOMPARE(view->displayedPlaylistId(), secondPlaylist);
    QCOMPARE(view->itemList()->count(), 1);
    view->playlistTabs()->setCurrentIndex(0);
    QCOMPARE(view->displayedPlaylistId(), firstPlaylist);
    QVERIFY(library->isActive());
    // Clicking a tab puts the playlist in use, without taking the keyboard.
    QTest::mouseClick(view->playlistTabs(), Qt::LeftButton, Qt::NoModifier,
                      view->playlistTabs()->tabRect(1).center());
    QCOMPARE(view->displayedPlaylistId(), secondPlaylist);
    QVERIFY(view->isActive());
    QVERIFY(!library->isActive());
    QTest::mouseClick(view->playlistTabs(), Qt::LeftButton, Qt::NoModifier,
                      view->playlistTabs()->tabRect(0).center());
    QCOMPARE(view->displayedPlaylistId(), firstPlaylist);
    setLibraryInUse(library);

    // Only the pane in use shows its selection strongly; both keep it.
    QVERIFY(library->isActive());
    QVERIFY(!view->isActive());
    QVERIFY(!view->isActive());
    QTest::mouseClick(view->itemList()->viewport(), Qt::LeftButton, Qt::NoModifier,
                      view->itemList()->visualItemRect(view->itemList()->item(1)).center());
    QTRY_VERIFY(view->isActive());
    QVERIFY(!library->isActive());
    const QModelIndex libraryRow = results->model()->index(0, 0);
    QTest::mouseClick(results->viewport(), Qt::LeftButton, Qt::NoModifier,
                      results->visualRect(libraryRow).center());
    QTRY_VERIFY(library->isActive());
    QVERIFY(!view->isActive());
    QCOMPARE(window.focusWidget(), library->searchBox());
    QCOMPARE(view->itemList()->currentRow(), 1);
    QCOMPARE(results->currentIndex().row(), 0);

    // Play and Pause share one place; the song being sung is marked in the
    // library separately from the selection.
    QVERIFY(window.playButton()->isVisible());
    QVERIFY(!window.pauseButton()->isVisible());
    view->itemList()->setCurrentRow(0);
    QTest::mouseClick(view->playButton(), Qt::LeftButton);
    QCOMPARE(player.state(), KaraokePlayer::State::Playing);
    QTest::keyClick(&window, Qt::Key_Escape);
    QVERIFY(window.pauseButton()->isVisible());
    QVERIFY(!window.playButton()->isVisible());
    const int playingRow = libraryRowForSongId(library, songs.secondId);
    QVERIFY(playingRow >= 0);
    QVERIFY(results->model()->index(playingRow, 0).data(LibraryResultsModel::PlayingRole).toBool());
    const int otherRow = libraryRowForSongId(library, songs.firstId);
    QVERIFY(!results->model()->index(otherRow, 0).data(LibraryResultsModel::PlayingRole).toBool());
    // The play is counted once the song is heard; its row shows it.
    const QModelIndex plays = results->model()->index(playingRow, LibraryResultsModel::PlaysColumn);
    QTRY_COMPARE(plays.data(LibraryResultsModel::PlaysRole).toInt(), 1);
    QTest::mouseClick(window.pauseButton(), Qt::LeftButton);
    QCOMPARE(player.state(), KaraokePlayer::State::Paused);
    QCOMPARE(plays.data().toString(), QStringLiteral("1"));
    QVERIFY(window.playButton()->isVisible());
    QCOMPARE(window.playButton()->text(), QStringLiteral("Resume"));
    QVERIFY(results->model()->index(playingRow, 0).data(LibraryResultsModel::PlayingRole).toBool());
    // Now Playing follows a name correction for the loaded song.
    QVERIFY(controller.setManualOverride(songs.secondId, QStringLiteral("Fixed Singer"),
                                         QStringLiteral("Fixed Title")));
    QCOMPARE(window.songText(), QStringLiteral("Fixed Singer \u2013 Fixed Title"));
    const int stillPlaying = libraryRowForSongId(library, songs.secondId);
    QVERIFY(results->model()->index(stillPlaying, 0).data(LibraryResultsModel::PlayingRole).toBool());
    player.stop();
    QVERIFY(!results->model()->index(stillPlaying, 0).data(LibraryResultsModel::PlayingRole).toBool());
}

// The colour painted in the middle of a row's padding, clear of any text.
QColor rowColour(QAbstractItemView* view, const QRect& row, int x)
{
    const QImage image = view->viewport()->grab().toImage();
    const qreal scale = image.devicePixelRatio();
    return image.pixelColor(QPoint(int(x * scale), int(row.center().y() * scale)));
}

void TestPlaylistView::onlyThePaneInUsePaintsItsSelectionGold()
{
    QTemporaryDir temporary;
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")));
    const LibrarySongs songs = prepareLibrary(temporary, controller);
    PlaylistStore playlists(temporary.filePath(QStringLiteral("app/playlists.sqlite")));
    QVERIFY(playlists.open(nullptr, controller.libraryRoots()));
    qint64 playlistId = 0;
    QVERIFY(playlists.createPlaylist(QStringLiteral("Colours"), &playlistId));
    addSong(playlists, controller, playlistId, songs.secondId);
    addSong(playlists, controller, playlistId, songs.firstId);

    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings, &controller, &playlists);
    window.resize(1200, 800);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    LibraryView* library = window.libraryView();
    PlaylistView* view = window.playlistView();
    QTreeView* results = library->resultsList();
    QListWidget* items = view->itemList();

    const auto libraryRow = [&] { return results->visualRect(results->currentIndex()); };
    const auto playlistRow = [&] { return items->visualItemRect(items->currentItem()); };
    const auto libraryColour = [&] {
        return rowColour(results, libraryRow(), results->viewport()->width() - 6);
    };
    const auto playlistColour = [&] { return rowColour(items, playlistRow(), 6); };
    const auto expectPaint = [&](bool libraryGold) {
        // The gold pane is the one the keyboard acts on.
        QCOMPARE(window.focusWidget(), libraryGold ? static_cast<QWidget*>(library->searchBox())
                                                   : static_cast<QWidget*>(items));
        QCOMPARE(libraryColour(), libraryGold ? theme::color::selection
                                              : theme::color::selectionIdle);
        QCOMPARE(playlistColour(), libraryGold ? theme::color::selectionIdle
                                               : theme::color::selection);
    };

    // Click a library song, then a playlist song, then the library again.
    const QModelIndex song = results->model()->index(1, 0);
    QTest::mouseClick(results->viewport(), Qt::LeftButton, Qt::NoModifier,
                      results->visualRect(song).center());
    QTest::mouseClick(items->viewport(), Qt::LeftButton, Qt::NoModifier,
                      items->visualItemRect(items->item(1)).center());
    QCOMPARE(results->currentIndex().row(), 1);
    QCOMPARE(items->currentRow(), 1);
    expectPaint(false);
    QTest::mouseClick(results->viewport(), Qt::LeftButton, Qt::NoModifier,
                      results->visualRect(song).center());
    expectPaint(true);

    // Playing is a separate mark; stopping removes it and keeps both selections.
    items->setCurrentRow(0);
    QTest::mouseClick(view->playButton(), Qt::LeftButton);
    QCOMPARE(player.state(), KaraokePlayer::State::Playing);
    QTest::keyClick(&window, Qt::Key_Escape);
    QVERIFY(items->item(0)->text().startsWith(QStringLiteral("▶ ")));
    items->setCurrentRow(1);
    player.stop();
    QVERIFY(!items->item(0)->text().startsWith(QStringLiteral("▶ ")));
    QCOMPARE(items->currentRow(), 1);
    QCOMPARE(results->currentIndex().row(), 1);
    expectPaint(false);  // the Play button was used: the playlist is in use

    // Moving the keyboard between the panes does the same.
    window.activateWindow();
    if (!QTest::qWaitForWindowActive(&window))
        QSKIP("This platform cannot activate the test window for keyboard focus.");
    library->searchBox()->setFocus(Qt::TabFocusReason);
    QTRY_VERIFY(library->isActive());
    expectPaint(true);
    items->setFocus(Qt::TabFocusReason);
    QTRY_VERIFY(view->isActive());
    expectPaint(false);
}

void TestPlaylistView::panesStayReadableOnALaptopScreenAtEveryScale()
{
    QTemporaryDir temporary;
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")));
    prepareLibrary(temporary, controller);
    PlaylistStore playlists(temporary.filePath(QStringLiteral("app/playlists.sqlite")));
    QVERIFY(playlists.open(nullptr, controller.libraryRoots()));
    const QString longName = QStringLiteral("Saturday Sing-Along With The Whole Family");
    for (const QString& name : {QStringLiteral("Friday Night"), QStringLiteral("Country"),
                                QStringLiteral("Favourites"), longName, QStringLiteral("Christmas")})
        QVERIFY(playlists.createPlaylist(name));

    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings, &controller, &playlists);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* tabs = window.playlistView()->findChild<QTabBar*>(QStringLiteral("playlistTabs"));
    QVERIFY(tabs);
    QCOMPARE(tabs->count(), 5);
    QTreeView* results = window.libraryView()->resultsList();
    QHeaderView* header = results->header();

    // Where a widget with this name sits in the window.
    const auto placeIn = [&window](QWidget* pane, const QString& name) {
        QWidget* widget = pane->findChild<QWidget*>(name);
        return widget ? QRect(widget->mapTo(&window, QPoint(0, 0)), widget->size()) : QRect();
    };
    for (const int percent : {80, 100, 110, 125, 150}) {
        theme::setScalePercent(percent);
        const QString at = QStringLiteral("at %1%").arg(percent);
        // A 1366x768 laptop, with room left for the taskbar and title bar.
        QVERIFY2(window.minimumSizeHint().width() <= 1366, qPrintable(at));
        QVERIFY2(window.minimumSizeHint().height() <= 705, qPrintable(at + QStringLiteral(" %1").arg(window.minimumSizeHint().height())));
        window.resize(1366, 705);
        QCoreApplication::processEvents();

        // Short playlist names are never cut; a long one is cut, not the others.
        const QFontMetrics metrics(tabs->font());
        for (int tab = 0; tab < tabs->count(); ++tab) {
            const QString name = tabs->tabText(tab);
            if (name == longName) {
                QVERIFY2(tabs->tabRect(tab).width() <= theme::px(200), qPrintable(at));
                QCOMPARE(tabs->tabToolTip(tab), longName);
            } else {
                QVERIFY2(tabs->tabRect(tab).width() >= metrics.horizontalAdvance(name) + theme::px(20),
                         qPrintable(QStringLiteral("%1 %2: tab %3 wide, text %4, font %5 %6px, bar %7 wide")
                                        .arg(at, name).arg(tabs->tabRect(tab).width())
                                        .arg(metrics.horizontalAdvance(name))
                                        .arg(tabs->font().family()).arg(tabs->font().pixelSize())
                                        .arg(tabs->width())));
            }
        }

        // The two panes' titles and bottom buttons line up across the window.
        LibraryView* library = window.libraryView();
        PlaylistView* list = window.playlistView();
        QVERIFY2(qAbs(placeIn(library, QStringLiteral("paneTitle")).center().y()
                      - placeIn(list, QStringLiteral("paneTitle")).center().y()) <= 1, qPrintable(at));
        QCOMPARE(placeIn(library, QStringLiteral("paneFooter")).bottom(),
                 placeIn(list, QStringLiteral("paneFooter")).bottom());

        // Artist and Song keep the most room; Label and Disc ID stay usable.
        const int artist = header->sectionSize(LibraryResultsModel::ArtistColumn);
        const int song = header->sectionSize(LibraryResultsModel::SongColumn);
        const int label = header->sectionSize(LibraryResultsModel::LabelColumn);
        const int disc = header->sectionSize(LibraryResultsModel::DiscColumn);
        QVERIFY2(artist >= label && song >= label, qPrintable(at));
        QVERIFY2(disc >= theme::px(104) && label >= theme::px(100), qPrintable(at));

        // The scroll bar starts under the column captions, not beside them.
        auto* corner = results->findChild<QFrame*>(QStringLiteral("headerCorner"));
        QVERIFY(corner);
        QCOMPARE(corner->maximumHeight(), header->height());
    }
    theme::setScalePercent(100);
    // In a wide window the Label column has room for a whole label name.
    window.resize(1920, 1080);
    QCoreApplication::processEvents();
    QVERIFY(header->sectionSize(LibraryResultsModel::LabelColumn)
            >= QFontMetrics(results->font()).horizontalAdvance(QStringLiteral("Essential Karaoke")) + 24);
}

void TestPlaylistView::playbackMarkerTracksActualState()
{
    QTemporaryDir temporary;
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")));
    const LibrarySongs songs = prepareLibrary(temporary, controller);
    PlaylistStore playlists(temporary.filePath(QStringLiteral("app/playlists.sqlite")));
    QVERIFY(playlists.open(nullptr, controller.libraryRoots()));
    QVERIFY(playlists.setAutoplay(false));
    qint64 playlistId = 0;
    QVERIFY(playlists.createPlaylist(QStringLiteral("Marker"), &playlistId));
    addSong(playlists, controller, playlistId, songs.firstId);

    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings, &controller, &playlists);
    window.setShowErrorDialogs(false);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    PlaylistView* view = window.playlistView();
    QTest::mouseClick(view->playButton(), Qt::LeftButton);
    QCOMPARE(player.state(), KaraokePlayer::State::Playing);
    QVERIFY(view->itemList()->item(0)->text().startsWith(QStringLiteral("▶ ")));
    player.pause();
    QCOMPARE(player.state(), KaraokePlayer::State::Paused);
    QVERIFY(view->itemList()->item(0)->text().startsWith(QStringLiteral("▶ ")));
    window.stopButton()->click();
    QCOMPARE(player.state(), KaraokePlayer::State::Stopped);
    QVERIFY(!view->itemList()->item(0)->text().startsWith(QStringLiteral("▶ ")));

    window.playButton()->click();
    QTRY_COMPARE_WITH_TIMEOUT(player.state(), KaraokePlayer::State::Finished, 4000);
    QVERIFY(!view->itemList()->item(0)->text().startsWith(QStringLiteral("▶ ")));

    window.playButton()->click();
    QCOMPARE(player.state(), KaraokePlayer::State::Playing);
    QVERIFY(view->itemList()->item(0)->text().startsWith(QStringLiteral("▶ ")));
    QVERIFY(player.postError());
    player.tick();
    QCOMPARE(player.state(), KaraokePlayer::State::Error);
    QVERIFY(!view->itemList()->item(0)->text().startsWith(QStringLiteral("▶ ")));
}

void TestPlaylistView::movingPastViewportKeepsSelectionFullyVisible()
{
    QTemporaryDir temporary;
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")));
    const LibrarySongs songs = prepareLibrary(temporary, controller);
    PlaylistStore playlists(temporary.filePath(QStringLiteral("app/playlists.sqlite")));
    QVERIFY(playlists.open(nullptr, controller.libraryRoots()));
    qint64 playlistId = 0;
    QVERIFY(playlists.createPlaylist(QStringLiteral("Long Moves"), &playlistId));
    for (int i = 0; i < 30; ++i)
        addSong(playlists, controller, playlistId,
                i % 2 ? songs.firstId : songs.secondId);

    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings, &controller, &playlists);
    window.resize(900, 420);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    PlaylistView* view = window.playlistView();
    QListWidget* list = view->itemList();
    QCoreApplication::processEvents();

    int lastFullyVisible = -1;
    for (int row = 0; row < list->count(); ++row) {
        if (list->viewport()->rect().contains(list->visualItemRect(list->item(row))))
            lastFullyVisible = row;
        else
            break;
    }
    QVERIFY(lastFullyVisible >= 1);
    QVERIFY(lastFullyVisible + 5 < list->count());
    list->setCurrentRow(lastFullyVisible);
    const qint64 movedItem = view->selectedItemId();
    list->setFocus();
    for (int i = 0; i < 5; ++i) {
        QTest::keyClick(list, Qt::Key_Down);
        QCOMPARE(view->selectedItemId(), movedItem);
        QVERIFY(list->viewport()->rect().contains(
            list->visualItemRect(list->currentItem())));
    }

    list->scrollToItem(list->currentItem(), QAbstractItemView::PositionAtTop);
    QCoreApplication::processEvents();
    for (int i = 0; i < 5; ++i) {
        QTest::keyClick(list, Qt::Key_Up);
        QCOMPARE(view->selectedItemId(), movedItem);
        QVERIFY(list->viewport()->rect().contains(
            list->visualItemRect(list->currentItem())));
    }
}

void TestPlaylistView::unavailableStoreLeavesLibraryUsable()
{
    QTemporaryDir temporary;
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")));
    prepareLibrary(temporary, controller);
    PlaylistStore unavailable(temporary.filePath(QStringLiteral("not-open.sqlite")));
    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings, &controller, &unavailable);
    window.setShowErrorDialogs(false);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.playlistView()->messageLabel()->text().contains(
        QStringLiteral("unavailable")));
    QVERIFY(!window.playlistView()->newButton()->isEnabled());
    window.libraryView()->searchBox()->setText(QStringLiteral("First Song"));
    QTRY_COMPARE(window.libraryView()->songResultCount(), 1);
    window.libraryView()->resultsList()->setCurrentIndex(
        window.libraryView()->resultsList()->model()->index(0, 0));
    QTest::mouseClick(window.libraryView()->singButton(), Qt::LeftButton);
    QCOMPARE(player.state(), KaraokePlayer::State::Playing);
    player.stop();
}

QTEST_MAIN(TestPlaylistView)
#include "tst_playlistview.moc"
