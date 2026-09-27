#include "BusTestPlayer.h"
#include "LibraryController.h"
#include "LibraryView.h"
#include "MainWindow.h"
#include "MetadataReviewDialog.h"
#include "PlaylistView.h"
#include "SongSettings.h"
#include "TestMedia.h"
#include "library/Catalogue.h"
#include "library/ContentIdentity.h"
#include "library/LibraryScanner.h"
#include "library/UserStateStore.h"
#include "playlist/PlaylistPlayback.h"
#include "playlist/PlaylistStore.h"

#include <QComboBox>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QPushButton>
#include <QScrollBar>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QtTest>

namespace {

// Songs used by every test: two versions of one song on different labels,
// a label-less disc and an unresolved song with no artist.
const QStringList kSongs = {
    QStringLiteral("Sunfly/SF001-01 - Johnny Cash - Ring of Fire"),
    QStringLiteral("Legends/LEG001-01 - Johnny Cash - Ring of Fire"),
    QStringLiteral("Sunfly/SF001-02 - Abba - Waterloo"),
    QStringLiteral("Legends/LEG001-02 - Zz Top - La Grange"),
    QStringLiteral("Other/XYZ001-01 - Beatles - Help"),
    QStringLiteral("SGB39/3902"),
};

void writeSongs(const QString& root, bool playable)
{
    int index = 0;
    for (const QString& stem : kSongs) {
        const QString base = root + QLatin1Char('/') + stem;
        QDir().mkpath(QFileInfo(base).absolutePath());
        // Every song, including the two versions of one title, has its own content.
        const int durationMs = 450 + 40 * index++;
        const bool ok = playable
            ? testmedia::writeMp3(base + QStringLiteral(".mp3"), durationMs)
                && testmedia::writeCdg(base + QStringLiteral(".cdg"),
                                       testmedia::markerCdg(durationMs, 150 + 10 * index))
            : testmedia::writeFile(base + QStringLiteral(".mp3"), stem.toUtf8() + QByteArray(2000, 'a'))
                && testmedia::writeFile(base + QStringLiteral(".cdg"), stem.toUtf8() + QByteArray(2400, 'b'));
        if (!ok)
            qFatal("Could not write synthetic song");
    }
}

void scan(LibraryController& controller, const QString& root)
{
    QSignalSpy finished(&controller, &LibraryController::scanFinished);
    if (!controller.chooseRoot(root))
        qFatal("Could not choose synthetic root");
    if (finished.count() == 0 && !finished.wait(20000))
        qFatal("Synthetic scan did not finish");
    QTRY_VERIFY_WITH_TIMEOUT(!controller.isScanning(), 20000);
}

QString describe(const CatalogueSearchRow& row)
{
    return QStringLiteral("%1|%2|%3").arg(row.displayArtist, row.displayTitle, row.label);
}

QStringList describe(const QList<CatalogueSearchRow>& rows)
{
    QStringList result;
    for (const CatalogueSearchRow& row : rows)
        result.append(describe(row));
    return result;
}

QStringList viewRows(LibraryView* view)
{
    QStringList result;
    const QAbstractItemModel* model = view->resultsList()->model();
    for (int row = 0; row < view->songResultCount(); ++row)
        result.append(model->index(row, 0).data(Qt::DisplayRole).toString());
    return result;
}

qint64 songId(LibraryController& controller, const QString& label, const QString& title)
{
    for (const CatalogueSearchRow& row : controller.browse()) {
        if (row.label == label && row.displayTitle == title)
            return row.songId;
    }
    qFatal("Synthetic song not found");
    return 0;
}

QString userState(const QTemporaryDir& temporary)
{
    return temporary.filePath(QStringLiteral("app/user-state.sqlite"));
}

// Deletes the rebuildable catalogue entirely, as a recovery or reinstall would.
void deleteCatalogue(const QString& path)
{
    for (const QString& suffix : {QString(), QStringLiteral("-wal"), QStringLiteral("-shm")})
        QFile::remove(path + suffix);
    QVERIFY(!QFileInfo::exists(path));
}

QString identityOf(const QString& root, const QString& stem)
{
    const QString base = root + QLatin1Char('/') + stem;
    return contentIdentity(base + QStringLiteral(".mp3"), base + QStringLiteral(".cdg"));
}

const QString kRingSunfly = QStringLiteral("Johnny Cash|Ring of Fire|Sunfly");
const QString kRingLegends = QStringLiteral("Johnny Cash|Ring of Fire|Legends");
const QString kWaterloo = QStringLiteral("Abba|Waterloo|Sunfly");
const QString kGrange = QStringLiteral("Zz Top|La Grange|Legends");
const QString kHelp = QStringLiteral("Beatles|Help|");

} // namespace

class TestLibrarySort : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void sortOrdersAndPlayStatistics();
    void sortIsRememberedAndAppliesToSearch();
    void playsCountOncePerSungStart();
    void existingVersionFiveCatalogueGainsPlayStatistics();
    void writesNeverWaitForABusyCatalogue();
    void playHistorySurvivesRebuildMoveAndCorrection();
    void userStateKeepsLocationsAndIsNeverErased();
    void movedSongSearchReadsEachCandidateOnce();
    void sortChangeStartsAtTopWithoutSelection();
};

void TestLibrarySort::initTestCase()
{
    QString error;
    QVERIFY2(KaraokePlayer::initializeGStreamer(&error), qPrintable(error));
}

void TestLibrarySort::sortOrdersAndPlayStatistics()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("music"));
    const QString path = temporary.filePath(QStringLiteral("app/library.sqlite"));
    writeSongs(root, false);
    {
        LibraryController controller(path);
        scan(controller, root);
    }
    Catalogue catalogue(path);
    QVERIFY(catalogue.open());
    const QStringList byArtist = describe(catalogue.browseActive());
    QCOMPARE(byArtist.size(), 6);
    const QString unresolved = byArtist.last();
    QVERIFY2(unresolved.startsWith(QLatin1Char('|')), qPrintable(unresolved));  // no artist: last

    // Default: artist, then title, then label as the tie-breaker.
    QCOMPARE(byArtist, (QStringList{kWaterloo, kHelp, kRingLegends, kRingSunfly, kGrange, unresolved}));
    QCOMPARE(describe(catalogue.browseActive(nullptr, LibrarySort::ArtistDesc)),
             (QStringList{kGrange, kRingLegends, kRingSunfly, kHelp, kWaterloo, unresolved}));
    QCOMPARE(describe(catalogue.browseActive(nullptr, LibrarySort::TitleAsc)),
             (QStringList{unresolved, kHelp, kGrange, kRingLegends, kRingSunfly, kWaterloo}));
    QCOMPARE(describe(catalogue.browseActive(nullptr, LibrarySort::TitleDesc)),
             (QStringList{kWaterloo, kRingLegends, kRingSunfly, kGrange, kHelp, unresolved}));
    // Label, then artist, then title; songs without a label come last.
    const QStringList byLabel = describe(catalogue.browseActive(nullptr, LibrarySort::LabelAsc));
    QCOMPARE(byLabel.mid(0, 4), (QStringList{kRingLegends, kGrange, kWaterloo, kRingSunfly}));
    QCOMPARE(byLabel.last(), kHelp);

    // Nothing played yet: the play orders fall back to Artist A-Z.
    QCOMPARE(describe(catalogue.browseActive(nullptr, LibrarySort::MostPlayed)), byArtist);
    QCOMPARE(describe(catalogue.browseActive(nullptr, LibrarySort::RecentlyPlayed)), byArtist);

    auto idOf = [&](const QString& description) {
        for (const CatalogueSearchRow& row : catalogue.browseActive()) {
            if (describe(row) == description)
                return row.songId;
        }
        return qint64(0);
    };
    const qint64 waterloo = idOf(kWaterloo);
    const qint64 ringSunfly = idOf(kRingSunfly);
    const qint64 ringLegends = idOf(kRingLegends);
    const qint64 help = idOf(kHelp);
    QVERIFY(catalogue.setPlayStats(waterloo, {3, 3000}));
    QVERIFY(catalogue.setPlayStats(help, {1, 2000}));
    QVERIFY(catalogue.setPlayStats(ringSunfly, {1, 4000}));
    // The projection only grows: a stale, smaller copy never lowers it.
    QVERIFY(catalogue.setPlayStats(waterloo, {2, 1000}));
    QCOMPARE(catalogue.playStats(waterloo).playCount, 3);
    QCOMPARE(catalogue.playStats(waterloo).lastPlayedMs, 3000);
    QCOMPARE(catalogue.playStats(ringSunfly).playCount, 1);
    QCOMPARE(catalogue.playStats(ringLegends).playCount, 0);
    QCOMPARE(catalogue.playStats(ringLegends).lastPlayedMs, 0);
    // Unknown songs are ignored.
    QVERIFY(catalogue.setPlayStats(999999, {1, 5000}));
    QCOMPARE(catalogue.playStats(999999).playCount, 0);

    // Most played, then Artist A-Z among equal counts.
    QCOMPARE(describe(catalogue.browseActive(nullptr, LibrarySort::MostPlayed)),
             (QStringList{kWaterloo, kHelp, kRingSunfly, kRingLegends, kGrange, unresolved}));
    // Newest first; never-played songs after, in Artist A-Z order.
    QCOMPARE(describe(catalogue.browseActive(nullptr, LibrarySort::RecentlyPlayed)),
             (QStringList{kRingSunfly, kWaterloo, kHelp, kRingLegends, kGrange, unresolved}));

    // Search uses the same orders.
    QCOMPARE(describe(catalogue.searchActive(QStringLiteral("ring of fire"), 10, nullptr,
                                             LibrarySort::LabelAsc)),
             (QStringList{kRingLegends, kRingSunfly}));
    QCOMPARE(describe(catalogue.searchActive(QStringLiteral("ring of fire"), 10, nullptr,
                                             LibrarySort::MostPlayed)),
             (QStringList{kRingSunfly, kRingLegends}));
    QCOMPARE(describe(catalogue.searchActive(QStringLiteral("a"), 10, nullptr,
                                             LibrarySort::ArtistDesc)).first(), kGrange);

    // The catalogue's copy is rebuilt from history, for songs whose files are
    // where they were: the same path with the same sizes (another root still
    // matches: it is the same files on a moved drive). A different file at the
    // same path (another size) is another version.
    const QString helpStem = root + QStringLiteral("/Other/XYZ001-01 - Beatles - Help");
    PlayHistoryEntry entry;
    entry.identity = QStringLiteral("v1:help");
    entry.playCount = 7;
    entry.lastPlayedMs = 9000;
    entry.rootPath = QStringLiteral("/somewhere/else");
    entry.mp3RelPath = QStringLiteral("Other/XYZ001-01 - Beatles - Help.mp3");
    entry.mp3Size = QFileInfo(helpStem + QStringLiteral(".mp3")).size();
    entry.cdgSize = QFileInfo(helpStem + QStringLiteral(".cdg")).size() + 1;
    QList<PlayHistoryEntry> unmatched;
    QVERIFY(catalogue.rebuildPlayProjection({entry}, &unmatched));
    QCOMPARE(unmatched.size(), 1);
    QCOMPARE(catalogue.playStats(help).playCount, 0);
    QCOMPARE(catalogue.playStats(waterloo).playCount, 0);  // not in the history: cleared
    entry.cdgSize -= 1;
    entry.mp3Size += 1;
    unmatched.clear();
    QVERIFY(catalogue.rebuildPlayProjection({entry}, &unmatched));
    QCOMPARE(unmatched.size(), 1);
    entry.mp3Size -= 1;
    unmatched.clear();
    QVERIFY(catalogue.rebuildPlayProjection({entry}, &unmatched));
    QVERIFY(unmatched.isEmpty());
    QCOMPARE(catalogue.playStats(help).playCount, 7);
    QCOMPARE(catalogue.playStats(help).lastPlayedMs, 9000);
    QVERIFY(catalogue.rebuildPlayProjection({}, nullptr));
    QCOMPARE(catalogue.playStats(help).playCount, 0);
}

void TestLibrarySort::sortIsRememberedAndAppliesToSearch()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("music"));
    const QString path = temporary.filePath(QStringLiteral("app/library.sqlite"));
    writeSongs(root, false);
    QStringList titleDescending;
    {
        LibraryController controller(path, {}, {}, nullptr, {}, userState(temporary));
        scan(controller, root);
        QCOMPARE(controller.librarySort(), LibrarySort::ArtistAsc);
        LibraryView view(&controller);
        view.show();
        view.activate();
        QCOMPARE(view.sortBox()->currentData().toInt(), int(LibrarySort::ArtistAsc));
        QCOMPARE(view.songResultCount(), 6);
        QVERIFY(viewRows(&view).first().contains(QStringLiteral("Waterloo")));

        view.sortBox()->setCurrentIndex(view.sortBox()->findData(int(LibrarySort::TitleDesc)));
        QCOMPARE(controller.librarySort(), LibrarySort::TitleDesc);
        titleDescending = viewRows(&view);
        QVERIFY(titleDescending.first().contains(QStringLiteral("Waterloo")));
        QVERIFY(titleDescending.at(1).contains(QStringLiteral("Ring of Fire")));
        QVERIFY(titleDescending.last().contains(QStringLiteral("Track")));

        // Filtered results use the chosen order...
        view.searchBox()->setText(QStringLiteral("johnny cash"));
        view.refreshSearch();
        QCOMPARE(view.songResultCount(), 2);
        view.sortBox()->setCurrentIndex(view.sortBox()->findData(int(LibrarySort::LabelAsc)));
        QCOMPARE(controller.search(QStringLiteral("johnny cash"), 10).first().label, QStringLiteral("Legends"));
        view.sortBox()->setCurrentIndex(view.sortBox()->findData(int(LibrarySort::TitleDesc)));
        // ...and clearing the search shows the whole library in that order.
        view.searchBox()->clear();
        QCOMPARE(view.sortBox()->currentData().toInt(), int(LibrarySort::TitleDesc));
        QCOMPARE(viewRows(&view), titleDescending);
    }
    // The choice survives a restart...
    {
        LibraryController restarted(path, {}, {}, nullptr, {}, userState(temporary));
        QCOMPARE(restarted.librarySort(), LibrarySort::TitleDesc);
        LibraryView view(&restarted);
        view.show();
        view.activate();
        QCOMPARE(view.sortBox()->currentData().toInt(), int(LibrarySort::TitleDesc));
        QCOMPARE(viewRows(&view), titleDescending);
    }
    // ...and rebuilding the catalogue from scratch: it is not catalogue data.
    deleteCatalogue(path);
    LibraryController rebuilt(path, {}, {}, nullptr, {}, userState(temporary));
    QCOMPARE(rebuilt.librarySort(), LibrarySort::TitleDesc);
    scan(rebuilt, root);
    LibraryView view(&rebuilt);
    view.show();
    view.activate();
    QCOMPARE(view.sortBox()->currentData().toInt(), int(LibrarySort::TitleDesc));
    QCOMPARE(viewRows(&view), titleDescending);
}

void TestLibrarySort::playsCountOncePerSungStart()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("music"));
    writeSongs(root, true);
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")), {}, {},
                                 nullptr, {}, userState(temporary));
    scan(controller, root);
    const QMap<QString, qint64> ids = {
        {QStringLiteral("ringSunfly"), songId(controller, QStringLiteral("Sunfly"), QStringLiteral("Ring of Fire"))},
        {QStringLiteral("ringLegends"), songId(controller, QStringLiteral("Legends"), QStringLiteral("Ring of Fire"))},
        {QStringLiteral("waterloo"), songId(controller, QStringLiteral("Sunfly"), QStringLiteral("Waterloo"))},
        {QStringLiteral("grange"), songId(controller, QStringLiteral("Legends"), QStringLiteral("La Grange"))},
        {QStringLiteral("help"), songId(controller, QString(), QStringLiteral("Help"))},
    };
    auto plays = [&](const QString& key) { return controller.playStats(ids.value(key)).playCount; };

    PlaylistStore playlists(temporary.filePath(QStringLiteral("app/playlists.sqlite")));
    QVERIFY(playlists.open(nullptr, controller.libraryRoots()));
    QVERIFY(playlists.setAutoplay(true));
    qint64 playlistId = 0;
    QVERIFY(playlists.createPlaylist(QStringLiteral("Autoplay"), &playlistId));
    qint64 itemId = 0;
    QVERIFY(playlists.addItem(playlistId, *controller.songRef(ids.value(QStringLiteral("waterloo"))), &itemId));
    QVERIFY(playlists.addItem(playlistId, *controller.songRef(ids.value(QStringLiteral("grange"))), &itemId));

    BusTestPlayer player;
    const QString settingsPath = temporary.filePath(QStringLiteral("app/song-settings.json"));
    SongSettingsStore settings(settingsPath);
    MainWindow window(&player, &settings, &controller, &playlists);
    window.setShowErrorDialogs(false);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    // Library Sing loads without playing: nothing counted until Play starts it.
    QTest::mouseClick(window.findButton(), Qt::LeftButton);
    LibraryView* library = window.libraryView();
    library->searchBox()->setText(QStringLiteral("ring of fire"));
    library->refreshSearch();
    library->sortBox()->setCurrentIndex(library->sortBox()->findData(int(LibrarySort::LabelAsc)));
    library->resultsList()->setCurrentIndex(library->resultsList()->model()->index(0, 0));  // Legends
    QTest::mouseClick(library->singButton(), Qt::LeftButton);
    QCOMPARE(plays(QStringLiteral("ringLegends")), 0);
    QTest::mouseClick(window.playButton(), Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(player.state(), KaraokePlayer::State::Playing, 4000);
    QTRY_COMPARE_WITH_TIMEOUT(plays(QStringLiteral("ringLegends")), 1, 4000);
    // Pause and Resume are the same play.
    player.pause();
    QCOMPARE(player.state(), KaraokePlayer::State::Paused);
    QTest::mouseClick(window.playButton(), Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(player.state(), KaraokePlayer::State::Playing, 4000);
    QCOMPARE(plays(QStringLiteral("ringLegends")), 1);
    QTRY_COMPARE_WITH_TIMEOUT(player.state(), KaraokePlayer::State::Finished, 4000);
    QCOMPARE(plays(QStringLiteral("ringLegends")), 1);
    // The other version of the same song is untouched.
    QCOMPARE(plays(QStringLiteral("ringSunfly")), 0);
    const qint64 firstPlayed = controller.playStats(ids.value(QStringLiteral("ringLegends"))).lastPlayedMs;
    QVERIFY(firstPlayed > 0);

    // Singing it again from the start is a new play.
    QTest::mouseClick(window.playButton(), Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(player.state(), KaraokePlayer::State::Finished, 4000);
    QCOMPARE(plays(QStringLiteral("ringLegends")), 2);

    // A song opened directly counts as its catalogue song once it plays.
    QVERIFY(window.openSong(root + QStringLiteral("/Other/XYZ001-01 - Beatles - Help.cdg")));
    QCOMPARE(plays(QStringLiteral("help")), 0);
    QTest::mouseClick(window.playButton(), Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(player.state(), KaraokePlayer::State::Finished, 4000);
    QCOMPARE(plays(QStringLiteral("help")), 1);

    // A start that fails before any sound (a delayed pipeline error) is not a play.
    QVERIFY(window.openSong(root + QStringLiteral("/Sunfly/SF001-02 - Abba - Waterloo.mp3")));
    QTest::mouseClick(window.playButton(), Qt::LeftButton);
    QCOMPARE(player.state(), KaraokePlayer::State::Playing);
    QVERIFY(player.postError());
    QTRY_COMPARE_WITH_TIMEOUT(player.state(), KaraokePlayer::State::Error, 4000);
    QTest::qWait(200);
    QCOMPARE(plays(QStringLiteral("waterloo")), 0);

    // Playlist play and the song Autoplay starts after it each count once.
    QTest::mouseClick(window.playlistsButton(), Qt::LeftButton);
    window.playlistView()->itemList()->setCurrentRow(0);
    QTest::mouseClick(window.playlistView()->playButton(), Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(plays(QStringLiteral("waterloo")), 1, 4000);
    QTRY_COMPARE_WITH_TIMEOUT(plays(QStringLiteral("grange")), 1, 6000);
    QTRY_COMPARE_WITH_TIMEOUT(player.state(), KaraokePlayer::State::Finished, 4000);
    QTest::qWait(300);
    QCOMPARE(plays(QStringLiteral("waterloo")), 1);
    QCOMPARE(plays(QStringLiteral("grange")), 1);

    // A maintenance preview is never a play.
    MetadataReviewDialog* review = window.openMetadataReview();
    review->filterBox()->setCurrentIndex(review->filterBox()->findText(QStringLiteral("All songs")));
    QVERIFY(review->selectSong(ids.value(QStringLiteral("ringSunfly"))));
    QTest::mouseClick(review->playPreviewButton(), Qt::LeftButton);
    QVERIFY(window.isPreviewing());
    QTRY_COMPARE_WITH_TIMEOUT(player.state(), KaraokePlayer::State::Finished, 4000);
    QCOMPARE(plays(QStringLiteral("ringSunfly")), 0);
    review->close();

    // A song that fails to load is never counted.
    QVERIFY(QFile::resize(root + QStringLiteral("/Other/XYZ001-01 - Beatles - Help.cdg"), 0));
    QTest::mouseClick(window.findButton(), Qt::LeftButton);
    library->searchBox()->setText(QStringLiteral("help"));
    library->refreshSearch();
    QCOMPARE(library->songResultCount(), 1);
    library->resultsList()->setCurrentIndex(library->resultsList()->model()->index(0, 0));
    QTest::mouseClick(library->singButton(), Qt::LeftButton);
    QTest::qWait(300);
    QCOMPARE(plays(QStringLiteral("help")), 1);
    QVERIFY(QFileInfo(player.song().mp3Path).fileName().contains(QStringLiteral("Ring of Fire")));
    // Play then sings the song still loaded (the preview's), which is a real play of it.
    QTest::mouseClick(window.playButton(), Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(player.state(), KaraokePlayer::State::Finished, 4000);
    QCOMPARE(plays(QStringLiteral("help")), 1);
    QCOMPARE(plays(QStringLiteral("ringSunfly")), 1);
    QCOMPARE(plays(QStringLiteral("ringLegends")), 2);
    QVERIFY(!QFileInfo::exists(settingsPath));
}

void TestLibrarySort::existingVersionFiveCatalogueGainsPlayStatistics()
{
    // A catalogue already migrated to version 5 before play statistics existed.
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("music"));
    const QString path = temporary.filePath(QStringLiteral("app/library.sqlite"));
    writeSongs(root, false);
    {
        LibraryController controller(path);
        scan(controller, root);
    }
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("drop-plays"));
        database.setDatabaseName(path);
        QVERIFY(database.open());
        QSqlQuery query(database);
        QVERIFY(query.exec(QStringLiteral("DROP TABLE song_plays")));
        QVERIFY(query.exec(QStringLiteral("PRAGMA user_version")) && query.next());
        QCOMPARE(query.value(0).toInt(), Catalogue::SchemaVersion);
        query.finish();
        database.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("drop-plays"));
    Catalogue catalogue(path);
    QVERIFY(catalogue.open());
    const qint64 first = catalogue.browseActive().first().songId;
    QVERIFY(catalogue.setPlayStats(first, {1, 42}));
    QCOMPARE(catalogue.playStats(first).playCount, 1);
    QCOMPARE(catalogue.browseActive(nullptr, LibrarySort::MostPlayed).first().songId, first);
}

void TestLibrarySort::writesNeverWaitForABusyCatalogue()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("music"));
    const QString path = temporary.filePath(QStringLiteral("app/library.sqlite"));
    writeSongs(root, false);
    qint64 first = 0;
    qint64 second = 0;
    const QString connection = QStringLiteral("scan-lock");
    {
        LibraryController controller(path, {}, {}, nullptr, {}, userState(temporary));
        scan(controller, root);
        first = controller.browse().at(0).songId;
        second = controller.browse().at(1).songId;
        // Another writer (as a scan batch would) holds the catalogue.
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(path);
        QVERIFY(database.open());
        QSqlQuery lock(database);
        QVERIFY(lock.exec(QStringLiteral("BEGIN IMMEDIATE")));

        QElapsedTimer timer;
        timer.start();
        const PlaybackPaths firstPaths = controller.playbackPathsFor(first);
        controller.recordPlay(first, QStringLiteral("v1:first"), firstPaths.mp3Path, firstPaths.graphicsPath);
        controller.setLibrarySort(LibrarySort::MostPlayed);
        QVERIFY2(timer.elapsed() < 1000, qPrintable(QString::number(timer.elapsed())));
        QCOMPARE(controller.librarySort(), LibrarySort::MostPlayed);
        QCOMPARE(controller.playStats(first).playCount, 0);  // not written yet, not lost

        // Once the lock is released the waiting writes are retried.
        QVERIFY(lock.exec(QStringLiteral("COMMIT")));
        QTRY_COMPARE_WITH_TIMEOUT(controller.playStats(first).playCount, 1, 6000);
        QCOMPARE(controller.browse().first().songId, first);

        // Writes still waiting when the library closes are written then.
        QVERIFY(lock.exec(QStringLiteral("BEGIN IMMEDIATE")));
        const PlaybackPaths secondPaths = controller.playbackPathsFor(second);
        controller.recordPlay(second, QStringLiteral("v1:second"), secondPaths.mp3Path, secondPaths.graphicsPath);
        QCOMPARE(controller.playHistory(QStringLiteral("v1:second")).playCount, 1);  // durable at once
        QVERIFY(lock.exec(QStringLiteral("COMMIT")));
        lock.finish();
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
    {
        // The catalogue's own copy was written when the library closed.
        Catalogue catalogue(path);
        QVERIFY(catalogue.open());
        QCOMPARE(catalogue.playStats(first).playCount, 1);
        QCOMPARE(catalogue.playStats(second).playCount, 1);
    }
    LibraryController restarted(path, {}, {}, nullptr, {}, userState(temporary));
    QCOMPARE(restarted.librarySort(), LibrarySort::MostPlayed);
    QCOMPARE(restarted.playStats(first).playCount, 1);
}

void TestLibrarySort::playHistorySurvivesRebuildMoveAndCorrection()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("music"));
    const QString path = temporary.filePath(QStringLiteral("app/library.sqlite"));
    writeSongs(root, true);
    const QString legendsStem = QStringLiteral("Legends/LEG001-01 - Johnny Cash - Ring of Fire");
    const QString waterlooStem = QStringLiteral("Sunfly/SF001-02 - Abba - Waterloo");
    const QString legends = identityOf(root, legendsStem);
    const QString sunfly = identityOf(root, QStringLiteral("Sunfly/SF001-01 - Johnny Cash - Ring of Fire"));
    const QString waterloo = identityOf(root, waterlooStem);
    QVERIFY(!legends.isEmpty() && legends != sunfly && legends != waterloo);

    // 1. Sing: Waterloo twice, the Legends Ring of Fire once, then preview the
    //    Sunfly version from maintenance (never a play).
    qint64 legendsLastPlayed = 0;
    {
        LibraryController controller(path, {}, temporary.filePath(QStringLiteral("app/overrides.sqlite")),
                                     nullptr, {}, userState(temporary));
        scan(controller, root);
        BusTestPlayer player;
        SongSettingsStore settings(temporary.filePath(QStringLiteral("app/song-settings.json")));
        MainWindow window(&player, &settings, &controller, nullptr);
        window.setShowErrorDialogs(false);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        for (int i = 0; i < 2; ++i) {
            QVERIFY(window.openSong(root + QLatin1Char('/') + waterlooStem + QStringLiteral(".mp3")));
            QTest::mouseClick(window.playButton(), Qt::LeftButton);
            QTRY_COMPARE_WITH_TIMEOUT(player.state(), KaraokePlayer::State::Finished, 4000);
        }
        QVERIFY(window.openSong(root + QLatin1Char('/') + legendsStem + QStringLiteral(".mp3")));
        QTest::mouseClick(window.playButton(), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(player.state(), KaraokePlayer::State::Finished, 4000);
        MetadataReviewDialog* review = window.openMetadataReview();
        review->filterBox()->setCurrentIndex(review->filterBox()->findText(QStringLiteral("All songs")));
        QVERIFY(review->selectSong(songId(controller, QStringLiteral("Sunfly"), QStringLiteral("Ring of Fire"))));
        QTest::mouseClick(review->playPreviewButton(), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(player.state(), KaraokePlayer::State::Finished, 4000);
        review->close();
        controller.setLibrarySort(LibrarySort::MostPlayed);

        QCOMPARE(controller.playHistory(waterloo).playCount, 2);
        QCOMPARE(controller.playHistory(legends).playCount, 1);
        QCOMPARE(controller.playHistory(sunfly).playCount, 0);
        legendsLastPlayed = controller.playHistory(legends).lastPlayedMs;
        QVERIFY(legendsLastPlayed > 0);
        QCOMPARE(describe(controller.browse()).mid(0, 2), (QStringList{kWaterloo, kRingLegends}));
    }

    // The catalogue's copy is derived: if it is lost, the next start restores
    // it from the user state without reading any song file.
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("wipe-plays"));
        database.setDatabaseName(path);
        QVERIFY(database.open());
        QSqlQuery wipe(database);
        QVERIFY(wipe.exec(QStringLiteral("DELETE FROM song_plays")));
        wipe.finish();
        database.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("wipe-plays"));
    {
        LibraryController controller(path, {}, {}, nullptr, {}, userState(temporary));
        QCOMPARE(describe(controller.browse()).mid(0, 2), (QStringList{kWaterloo, kRingLegends}));
    }

    // 2-4. Delete the catalogue, rebuild it by scanning: the history reconnects.
    deleteCatalogue(path);
    {
        LibraryController controller(path, {}, temporary.filePath(QStringLiteral("app/overrides.sqlite")),
                                     nullptr, {}, userState(temporary));
        QCOMPARE(controller.librarySort(), LibrarySort::MostPlayed);
        scan(controller, root);
        QCOMPARE(describe(controller.browse()).mid(0, 2), (QStringList{kWaterloo, kRingLegends}));
        const qint64 legendsSong = songId(controller, QStringLiteral("Legends"), QStringLiteral("Ring of Fire"));
        QCOMPARE(controller.playStats(legendsSong).playCount, 1);
        QCOMPARE(controller.playStats(legendsSong).lastPlayedMs, legendsLastPlayed);
        QCOMPARE(controller.playStats(songId(controller, QStringLiteral("Sunfly"), QStringLiteral("Ring of Fire"))).playCount, 0);
        controller.setLibrarySort(LibrarySort::RecentlyPlayed);
        QCOMPARE(describe(controller.browse()).first(), kRingLegends);

        // A name correction and a metadata reprocess keep the history.
        QVERIFY(controller.setManualOverride(legendsSong, QStringLiteral("Johnny Cash"),
                                             QStringLiteral("Ring of Fire (Live)")));
        QSignalSpy finished(&controller, &LibraryController::scanFinished);
        controller.requestMetadataReprocess();
        QVERIFY(finished.wait(20000));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.isScanning(), 20000);
        QCOMPARE(controller.playStats(legendsSong).playCount, 1);
        QCOMPARE(describe(controller.browse()).first(), QStringLiteral("Johnny Cash|Ring of Fire (Live)|Legends"));
    }

    // Moving and renaming the files: the next scan re-finds the song by content.
    QVERIFY(QDir().mkpath(root + QStringLiteral("/Legends/moved")));
    const QString movedStem = QStringLiteral("Legends/moved/LEG001-01 - Johnny Cash - Ring of Fire (Moved)");
    for (const char* ext : {".mp3", ".cdg"})
        QVERIFY(QFile::rename(root + QLatin1Char('/') + legendsStem + QLatin1String(ext),
                              root + QLatin1Char('/') + movedStem + QLatin1String(ext)));
    deleteCatalogue(path);  // and even with the catalogue rebuilt again
    {
        LibraryController controller(path, {}, temporary.filePath(QStringLiteral("app/overrides.sqlite")),
                                     nullptr, {}, userState(temporary));
        scan(controller, root);
        qint64 movedSong = 0;
        for (const CatalogueSearchRow& row : controller.browse()) {
            const auto ref = controller.songRef(row.songId);
            if (ref && ref->mp3RelPath.contains(QStringLiteral("moved")))
                movedSong = row.songId;
        }
        QVERIFY(movedSong != 0);
        QCOMPARE(controller.playStats(movedSong).playCount, 1);
        QCOMPARE(controller.playStats(movedSong).lastPlayedMs, legendsLastPlayed);
        QCOMPARE(describe(controller.browse()).first().section(QLatin1Char('|'), 1, 1).contains(QStringLiteral("Ring of Fire")), true);
        // The history now remembers where the song is.
        QVERIFY(controller.playHistory(legends).mp3RelPath.contains(QStringLiteral("moved")));
        // Different versions still have their own histories.
        QCOMPARE(controller.playHistory(sunfly).playCount, 0);
        QCOMPARE(controller.playHistory(waterloo).playCount, 2);

        // The same files appearing as another music folder (a drive that moved
        // while its old entry remains in the catalogue) keep their history.
        const QString copy = temporary.filePath(QStringLiteral("music-again"));
        QDirIterator files(root, QDir::Files, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            const QString source = files.next();
            const QString target = copy + QLatin1Char('/') + QDir(root).relativeFilePath(source);
            QVERIFY(QDir().mkpath(QFileInfo(target).absolutePath()));
            QVERIFY(QFile::copy(source, target));
        }
        scan(controller, copy);
        QCOMPARE(controller.activeRoot().path, Catalogue::canonicalPath(copy));
        controller.setLibrarySort(LibrarySort::MostPlayed);
        QCOMPARE(describe(controller.browse()).first(), kWaterloo);
        QCOMPARE(controller.playStats(controller.browse().first().songId).playCount, 2);

        // A different song put in place of Waterloo (same name, other content)
        // does not inherit its history; the history itself is kept.
        const QString replaced = copy + QLatin1Char('/') + waterlooStem;
        QVERIFY(testmedia::writeMp3(replaced + QStringLiteral(".mp3"), 900));
        QVERIFY(testmedia::writeCdg(replaced + QStringLiteral(".cdg"), testmedia::markerCdg(900, 300)));
        scan(controller, copy);
        const qint64 replacedSong = songId(controller, QStringLiteral("Sunfly"), QStringLiteral("Waterloo"));
        QCOMPARE(controller.playStats(replacedSong).playCount, 0);
        QCOMPARE(controller.playHistory(waterloo).playCount, 2);
    }
}

void TestLibrarySort::userStateKeepsLocationsAndIsNeverErased()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path = userState(temporary);
    PlayHistoryEntry where;
    where.identity = QStringLiteral("v1:song");
    where.rootPath = QStringLiteral("/music");
    where.mp3RelPath = QStringLiteral("A/song.mp3");
    where.mp3Size = 100;
    where.cdgSize = 200;
    {
        UserStateStore store(path);
        QVERIFY(store.open());
        QVERIFY(store.recordPlay(where, 1000));
        // A play whose location could not be worked out keeps the known one.
        PlayHistoryEntry incomplete;
        incomplete.identity = where.identity;
        incomplete.rootPath = QStringLiteral("/elsewhere");
        PlayHistoryEntry updated;
        QVERIFY(store.recordPlay(incomplete, 2000, &updated));
        QCOMPARE(updated.playCount, 2);
        QCOMPARE(updated.lastPlayedMs, 2000);
        QCOMPARE(updated.rootPath, where.rootPath);
        QCOMPARE(updated.mp3RelPath, where.mp3RelPath);
        QCOMPARE(updated.cdgSize, 200);
        // An older clock never moves "last played" back.
        QVERIFY(store.recordPlay(where, 1500, &updated));
        QCOMPARE(updated.lastPlayedMs, 2000);

        // A scan's move is recorded only if nothing newer changed the location.
        PlayHistoryEntry moved = where;
        moved.mp3RelPath = QStringLiteral("B/moved.mp3");
        PlayHistoryEntry stale = where;
        stale.mp3RelPath = QStringLiteral("Z/old.mp3");
        QVERIFY(store.updateLocation(moved, stale));
        QCOMPARE(store.playHistoryFor(where.identity).mp3RelPath, where.mp3RelPath);
        QVERIFY(store.updateLocation(moved, where));
        QCOMPARE(store.playHistoryFor(where.identity).mp3RelPath, moved.mp3RelPath);
        QCOMPARE(store.playHistoryFor(where.identity).playCount, 3);

        QVERIFY(store.setPreference(QStringLiteral("library_sort"), QStringLiteral("most_played")));
        QCOMPARE(store.preference(QStringLiteral("library_sort")), QStringLiteral("most_played"));
        // Never inside a music folder.
        UserStateStore inside(temporary.filePath(QStringLiteral("music/user-state.sqlite")));
        QVERIFY(!inside.open(nullptr, {temporary.filePath(QStringLiteral("music"))}));
        QVERIFY(!QFileInfo::exists(temporary.filePath(QStringLiteral("music/user-state.sqlite"))));
    }
    // A damaged store is left alone by background work and set aside (kept)
    // by the application, never silently emptied in place.
    const QString damaged = temporary.filePath(QStringLiteral("app/damaged.sqlite"));
    testmedia::writeFile(damaged, QByteArray("definitely not sqlite"));
    {
        UserStateStore worker(damaged);
        QVERIFY(!worker.open(nullptr, {}, false));
        QCOMPARE(QFile(damaged).size(), qint64(21));
        UserStateStore app(damaged);
        QVERIFY(app.open());
        QVERIFY(app.playHistory().isEmpty());
    }
    QCOMPARE(QDir(temporary.filePath(QStringLiteral("app")))
                 .entryList({QStringLiteral("damaged.corrupt-*.sqlite")}, QDir::Files).size(), 1);
}

void TestLibrarySort::movedSongSearchReadsEachCandidateOnce()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("music"));
    const QString path = temporary.filePath(QStringLiteral("app/library.sqlite"));
    writeSongs(root, false);
    {
        LibraryController controller(path, {}, {}, nullptr, {}, userState(temporary));
        scan(controller, root);
    }
    const QString waterloo = root + QStringLiteral("/Sunfly/SF001-02 - Abba - Waterloo");
    auto addUnmatched = [&](int from, int count) {
        UserStateStore store(userState(temporary));
        QVERIFY(store.open());
        for (int i = from; i < from + count; ++i) {
            PlayHistoryEntry entry;
            entry.identity = QStringLiteral("v1:gone-%1").arg(i);
            entry.rootPath = root;
            entry.mp3RelPath = QStringLiteral("Gone/%1.mp3").arg(i);
            entry.mp3Size = QFileInfo(waterloo + QStringLiteral(".mp3")).size();
            entry.cdgSize = QFileInfo(waterloo + QStringLiteral(".cdg")).size();
            QVERIFY(store.recordPlay(entry, 1000 + i));
        }
    };
    auto rescanReads = [&] {
        LibraryScanner scanner(path);
        ScanOptions options;
        options.readTags = false;
        scanner.setOptions(options);
        scanner.setUserStatePath(userState(temporary));
        QVariantMap summary;
        QObject::connect(&scanner, &LibraryScanner::finished,
                         [&](const QVariantMap& result) { summary = result; });
        scanner.scan(root);
        return summary.value(QStringLiteral("counts")).toMap()
            .value(QStringLiteral("sourceFileReads")).toLongLong();
    };
    addUnmatched(0, 1);
    const qint64 one = rescanReads();
    addUnmatched(1, 40);
    const qint64 many = rescanReads();
    // 41 lost songs of the same sizes: the one candidate is still read once.
    QCOMPARE(many, one);
    UserStateStore store(userState(temporary));
    QVERIFY(store.open());
    QCOMPARE(store.playHistory().size(), 41);  // nothing unfound is ever dropped
}

void TestLibrarySort::sortChangeStartsAtTopWithoutSelection()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("music"));
    writeSongs(root, true);
    // Enough songs that the list scrolls.
    for (int i = 0; i < 80; ++i) {
        const QString base = root + QStringLiteral("/Filler/FL001-%1 - Filler Singer %1 - Filler Song %1")
                                        .arg(i + 1, 2, 10, QLatin1Char('0'));
        QDir().mkpath(QFileInfo(base).absolutePath());
        QVERIFY(testmedia::writeFile(base + QStringLiteral(".mp3"), QByteArray(1000 + i, 'm')));
        QVERIFY(testmedia::writeFile(base + QStringLiteral(".cdg"), QByteArray(2400 + 24 * i, 'c')));
    }
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")), {}, {},
                                 nullptr, {}, userState(temporary));
    scan(controller, root);
    PlaylistStore playlists(temporary.filePath(QStringLiteral("app/playlists.sqlite")));
    QVERIFY(playlists.open(nullptr, controller.libraryRoots()));
    qint64 playlistId = 0;
    QVERIFY(playlists.createPlaylist(QStringLiteral("Evening"), &playlistId));
    qint64 itemId = 0;
    QVERIFY(playlists.addItem(playlistId, *controller.songRef(songId(controller, QStringLiteral("Sunfly"),
                                                                     QStringLiteral("Waterloo"))), &itemId));
    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("app/song-settings.json")));
    MainWindow window(&player, &settings, &controller, &playlists);
    window.setShowErrorDialogs(false);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    // A playlist song is playing while the library is sorted.
    QTest::mouseClick(window.playlistsButton(), Qt::LeftButton);
    window.playlistView()->itemList()->setCurrentRow(0);
    QTest::mouseClick(window.playlistView()->playButton(), Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(player.state(), KaraokePlayer::State::Playing, 4000);
    const QString playing = player.song().mp3Path;
    const auto context = window.playlistPlayback()->context();
    QVERIFY(context.has_value());

    QTest::mouseClick(window.findButton(), Qt::LeftButton);
    LibraryView* library = window.libraryView();
    QListView* list = library->resultsList();
    auto sortBy = [&](LibrarySort sort) {
        library->sortBox()->setCurrentIndex(library->sortBox()->findData(int(sort)));
    };
    auto select = [&](int row) {
        list->setCurrentIndex(list->model()->index(row, 0));
        list->scrollTo(list->currentIndex());
    };

    // Browsing: a highlighted song is not followed into the new order.
    library->searchBox()->clear();
    QTRY_VERIFY(library->songResultCount() > 80);
    select(0);  // "Abba - Waterloo" at the top of Artist A-Z
    QVERIFY(library->selectedSongId() != 0);
    sortBy(LibrarySort::ArtistDesc);
    QCOMPARE(library->selectedSongId(), 0);
    QVERIFY(!list->currentIndex().isValid());
    QVERIFY(list->selectionModel()->selectedIndexes().isEmpty());
    QVERIFY(!library->singButton()->isEnabled());
    QCOMPARE(list->verticalScrollBar()->value(), list->verticalScrollBar()->minimum());
    QVERIFY(list->verticalScrollBar()->maximum() > 0);  // the list really scrolls

    // The same from far down the list.
    select(library->songResultCount() - 1);
    QVERIFY(list->verticalScrollBar()->value() > 0);
    sortBy(LibrarySort::TitleAsc);
    QCOMPARE(library->selectedSongId(), 0);
    QCOMPARE(list->verticalScrollBar()->value(), list->verticalScrollBar()->minimum());

    // Search results: the text stays, the order changes, nothing is selected.
    library->searchBox()->setText(QStringLiteral("filler"));
    library->refreshSearch();
    QCOMPARE(library->songResultCount(), 80);
    select(60);
    QVERIFY(list->verticalScrollBar()->value() > 0);
    sortBy(LibrarySort::TitleDesc);
    QCOMPARE(library->searchBox()->text(), QStringLiteral("filler"));
    QCOMPARE(library->songResultCount(), 80);
    QVERIFY(list->model()->index(0, 0).data(Qt::DisplayRole).toString().contains(QStringLiteral("Song 80")));
    QCOMPARE(library->selectedSongId(), 0);
    QCOMPARE(list->verticalScrollBar()->value(), list->verticalScrollBar()->minimum());
    QCOMPARE(controller.librarySort(), LibrarySort::TitleDesc);  // still remembered

    // Playback and its playlist context carried on untouched.
    QCOMPARE(player.song().mp3Path, playing);
    QVERIFY(player.state() == KaraokePlayer::State::Playing || player.state() == KaraokePlayer::State::Finished);
    QCOMPARE(window.playlistPlayback()->context(), context);
    QCOMPARE(playlists.items(playlistId).size(), 1);
}

QTEST_MAIN(TestLibrarySort)
#include "tst_librarysort.moc"
