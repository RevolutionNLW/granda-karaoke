#include "library/Catalogue.h"
#include "library/LibraryScanner.h"
#include "playlist/PlaylistStore.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

class CatalogueResolverTestAccess {
public:
    static QSqlDatabase database(const Catalogue& catalogue)
    {
        return catalogue.database();
    }

    static QString lookupSql()
    {
        return Catalogue::playlistSongLookupSql();
    }
};

namespace {

void writeFile(const QString& path, const QByteArray& data)
{
    QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(data), qint64(data.size()));
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

SongRef song(qint64 id, const QString& title)
{
    return {id, title, QStringLiteral("Singer"), QStringLiteral("DISC"), int(id),
            QStringLiteral("/music"), title + QStringLiteral(".mp3")};
}

QList<qint64> itemSongIds(const PlaylistStore& store, qint64 playlistId)
{
    QList<qint64> result;
    for (const PlaylistEntry& entry : store.items(playlistId))
        result.append(entry.songId);
    return result;
}

} // namespace

class TestPlaylists : public QObject {
    Q_OBJECT

private slots:
    void lifecyclePersistenceAndPreferences();
    void orderingDuplicatesSuccessorsAndCascade();
    void catalogueIsolationAndStableRescan();
    void indexedCatalogueResolversScale();
    void schemaCorruptionAndStorageSafety();
};

void TestPlaylists::lifecyclePersistenceAndPreferences()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path = temporary.filePath(QStringLiteral("app/playlists.sqlite"));
    qint64 first = 0;
    qint64 second = 0;
    {
        PlaylistStore store(path);
        QString error;
        QVERIFY2(store.open(&error), qPrintable(error));
        QVERIFY(!store.autoplay());
        QVERIFY(!store.createPlaylist(QStringLiteral("   "), nullptr, &error));
        QVERIFY(error.contains(QStringLiteral("empty")));
        QVERIFY(store.createPlaylist(QStringLiteral("  Family  "), &first));
        QVERIFY(store.createPlaylist(QStringLiteral("Family"), &second));
        QVERIFY(first != second);
        QCOMPARE(store.playlists().size(), 2);
        QCOMPARE(store.playlist(first)->name, QStringLiteral("Family"));
        const qint64 oldUpdated = store.playlist(first)->updatedAt;
        QVERIFY(store.renamePlaylist(first, QStringLiteral(" Favourites ")));
        QVERIFY(store.playlist(first)->updatedAt > oldUpdated);
        QVERIFY(store.setLastPlaylistId(999999));
        QCOMPARE(store.lastPlaylistId(), 999999LL);
        QVERIFY(store.setAutoplay(true));
        QVERIFY(store.autoplay());
    }
    {
        PlaylistStore store(path);
        QVERIFY(store.open());
        QCOMPARE(store.playlists().size(), 2);
        QCOMPARE(store.playlist(first)->name, QStringLiteral("Favourites"));
        QCOMPARE(store.lastPlaylistId(), 999999LL);
        QVERIFY(store.autoplay());
        QVERIFY(store.deletePlaylist(second));
        QCOMPARE(store.playlists().size(), 1);
    }
}

void TestPlaylists::orderingDuplicatesSuccessorsAndCascade()
{
    QTemporaryDir temporary;
    PlaylistStore store(temporary.filePath(QStringLiteral("playlists.sqlite")));
    QVERIFY(store.open());
    qint64 firstPlaylist = 0;
    qint64 secondPlaylist = 0;
    QVERIFY(store.createPlaylist(QStringLiteral("One"), &firstPlaylist));
    QVERIFY(store.createPlaylist(QStringLiteral("Two"), &secondPlaylist));

    qint64 a = 0;
    qint64 b = 0;
    qint64 c = 0;
    qint64 duplicate = 0;
    QVERIFY(store.addItem(firstPlaylist, song(1, QStringLiteral("A")), &a));
    QVERIFY(store.addItem(firstPlaylist, song(2, QStringLiteral("B")), &b));
    QVERIFY(store.insertItem(firstPlaylist, 1, song(3, QStringLiteral("C")), &c));
    QVERIFY(store.insertItem(firstPlaylist, -100, song(1, QStringLiteral("A")), &duplicate));
    QVERIFY(a != duplicate);
    QCOMPARE(itemSongIds(store, firstPlaylist), QList<qint64>({1, 1, 3, 2}));
    QVERIFY(!store.moveItemUp(duplicate));
    QCOMPARE(itemSongIds(store, firstPlaylist), QList<qint64>({1, 1, 3, 2}));
    QVERIFY(!store.moveItemDown(b));
    QVERIFY(store.moveItem(b, 1));
    QCOMPARE(itemSongIds(store, firstPlaylist), QList<qint64>({1, 2, 1, 3}));
    QVERIFY(store.moveItemUp(c));
    QVERIFY(store.moveItemDown(c));
    QCOMPARE(store.itemAfter(firstPlaylist, duplicate)->itemId, b);
    QCOMPARE(store.itemAfter(firstPlaylist, b)->itemId, a);
    QVERIFY(!store.itemAfter(firstPlaylist, c));
    QVERIFY(!store.itemAfter(firstPlaylist, 999999));

    QVERIFY(store.removeItem(duplicate));
    QVERIFY(!store.item(duplicate));
    qint64 replacement = 0;
    QVERIFY(store.addItem(firstPlaylist, song(4, QStringLiteral("D")), &replacement));
    QVERIFY(replacement > duplicate);
    const QList<PlaylistEntry> dense = store.items(firstPlaylist);
    for (int i = 0; i < dense.size(); ++i)
        QCOMPARE(dense.at(i).position, i);

    qint64 other = 0;
    QVERIFY(store.addItem(secondPlaylist, song(1, QStringLiteral("A")), &other));
    QVERIFY(other != a);
    QVERIFY(store.deletePlaylist(firstPlaylist));
    QVERIFY(store.items(firstPlaylist).isEmpty());
    QVERIFY(store.item(other));
    QVERIFY(!store.itemAfter(firstPlaylist, a));

    store.close();
    QVERIFY(store.open());
    QCOMPARE(store.items(secondPlaylist).size(), 1);
}

void TestPlaylists::catalogueIsolationAndStableRescan()
{
    QTemporaryDir temporary;
    const QString root = temporary.filePath(QStringLiteral("music"));
    const QString base = root + QStringLiteral("/AB001-01 - Singer - Song");
    writeFile(base + QStringLiteral(".mp3"), QByteArray("audio"));
    writeFile(base + QStringLiteral(".cdg"), QByteArray("graphics"));
    const QString cataloguePath = temporary.filePath(QStringLiteral("app/library.sqlite"));
    LibraryScanner scanner(cataloguePath);
    QString failure;
    connect(&scanner, &LibraryScanner::failed,
            this, [&](const QString& value) { failure = value; });
    scanner.scan(root);
    QVERIFY2(failure.isEmpty(), qPrintable(failure));

    Catalogue catalogue(cataloguePath);
    QVERIFY(catalogue.open(nullptr, {root}));
    const auto rows = catalogue.search(QStringLiteral("Song"));
    QCOMPARE(rows.size(), 1);
    const qint64 songId = rows.first().songId;
    const auto ref = catalogue.songRef(songId);
    QVERIFY(ref);

    PlaylistStore store(temporary.filePath(QStringLiteral("app/playlists.sqlite")));
    QVERIFY(store.open(nullptr, {root}));
    qint64 playlistId = 0;
    qint64 itemId = 0;
    QVERIFY(store.createPlaylist(QStringLiteral("Test"), &playlistId));
    QVERIFY(store.addItem(playlistId, *ref, &itemId));
    QVERIFY(store.removeItem(itemId));
    QCOMPARE(catalogue.search(QStringLiteral("Song")).first().songId, songId);

    catalogue.close();
    failure.clear();
    scanner.scan(root);
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
    QVERIFY(catalogue.open(nullptr, {root}));
    QCOMPARE(catalogue.search(QStringLiteral("Song")).first().songId, songId);
    QCOMPARE(catalogue.findSongByMp3Path(ref->rootPath, ref->mp3RelPath), songId);
    QCOMPARE(QFileInfo(catalogue.playbackPathsFor(songId).mp3Path).canonicalFilePath(),
             QFileInfo(QDir(root).filePath(ref->mp3RelPath)).canonicalFilePath());
}

void TestPlaylists::indexedCatalogueResolversScale()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("music"));
    QVERIFY(QDir().mkpath(root));

    Catalogue catalogue(temporary.filePath(QStringLiteral("app/library.sqlite")));
    QVERIFY(catalogue.open(nullptr, {root}));
    qint64 rootId = 0;
    QVERIFY(catalogue.addRoot(root, &rootId));

    QSqlDatabase database = CatalogueResolverTestAccess::database(catalogue);
    QVERIFY(database.transaction());
    QSqlQuery songInsert(database);
    songInsert.prepare(QStringLiteral(
        "INSERT INTO songs(id,display_title,display_artist,search_text) "
        "VALUES(?,?,?,?)"));
    QSqlQuery fileInsert(database);
    fileInsert.prepare(QStringLiteral(
        "INSERT INTO files(id,root_id,rel_path,rel_dir,file_name,ext,kind,size,"
        "mtime_ms,present,tags_state) VALUES(?,?,?,?,?,'mp3','mp3',1,0,1,'none')"));
    QSqlQuery sourceInsert(database);
    sourceInsert.prepare(QStringLiteral(
        "INSERT INTO sources(id,song_id,root_id,kind,mp3_file_id,playable,parsed_json) "
        "VALUES(?,?,?,'loose_cdg',?,1,'{}')"));

    constexpr int rowCount = 20000;
    QString targetRelativePath;
    for (int i = 1; i <= rowCount; ++i) {
        const QString relativePath = QStringLiteral("Synthetic/%1/Track-%2.mp3")
            .arg(i / 1000).arg(i, 5, 10, QLatin1Char('0'));
        if (i == rowCount)
            targetRelativePath = relativePath;

        songInsert.bindValue(0, i);
        songInsert.bindValue(1, QStringLiteral("Song %1").arg(i));
        songInsert.bindValue(2, QStringLiteral("Singer"));
        songInsert.bindValue(3, QStringLiteral("song %1 singer").arg(i));
        QVERIFY2(songInsert.exec(), qPrintable(songInsert.lastError().text()));

        const QFileInfo relativeInfo(relativePath);
        fileInsert.bindValue(0, i);
        fileInsert.bindValue(1, rootId);
        fileInsert.bindValue(2, relativePath);
        fileInsert.bindValue(3, relativeInfo.path());
        fileInsert.bindValue(4, relativeInfo.fileName());
        QVERIFY2(fileInsert.exec(), qPrintable(fileInsert.lastError().text()));

        sourceInsert.bindValue(0, i);
        sourceInsert.bindValue(1, i);
        sourceInsert.bindValue(2, rootId);
        sourceInsert.bindValue(3, i);
        QVERIFY2(sourceInsert.exec(), qPrintable(sourceInsert.lastError().text()));
    }
    QVERIFY(database.commit());

    QSqlQuery plan(database);
    plan.prepare(QStringLiteral("EXPLAIN QUERY PLAN ")
                 + CatalogueResolverTestAccess::lookupSql());
    plan.addBindValue(rootId);
    plan.addBindValue(targetRelativePath);
    QVERIFY2(plan.exec(), qPrintable(plan.lastError().text()));
    QStringList planDetails;
    while (plan.next())
        planDetails.append(plan.value(3).toString());
    const QString joinedPlan = planDetails.join(QLatin1Char('\n'));
    QVERIFY2(joinedPlan.contains(QStringLiteral("SEARCH f USING INDEX")),
             qPrintable(joinedPlan));
    QVERIFY2(joinedPlan.contains(QStringLiteral(
                 "SEARCH s USING INDEX idx_sources_mp3_file")),
             qPrintable(joinedPlan));
    QVERIFY2(!joinedPlan.contains(QStringLiteral("SCAN f")), qPrintable(joinedPlan));
    QVERIFY2(!joinedPlan.contains(QStringLiteral("SCAN s")), qPrintable(joinedPlan));

    QCOMPARE(catalogue.findSongByMp3Path(root,
                 QString(targetRelativePath).replace(QLatin1Char('/'), QLatin1Char('\\'))),
             qint64(rowCount));
    QString wrongCase = targetRelativePath;
    wrongCase.replace(QStringLiteral("Track"), QStringLiteral("track"));
    QCOMPARE(catalogue.findSongByMp3Path(root, wrongCase), 0LL);
    QCOMPARE(catalogue.findUniqueActiveSongByMp3Path(wrongCase), 0LL);

    QSqlQuery updatePresent(database);
    updatePresent.prepare(QStringLiteral("UPDATE files SET present=? WHERE id=?"));
    updatePresent.addBindValue(0);
    updatePresent.addBindValue(rowCount);
    QVERIFY2(updatePresent.exec(), qPrintable(updatePresent.lastError().text()));
    QCOMPARE(catalogue.findSongByMp3Path(root, targetRelativePath), 0LL);
    QCOMPARE(catalogue.findUniqueActiveSongByMp3Path(targetRelativePath), 0LL);
    updatePresent.bindValue(0, 1);
    updatePresent.bindValue(1, rowCount);
    QVERIFY2(updatePresent.exec(), qPrintable(updatePresent.lastError().text()));

    QSqlQuery ambiguous(database);
    QVERIFY2(ambiguous.exec(QStringLiteral(
                 "INSERT INTO songs(id,display_title,search_text) "
                 "VALUES(20001,'Ambiguous','ambiguous')")),
             qPrintable(ambiguous.lastError().text()));
    ambiguous.prepare(QStringLiteral(
        "INSERT INTO sources(id,song_id,root_id,kind,mp3_file_id,playable,parsed_json) "
        "VALUES(20001,20001,?,'loose_cdg',20000,1,'{}')"));
    ambiguous.addBindValue(rootId);
    QVERIFY2(ambiguous.exec(), qPrintable(ambiguous.lastError().text()));
    QCOMPARE(catalogue.findSongByMp3Path(root, targetRelativePath), 0LL);
    QCOMPARE(catalogue.findUniqueActiveSongByMp3Path(targetRelativePath), 0LL);
    QVERIFY(ambiguous.exec(QStringLiteral("DELETE FROM sources WHERE id=20001")));
    QVERIFY(ambiguous.exec(QStringLiteral("DELETE FROM songs WHERE id=20001")));

    QElapsedTimer timer;
    timer.start();
    bool allResolved = true;
    for (int i = 0; i < 100; ++i) {
        allResolved = allResolved
            && catalogue.findSongByMp3Path(root, targetRelativePath) == rowCount;
        allResolved = allResolved
            && catalogue.findUniqueActiveSongByMp3Path(targetRelativePath) == rowCount;
    }
    const qint64 elapsedMs = timer.elapsed();
    QVERIFY(allResolved);
    QVERIFY2(elapsedMs < 2000,
             qPrintable(QStringLiteral("200 resolver calls took %1 ms").arg(elapsedMs)));
    qInfo().noquote() << "200 indexed resolver calls took" << elapsedMs << "ms";
}

void TestPlaylists::schemaCorruptionAndStorageSafety()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());

    const QString newer = temporary.filePath(QStringLiteral("newer.sqlite"));
    const QString connection = QStringLiteral("playlist-newer-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setDatabaseName(newer);
        QVERIFY(db.open());
        QSqlQuery query(db);
        QVERIFY(query.exec(QStringLiteral("PRAGMA user_version=999")));
        db.close();
    }
    QSqlDatabase::removeDatabase(connection);
    const QByteArray before = readFile(newer);
    PlaylistStore future(newer);
    QString error;
    QVERIFY(!future.open(&error));
    QVERIFY(error.contains(QStringLiteral("newer than supported")));
    QCOMPARE(readFile(newer), before);

    const QString corrupt = temporary.filePath(QStringLiteral("playlists.sqlite"));
    writeFile(corrupt, QByteArray("definitely not sqlite"));
    PlaylistStore recovered(corrupt);
    QVERIFY2(recovered.open(&error), qPrintable(error));
    QVERIFY(recovered.createPlaylist(QStringLiteral("Recovered")));
    QCOMPARE(QDir(temporary.path()).entryList(
        {QStringLiteral("playlists.corrupt-*.sqlite")}, QDir::Files).size(), 1);

    const QString root = temporary.filePath(QStringLiteral("library"));
    QVERIFY(QDir().mkpath(root));
    PlaylistStore unsafe(root + QStringLiteral("/playlists.sqlite"));
    QVERIFY(!unsafe.open(&error, {root}));
    QVERIFY(error.contains(QStringLiteral("inside library root")));
    QVERIFY(!QFileInfo::exists(unsafe.databasePath()));

    const QString blocker = temporary.filePath(QStringLiteral("regular-file"));
    writeFile(blocker, QByteArray("x"));
    PlaylistStore unavailable(blocker + QStringLiteral("/playlists.sqlite"));
    QVERIFY(!unavailable.open(&error));
    QVERIFY(!unavailable.isOpen());
}

QTEST_GUILESS_MAIN(TestPlaylists)
#include "tst_playlists.moc"
