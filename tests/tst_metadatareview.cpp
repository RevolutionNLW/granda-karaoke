#include "BusTestPlayer.h"
#include "LibraryController.h"
#include "LyricsView.h"
#include "LibraryView.h"
#include "MainWindow.h"
#include "PlaylistView.h"
#include "MetadataReviewDialog.h"
#include "SongSettings.h"
#include "TestMedia.h"
#include "library/Catalogue.h"
#include "library/ContentIdentity.h"
#include "library/LibraryScanner.h"
#include "library/MetadataOverrideStore.h"
#include "playlist/PlaylistPlayback.h"
#include "playlist/PlaylistStore.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QTreeView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QElapsedTimer>
#include <QMutex>
#include <QtTest>

#include <atomic>
#include <functional>
#include <thread>

namespace {

void writeFile(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        qFatal("Could not write fixture");
}

QMap<QString, QByteArray> tree(const QString& root)
{
    QMap<QString, QByteArray> result;
    QDirIterator it(root, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        QFile file(path);
        QByteArray value = QByteArrayLiteral("dir");
        if (it.fileInfo().isFile() && file.open(QIODevice::ReadOnly))
            value = QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256)
                + QByteArray::number(it.fileInfo().lastModified().toMSecsSinceEpoch());
        result.insert(QDir(root).relativeFilePath(path), value);
    }
    return result;
}

struct Paths {
    QString root;
    QString catalogue;
    QString overrides;
    QString playlists;
};

void waitForIdle(LibraryController& controller)
{
    QTRY_VERIFY_WITH_TIMEOUT(!controller.isScanning(), 20000);
}

int rowFor(MetadataReviewDialog& dialog, const QString& file)
{
    QTableWidget* table = dialog.songTable();
    for (int i = 0; i < table->rowCount(); ++i) {
        if (table->item(i, 4)->text() == file)
            return i;
    }
    return -1;
}

QByteArray readAll(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        qFatal("Could not read fixture");
    return file.readAll();
}

// Runs statements on a plain read-write SQLite connection; `during` is called
// while the connection is still open (before SQLite tidies up on close).
void withRawConnection(const QString& path, const QStringList& statements,
                       const std::function<void()>& during = {})
{
    const QString connection = QStringLiteral("raw-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(path);
        if (!database.open())
            qFatal("Could not open raw fixture");
        QSqlQuery query(database);
        for (const QString& statement : statements) {
            if (!query.exec(statement))
                qFatal("Raw fixture statement failed: %s", qPrintable(query.lastError().text()));
        }
        query.finish();
        if (during)
            during();
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
}

// A catalogue whose only record of `root` is a committed but not yet
// checkpointed WAL: any read-write open replays it into the main file.
void writePendingWalCatalogue(const QString& staging, const QString& target, const QString& root)
{
    const QString path = staging + QStringLiteral("/wal.sqlite");
    {
        Catalogue catalogue(path);
        if (!catalogue.open())
            qFatal("Could not create WAL fixture");
    }
    const QByteArray base = readAll(path);
    QByteArray wal;
    withRawConnection(path, {QStringLiteral("PRAGMA wal_autocheckpoint=0"),
                             QStringLiteral("INSERT INTO library_roots(path,added_at,online,active) "
                                            "VALUES('%1',0,1,1)").arg(root)},
                      [&] { wal = readAll(path + QStringLiteral("-wal")); });
    if (wal.size() < 32)
        qFatal("WAL fixture has no pending frames");
    writeFile(target, base);
    writeFile(target + QStringLiteral("-wal"), wal);
}

// A catalogue left mid-transaction in rollback-journal mode: a hot journal
// that any read-write open rolls back and deletes.
void writeHotJournalCatalogue(const QString& staging, const QString& target)
{
    const QString path = staging + QStringLiteral("/journal.sqlite");
    withRawConnection(path, {QStringLiteral("PRAGMA journal_mode=DELETE"),
                             QStringLiteral("CREATE TABLE library_roots(id INTEGER PRIMARY KEY, path TEXT)"),
                             QStringLiteral("WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM n WHERE i<2000) "
                                            "INSERT INTO library_roots(path) SELECT printf('%0200d', i) FROM n")});
    QByteArray main;
    QByteArray journal;
    // A tiny page cache makes SQLite spill changed pages into the main file
    // mid-transaction, which first syncs a complete (hot) journal.
    withRawConnection(path, {QStringLiteral("PRAGMA journal_mode=DELETE"),
                             QStringLiteral("PRAGMA cache_size=2"),
                             QStringLiteral("BEGIN IMMEDIATE"),
                             QStringLiteral("UPDATE library_roots SET path=printf('%0200d', -id)")},
                      [&] {
                          main = readAll(path);
                          journal = readAll(path + QStringLiteral("-journal"));
                      });
    if (journal.isEmpty())
        qFatal("Journal fixture has no journal");
    writeFile(target, main);
    writeFile(target + QStringLiteral("-journal"), journal);
}

// A short playable synthetic song (quiet tone plus a two-marker CDG).
void writePlayableSong(const QString& mp3Path, int durationMs)
{
    QDir().mkpath(QFileInfo(mp3Path).absolutePath());
    const QString cdgPath = mp3Path.chopped(4) + QStringLiteral(".cdg");
    if (!testmedia::writeMp3(mp3Path, durationMs)
        || !testmedia::writeCdg(cdgPath, testmedia::markerCdg(durationMs, qMax(50, durationMs / 3))))
        qFatal("Could not write synthetic playable song");
}

// Replaces a song's stored evidence, as a title-screen reprocess would.
void setEvidence(const QString& cataloguePath, qint64 songId, const QString& json)
{
    const QString connection = QStringLiteral("evidence-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(cataloguePath);
        if (!database.open())
            qFatal("Could not open catalogue for evidence fixture");
        QSqlQuery query(database);
        query.prepare(QStringLiteral("UPDATE songs SET evidence_json=? WHERE id=?"));
        query.addBindValue(json);
        query.addBindValue(songId);
        if (!query.exec() || query.numRowsAffected() != 1)
            qFatal("Could not set evidence fixture");
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
}

QString ocrEvidence(const QStringList& readings)
{
    QStringList items = {QStringLiteral(
        "{\"artist\":\"\",\"confidence\":\"unresolved\",\"source\":\"fallback\",\"title\":\"Disc SGB39 - Track 02\"}")};
    for (const QString& reading : readings)
        items.append(QStringLiteral("{\"engine\":\"test\",\"source\":\"cdg_ocr\",\"text\":\"%1\",\"validated\":false}")
                         .arg(reading));
    return QStringLiteral("{\"conflicts\":[],\"evidence\":[%1],\"rule\":\"fallback\",\"v\":5}")
        .arg(items.join(QLatin1Char(',')));
}

} // namespace

class TestMetadataReview : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void correctionLifecycleKeepsIdentityAndSources();
    void upgradeReprocessesInBackgroundAndStaysSearchable();
    void nameEditsKeepImportedValues();
    void damagedOverrideStoreIsRebuiltNotErased();
    void syncAndEditsAreSerialised();
    void damagedCatalogueInsideLibraryRootIsNeverTouched();
    void pendingJournalCatalogueInsideRootIsRefusedBeforeSqlite();
    void previewNeverCreatesPlaylistContextOrAutoplays();
    void detectedTitleFillsEditWithoutSaving();
    void revertToAutomaticRestoresAutomaticName();
};

void TestMetadataReview::initTestCase()
{
    QString error;
    QVERIFY2(KaraokePlayer::initializeGStreamer(&error), qPrintable(error));
}

void TestMetadataReview::correctionLifecycleKeepsIdentityAndSources()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const Paths paths{temporary.filePath(QStringLiteral("music")),
                      temporary.filePath(QStringLiteral("app/library.sqlite")),
                      temporary.filePath(QStringLiteral("app/metadata-overrides.sqlite")),
                      temporary.filePath(QStringLiteral("app/playlists.sqlite"))};
    writeFile(paths.root + QStringLiteral("/SGB39/3902.mp3"), QByteArray(3000, 'a'));
    writeFile(paths.root + QStringLiteral("/SGB39/3902.cdg"), QByteArray(2400, 'b'));
    writeFile(paths.root + QStringLiteral("/SGB39/3903.mp3"), QByteArray(3100, 'c'));
    writeFile(paths.root + QStringLiteral("/SGB39/3903.cdg"), QByteArray(2400, 'd'));
    writeFile(paths.root + QStringLiteral("/DK/DK001-01 - Crazy - Nelson, Willie.mp3"), QByteArray(3200, 'e'));
    writeFile(paths.root + QStringLiteral("/DK/DK001-01 - Crazy - Nelson, Willie.cdg"), QByteArray(2400, 'f'));
    const QMap<QString, QByteArray> before = tree(paths.root);
    const QString identity = contentIdentity(paths.root + QStringLiteral("/SGB39/3902.mp3"),
                                             paths.root + QStringLiteral("/SGB39/3902.cdg"));
    QVERIFY(!identity.isEmpty());

    qint64 songId = 0;
    qint64 itemId = 0;
    {
        LibraryController controller(paths.catalogue, {}, paths.overrides);
        controller.setProtectedStoragePaths({paths.overrides, paths.playlists});
        QVERIFY(controller.chooseRoot(paths.root));
        waitForIdle(controller);

        // The unresolved song is listed with its evidence.
        MetadataReviewDialog dialog(&controller);
        dialog.show();
        QCOMPARE(dialog.filterBox()->currentText(), QStringLiteral("Unresolved"));
        const int row = rowFor(dialog, QStringLiteral("SGB39/3902.mp3"));
        QVERIFY(row >= 0);
        QCOMPARE(rowFor(dialog, QStringLiteral("DK/DK001-01 - Crazy - Nelson, Willie.mp3")), -1);
        dialog.songTable()->selectRow(row);
        songId = dialog.selectedSongId();
        QVERIFY(songId != 0);
        QVERIFY(dialog.detailView()->toPlainText().contains(QStringLiteral("SGB39/3902.mp3")));
        QVERIFY(dialog.detailView()->toPlainText().contains(QStringLiteral("Disc SGB39 - Track 02")));
        QVERIFY(!dialog.clearButton()->isEnabled());
        for (QPushButton* button : dialog.findChildren<QPushButton*>())
            QCOMPARE(button->focusPolicy(), Qt::NoFocus);

        // The song is in a playlist before it is corrected.
        PlaylistStore playlists(paths.playlists);
        QString error;
        QVERIFY2(playlists.open(&error, controller.libraryRoots()), qPrintable(error));
        qint64 playlistId = 0;
        QVERIFY(playlists.createPlaylist(QStringLiteral("Friday"), &playlistId));
        const auto ref = controller.songRef(songId);
        QVERIFY(ref);
        QVERIFY(playlists.addItem(playlistId, *ref, &itemId));

        // Correct it by hand (Enter in the title box saves).
        dialog.artistEdit()->setText(QStringLiteral("Frank Sinatra"));
        dialog.titleEdit()->setText(QStringLiteral("My Way"));
        QTest::keyClick(dialog.titleEdit(), Qt::Key_Return);
        // It left the Unresolved list; the next unresolved song is selected.
        QVERIFY(dialog.selectedSongId() != songId);
        QCOMPARE(rowFor(dialog, QStringLiteral("SGB39/3902.mp3")), -1);
        dialog.filterBox()->setCurrentText(QStringLiteral("Manual corrections"));
        QVERIFY(dialog.selectSong(songId));
        QVERIFY(dialog.clearButton()->isEnabled());

        // Found by the correction and by the old ugly name.
        QCOMPARE(controller.search(QStringLiteral("frank sinatra my way"), 10).size(), 1);
        QCOMPARE(controller.search(QStringLiteral("3902"), 10).size(), 1);
        QCOMPARE(controller.search(QStringLiteral("sgb39"), 10).size(), 2);
        // The playlist item is the same song, now shown with the new name.
        const auto entry = playlists.item(itemId);
        QVERIFY(entry);
        const PlaylistSongResolution resolution = controller.resolvePlaylistSong(*entry);
        QCOMPARE(resolution.songId, songId);
        QCOMPARE(controller.songRef(songId)->title, QStringLiteral("My Way"));
        QCOMPARE(QFileInfo(controller.playbackPathsFor(songId).mp3Path).canonicalFilePath(),
                 QFileInfo(paths.root + QStringLiteral("/SGB39/3902.mp3")).canonicalFilePath());
    }

    // A restart keeps the correction; a reprocess never overwrites it.
    {
        LibraryController controller(paths.catalogue, {}, paths.overrides);
        waitForIdle(controller);
        QCOMPARE(controller.reviewDetail(songId).value(QStringLiteral("title")).toString(),
                 QStringLiteral("My Way"));
        controller.requestMetadataReprocess();
        QVERIFY(controller.isScanning());
        waitForIdle(controller);
        const QVariantMap detail = controller.reviewDetail(songId);
        QCOMPARE(detail.value(QStringLiteral("title")).toString(), QStringLiteral("My Way"));
        QCOMPARE(detail.value(QStringLiteral("artist")).toString(), QStringLiteral("Frank Sinatra"));
        QCOMPARE(detail.value(QStringLiteral("autoTitle")).toString(),
                 QStringLiteral("Disc SGB39 - Track 02"));
    }

    // Deleting the catalogue loses nothing the user entered.
    for (const QString& suffix : {QString(), QStringLiteral("-wal"), QStringLiteral("-shm")})
        QFile::remove(paths.catalogue + suffix);
    {
        LibraryController controller(paths.catalogue, {}, paths.overrides);
        QVERIFY(controller.chooseRoot(paths.root));
        waitForIdle(controller);
        const QList<CatalogueSearchRow> rows = controller.search(QStringLiteral("my way"), 10);
        QCOMPARE(rows.size(), 1);
        const qint64 rebuiltId = rows.first().songId;
        PlaylistStore playlists(paths.playlists);
        QVERIFY(playlists.open(nullptr, controller.libraryRoots()));
        const auto entry = playlists.item(itemId);
        QVERIFY(entry);
        QCOMPARE(controller.resolvePlaylistSong(*entry).songId, rebuiltId);

        // Clearing returns to the automatic name.
        MetadataReviewDialog dialog(&controller);
        dialog.filterBox()->setCurrentText(QStringLiteral("Manual corrections"));
        QVERIFY(dialog.selectSong(rebuiltId));
        QTest::mouseClick(dialog.clearButton(), Qt::LeftButton);
        QCOMPARE(controller.reviewDetail(rebuiltId).value(QStringLiteral("title")).toString(),
                 QStringLiteral("Disc SGB39 - Track 02"));
        QCOMPARE(controller.search(QStringLiteral("my way"), 10).size(), 0);
        MetadataOverrideStore store(paths.overrides);
        QVERIFY(store.open());
        QVERIFY(store.all().isEmpty());
    }

    // Key/Tempo memory is keyed by content, which none of this touched.
    QCOMPARE(contentIdentity(paths.root + QStringLiteral("/SGB39/3902.mp3"),
                             paths.root + QStringLiteral("/SGB39/3902.cdg")),
             identity);
    QCOMPARE(tree(paths.root), before);
}

void TestMetadataReview::upgradeReprocessesInBackgroundAndStaysSearchable()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("music"));
    const QString catalogue = temporary.filePath(QStringLiteral("app/library.sqlite"));
    for (int i = 1; i <= 30; ++i) {
        const QString stem = root + QStringLiteral("/EK14/14--%1").arg(i);
        writeFile(stem + QStringLiteral(".mp3"), QByteArray(1000 + i, 'm'));
        writeFile(stem + QStringLiteral(".cdg"), QByteArray(2400, char('a' + i % 20)));
    }
    {
        LibraryController controller(catalogue);
        QVERIFY(controller.chooseRoot(root));
        waitForIdle(controller);
    }
    // Pretend the catalogue was written by older rules: an old resolver version
    // and names parsed by an older parser.
    {
        const QString connection = QUuid::createUuid().toString();
        {
            QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
            database.setDatabaseName(catalogue);
            QVERIFY(database.open());
            QSqlQuery query(database);
            QVERIFY(query.exec(QStringLiteral(
                "UPDATE catalogue_meta SET value='4' WHERE key IN ('resolver_version','parser_version')")));
            QVERIFY(query.exec(QStringLiteral(
                "UPDATE sources SET parsed_json=json_set(parsed_json,'$.kind',6,'$.discId','',"
                "'$.track',0,'$.fields',json_array(json_extract(parsed_json,'$.stem')))")));
            QVERIFY(query.exec(QStringLiteral(
                "UPDATE songs SET display_title='stale',search_text='stale',resolver_version=4")));
            database.close();
        }
        QSqlDatabase::removeDatabase(connection);
    }
    // While a song plays the background reprocess waits, and the old names
    // are still searchable; afterwards the new rules apply.
    LibraryController controller(catalogue);
    controller.setPlaybackActive(true);
    QTRY_VERIFY_WITH_TIMEOUT(controller.isScanning(), 5000);
    QTest::qWait(300);
    QVERIFY(controller.isScanning());
    QCOMPARE(controller.search(QStringLiteral("stale"), 100).size(), 30);
    controller.setPlaybackActive(false);
    waitForIdle(controller);
    QCOMPARE(controller.search(QStringLiteral("stale"), 100).size(), 0);
    const QList<CatalogueSearchRow> rows = controller.search(QStringLiteral("ek14 16"), 100);
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows.first().displayTitle, QStringLiteral("Disc EK14 - Track 16"));
    QCOMPARE(controller.reviewCount(ReviewFilter::Unresolved), 30);
}

void TestMetadataReview::nameEditsKeepImportedValues()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("music"));
    const QString catalogue = temporary.filePath(QStringLiteral("app/library.sqlite"));
    const QString overrides = temporary.filePath(QStringLiteral("app/metadata-overrides.sqlite"));
    writeFile(root + QStringLiteral("/New/new-01.mp3"), QByteArray(3000, 'a'));
    writeFile(root + QStringLiteral("/New/new-01.cdg"), QByteArray(2400, 'b'));
    LibraryController controller(catalogue, {}, overrides);
    QVERIFY(controller.chooseRoot(root));
    waitForIdle(controller);
    const qint64 songId = controller.search(QStringLiteral("new 01"), 10).first().songId;
    MetadataOverride imported;
    imported.artist = QStringLiteral("Patsy Cline");
    imported.title = QStringLiteral("Crazy");
    imported.label = QStringLiteral("Legends");
    imported.series = QStringLiteral("Country Gold");
    imported.trustedDiscId = QStringLiteral("LEG099");
    imported.trustedTrack = 7;
    imported.origin = QStringLiteral("import");
    QString error;
    QVERIFY2(controller.setTrustedMetadata(songId, imported, &error), qPrintable(error));

    // The review screen changes the title only.
    QVERIFY2(controller.setManualOverride(songId, QStringLiteral("Patsy Cline"),
                                          QStringLiteral("Crazy (Live)"), &error), qPrintable(error));
    MetadataOverride kept = controller.existingTrusted(songId);
    QCOMPARE(*kept.title, QStringLiteral("Crazy (Live)"));
    QCOMPARE(*kept.label, QStringLiteral("Legends"));
    QCOMPARE(*kept.series, QStringLiteral("Country Gold"));
    QCOMPARE(*kept.trustedDiscId, QStringLiteral("LEG099"));
    QCOMPARE(*kept.trustedTrack, 7);
    QCOMPARE(kept.origin, QStringLiteral("import"));
    QVariantMap detail = controller.reviewDetail(songId);
    QCOMPARE(detail.value(QStringLiteral("label")).toString(), QStringLiteral("Legends"));
    QCOMPARE(detail.value(QStringLiteral("discId")).toString(), QStringLiteral("LEG099"));

    // "Use Automatic Name" returns only the name to automatic.
    QVERIFY2(controller.clearManualOverride(songId, &error), qPrintable(error));
    kept = controller.existingTrusted(songId);
    QVERIFY(!kept.title && !kept.artist);
    QCOMPARE(*kept.label, QStringLiteral("Legends"));
    detail = controller.reviewDetail(songId);
    QCOMPARE(detail.value(QStringLiteral("title")).toString(), QStringLiteral("new-01"));
    QCOMPARE(detail.value(QStringLiteral("label")).toString(), QStringLiteral("Legends"));
    QCOMPARE(detail.value(QStringLiteral("track")).toInt(), 7);
}

void TestMetadataReview::damagedOverrideStoreIsRebuiltNotErased()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("music"));
    const QString catalogue = temporary.filePath(QStringLiteral("app/library.sqlite"));
    const QString overrides = temporary.filePath(QStringLiteral("app/metadata-overrides.sqlite"));
    writeFile(root + QStringLiteral("/SGB39/3902.mp3"), QByteArray(3000, 'a'));
    writeFile(root + QStringLiteral("/SGB39/3902.cdg"), QByteArray(2400, 'b'));
    qint64 songId = 0;
    {
        LibraryController controller(catalogue, {}, overrides);
        QVERIFY(controller.chooseRoot(root));
        waitForIdle(controller);
        songId = controller.search(QStringLiteral("3902"), 10).first().songId;
        QString error;
        QVERIFY2(controller.setManualOverride(songId, QStringLiteral("Frank Sinatra"),
                                              QStringLiteral("My Way"), &error), qPrintable(error));
    }
    // Damage the authoritative store.
    for (const QString& suffix : {QStringLiteral("-wal"), QStringLiteral("-shm")})
        QFile::remove(overrides + suffix);
    {
        QFile file(overrides);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(QByteArray(8192, 'x'));
    }
    {
        LibraryController controller(catalogue, {}, overrides);
        waitForIdle(controller);
        QCOMPARE(controller.reviewDetail(songId).value(QStringLiteral("title")).toString(),
                 QStringLiteral("My Way"));
        // The store was rebuilt from the catalogue, and the damaged copy kept.
        QCOMPARE(*controller.existingTrusted(songId).title, QStringLiteral("My Way"));
        QCOMPARE(QDir(QFileInfo(overrides).absolutePath())
                     .entryList({QStringLiteral("metadata-overrides.corrupt-*")}, QDir::Files).size(), 1);
        controller.requestMetadataReprocess();
        QVERIFY(controller.isScanning());
        waitForIdle(controller);
        QCOMPARE(controller.reviewDetail(songId).value(QStringLiteral("title")).toString(),
                 QStringLiteral("My Way"));
    }
    // Even if the store is lost outright, a rescan keeps the corrections.
    for (const QString& suffix : {QString(), QStringLiteral("-wal"), QStringLiteral("-shm")})
        QFile::remove(overrides + suffix);
    {
        LibraryController controller(catalogue, {}, overrides);
        QVERIFY(controller.chooseRoot(root));
        waitForIdle(controller);
        QCOMPARE(controller.search(QStringLiteral("frank sinatra my way"), 10).size(), 1);
    }
}

void TestMetadataReview::syncAndEditsAreSerialised()
{
    // A background sync reads the store and applies it to the catalogue; a
    // correction changes both. Neither may interleave with the other, or a
    // just-cleared correction could be applied again.
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("music"));
    const QString catalogue = temporary.filePath(QStringLiteral("app/library.sqlite"));
    const QString overrides = temporary.filePath(QStringLiteral("app/metadata-overrides.sqlite"));
    writeFile(root + QStringLiteral("/SGB39/3902.mp3"), QByteArray(3000, 'a'));
    writeFile(root + QStringLiteral("/SGB39/3902.cdg"), QByteArray(2400, 'b'));
    LibraryController controller(catalogue, {}, overrides);
    QVERIFY(controller.chooseRoot(root));
    waitForIdle(controller);
    const qint64 songId = controller.search(QStringLiteral("3902"), 10).first().songId;

    // While a sync holds the lock (simulated on another thread), a correction
    // made on the GUI thread waits for it.
    QMutex& lock = MetadataOverrideStore::synchronisation();
    std::atomic_bool held = false;
    std::thread syncing([&] {
        QMutexLocker locker(&lock);
        held = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
    });
    while (!held.load())
        std::this_thread::yield();
    QElapsedTimer timer;
    timer.start();
    QVERIFY(controller.setManualOverride(songId, QStringLiteral("Frank Sinatra"),
                                         QStringLiteral("My Way")));
    QVERIFY(timer.elapsed() >= 300);
    syncing.join();

    // And the worker's sync waits for an edit in progress.
    lock.lock();
    controller.requestMetadataReprocess();
    QTest::qWait(1500);
    QVERIFY(controller.isScanning());
    lock.unlock();
    waitForIdle(controller);
    QCOMPARE(controller.reviewDetail(songId).value(QStringLiteral("title")).toString(),
             QStringLiteral("My Way"));
}

QTEST_MAIN(TestMetadataReview)
#include "tst_metadatareview.moc"

void TestMetadataReview::damagedCatalogueInsideLibraryRootIsNeverTouched()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString music = temporary.filePath(QStringLiteral("music"));
    writeFile(music + QStringLiteral("/SF001/SF001-01 - Test Artist - Test Song.mp3"), QByteArray("mp3"));
    writeFile(music + QStringLiteral("/SF001/SF001-01 - Test Artist - Test Song.cdg"), QByteArray("cdg"));

    // An old catalogue inside the music folder that records that folder as
    // its root, then damaged: its header still reads, but the root list does not.
    const QString legacy = music + QStringLiteral("/library.sqlite");
    {
        Catalogue catalogue(legacy);
        QVERIFY(catalogue.open());
        QVERIFY(!catalogue.addRoot(music, nullptr));  // today's builds refuse this layout
    }
    const QString connection = QStringLiteral("legacy-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(legacy);
        QVERIFY(database.open());
        QSqlQuery query(database);
        QVERIFY(query.exec(QStringLiteral("PRAGMA journal_mode=DELETE")));
        query.prepare(QStringLiteral("INSERT INTO library_roots(path,added_at,online,active) VALUES(?,0,1,1)"));
        query.addBindValue(Catalogue::canonicalPath(music));
        QVERIFY(query.exec());
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
    {
        QFile file(legacy);
        QVERIFY(file.open(QIODevice::ReadWrite));
        const QByteArray header = file.read(100);
        const int pageSize = (quint8(header.at(16)) << 8) | quint8(header.at(17));
        QVERIFY(pageSize >= 512 && file.size() > pageSize);
        file.seek(pageSize);
        file.write(QByteArray(int(file.size() - pageSize), '\xA5'));
    }
    // Leftovers of older versions beside it, also damaged.
    writeFile(legacy + QStringLiteral("-wal"), QByteArray("stale wal bytes"));
    writeFile(legacy + QStringLiteral("-shm"), QByteArray("stale shm bytes"));
    writeFile(music + QStringLiteral("/old-library.sqlite"), QByteArray("definitely not sqlite"));
    writeFile(music + QStringLiteral("/old-library.sqlite-journal"), QByteArray("hot journal?"));
    const QMap<QString, QByteArray> before = tree(music);
    QVERIFY(before.contains(QStringLiteral("library.sqlite")));

    QString error;
    for (const QString& path : {legacy, music + QStringLiteral("/old-library.sqlite")}) {
        // No roots known: a damaged file cannot be proved safe, so it is refused.
        for (const bool recovery : {false, true}) {
            Catalogue catalogue(path);
            catalogue.setCorruptionRecoveryAllowed(recovery);
            error.clear();
            QVERIFY2(!catalogue.open(&error), qPrintable(path));
            QVERIFY2(error.contains(QStringLiteral("untouched")), qPrintable(error));
            QCOMPARE(tree(music), before);
        }
        // The music folder known independently: refused before it is read.
        Catalogue catalogue(path);
        catalogue.setCorruptionRecoveryAllowed(true);
        error.clear();
        QVERIFY(!catalogue.open(&error, {music}));
        QVERIFY2(error.contains(QStringLiteral("must not be inside")), qPrintable(error));
        QCOMPARE(tree(music), before);
    }
    {
        // The application pointed at it, remembering the folder: refused.
        LibraryController controller(legacy, {}, temporary.filePath(QStringLiteral("app/overrides.sqlite")),
                                     nullptr, {Catalogue::canonicalPath(music)});
        QVERIFY(!controller.isAvailable());
        QCOMPARE(tree(music), before);
    }
    QCOMPARE(tree(music), before);

    // The application's own catalogue in app data scans that folder normally
    // and leaves every file under it exactly as it was.
    {
        LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")), {},
                                     temporary.filePath(QStringLiteral("app/overrides.sqlite")));
        QVERIFY2(controller.isAvailable(), qPrintable(controller.openError()));
        QVERIFY2(controller.chooseRoot(music, &error), qPrintable(error));
        waitForIdle(controller);
        QCOMPARE(controller.search(QStringLiteral("test song"), 10).size(), 1);
    }
    QCOMPARE(tree(music), before);
}

void TestMetadataReview::pendingJournalCatalogueInsideRootIsRefusedBeforeSqlite()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString music = temporary.filePath(QStringLiteral("music"));
    const QString staging = temporary.filePath(QStringLiteral("staging"));
    const QString control = temporary.filePath(QStringLiteral("control"));
    QVERIFY(QDir().mkpath(staging));
    writeFile(music + QStringLiteral("/SF001/SF001-01 - Test Artist - Test Song.mp3"), QByteArray("mp3"));
    writeFile(music + QStringLiteral("/SF001/SF001-01 - Test Artist - Test Song.cdg"), QByteArray("cdg"));
    const QString walCatalogue = music + QStringLiteral("/library.sqlite");
    const QString journalCatalogue = music + QStringLiteral("/old/catalogue.sqlite");
    writePendingWalCatalogue(staging, walCatalogue, Catalogue::canonicalPath(music));
    writeHotJournalCatalogue(staging, journalCatalogue);

    // Control: the fixtures really are pending. A plain read-write open of an
    // identical copy outside the music folder rewrites the files.
    for (const QString& name : {QStringLiteral("library.sqlite"), QStringLiteral("old/catalogue.sqlite")}) {
        const QString source = music + QLatin1Char('/') + name;
        const QString copy = control + QLatin1Char('/') + QFileInfo(name).fileName();
        for (const QString& suffix : {QString(), QStringLiteral("-wal"), QStringLiteral("-journal")}) {
            if (QFileInfo::exists(source + suffix))
                writeFile(copy + suffix, readAll(source + suffix));
        }
        const QMap<QString, QByteArray> pending = tree(control);
        withRawConnection(copy, {QStringLiteral("SELECT count(*) FROM library_roots")});
        QVERIFY2(tree(control) != pending, qPrintable(name));
    }

    const QMap<QString, QByteArray> before = tree(music);
    QVERIFY(before.contains(QStringLiteral("library.sqlite-wal")));
    QVERIFY(before.contains(QStringLiteral("old/catalogue.sqlite-journal")));
    const QStringList known = {Catalogue::canonicalPath(music)};
    // The WAL catalogue's main file does not record the music folder, so a
    // refusal naming it can only come from the check made before SQLite runs.
    for (const QString& path : {walCatalogue, journalCatalogue}) {
        for (const bool recovery : {false, true}) {
            Catalogue catalogue(path);
            catalogue.setCorruptionRecoveryAllowed(recovery);
            QString error;
            QVERIFY(!catalogue.open(&error, known));
            QVERIFY2(error.contains(QStringLiteral("must not be inside library root")), qPrintable(error));
            QCOMPARE(tree(music), before);
        }
        {
            LibraryController controller(path, {}, temporary.filePath(QStringLiteral("app/overrides.sqlite")),
                                         nullptr, known);
            QVERIFY(!controller.isAvailable());
        }
        QCOMPARE(tree(music), before);
        // Background tools: the reprocess and scan workers used by the app and CLI.
        {
            LibraryScanner scanner(path);
            scanner.setKnownRoots(known);
            QSignalSpy failed(&scanner, &LibraryScanner::failed);
            scanner.reprocessMetadata();
            QCOMPARE(failed.size(), 1);
            QVERIFY(failed.first().first().toString().contains(QStringLiteral("must not be inside")));
        }
        QCOMPARE(tree(music), before);
        {
            LibraryScanner scanner(path);
            QSignalSpy failed(&scanner, &LibraryScanner::failed);
            scanner.scan(music);
            QCOMPARE(failed.size(), 1);
            QVERIFY(failed.first().first().toString().contains(QStringLiteral("must not be inside")));
        }
        QCOMPARE(tree(music), before);
        {
            MetadataOverrideStore store(path);
            QString error;
            QVERIFY(!store.open(&error, known));
        }
        QCOMPARE(tree(music), before);
    }
}

void TestMetadataReview::previewNeverCreatesPlaylistContextOrAutoplays()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("music"));
    const QString unresolvedMp3 = root + QStringLiteral("/SGB39/3902.mp3");
    const QString otherMp3 = root + QStringLiteral("/SGB39/3903.mp3");
    const QString namedMp3 = root + QStringLiteral("/PV001/PV001-01 - Test Singer - Named Song.mp3");
    writePlayableSong(unresolvedMp3, 450);
    writePlayableSong(otherMp3, 1500);
    writePlayableSong(namedMp3, 450);
    const QString settingsPath = temporary.filePath(QStringLiteral("app/song-settings.json"));

    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")));
    QVERIFY(controller.chooseRoot(root));
    waitForIdle(controller);
    const QMap<QString, QByteArray> before = tree(root);
    PlaylistStore playlists(temporary.filePath(QStringLiteral("app/playlists.sqlite")));
    QVERIFY(playlists.open(nullptr, controller.libraryRoots()));
    QVERIFY(playlists.setAutoplay(true));
    qint64 playlistId = 0;
    QVERIFY(playlists.createPlaylist(QStringLiteral("Autoplay"), &playlistId));
    const qint64 namedId = controller.search(QStringLiteral("Named Song"), 5).first().songId;
    qint64 namedItem = 0;
    QVERIFY(playlists.addItem(playlistId, *controller.songRef(namedId), &namedItem));

    BusTestPlayer player;
    SongSettingsStore settings(settingsPath);
    MainWindow window(&player, &settings, &controller, &playlists);
    window.setShowErrorDialogs(false);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    MetadataReviewDialog* dialog = window.openMetadataReview();
    QVERIFY(dialog);
    const int unresolvedRow = rowFor(*dialog, QStringLiteral("SGB39/3902.mp3"));
    const int otherRow = rowFor(*dialog, QStringLiteral("SGB39/3903.mp3"));
    QVERIFY(unresolvedRow >= 0 && otherRow >= 0);

    // Selecting rows never starts anything.
    dialog->songTable()->selectRow(otherRow);
    dialog->songTable()->selectRow(unresolvedRow);
    QTest::qWait(100);
    QCOMPARE(player.state(), KaraokePlayer::State::Empty);
    QVERIFY(dialog->playPreviewButton()->isEnabled());
    QVERIFY(!dialog->stopPreviewButton()->isEnabled());
    QVERIFY(playlists.items(playlistId).size() == 1);

    // Preview with Autoplay on: no context, lyrics only in the review screen,
    // and it stays on the previewed song when it finishes.
    QTest::mouseClick(dialog->playPreviewButton(), Qt::LeftButton);
    QVERIFY(window.isPreviewing());
    QVERIFY(QFileInfo(player.song().mp3Path).canonicalFilePath()
            == QFileInfo(unresolvedMp3).canonicalFilePath());
    QVERIFY(!window.playlistPlayback()->context());
    QTRY_COMPARE_WITH_TIMEOUT(player.state(), KaraokePlayer::State::Playing, 4000);
    QVERIFY(!window.lyricsVisible());
    QVERIFY(dialog->stopPreviewButton()->isEnabled());
    QVERIFY(dialog->previewLabel()->text().contains(QStringLiteral("3902")));
    QTRY_VERIFY_WITH_TIMEOUT(!dialog->previewView()->frame().isNull(), 4000);
    QTRY_COMPARE_WITH_TIMEOUT(player.state(), KaraokePlayer::State::Finished, 4000);
    QTest::qWait(300);
    QCOMPARE(player.state(), KaraokePlayer::State::Finished);
    QVERIFY(QFileInfo(player.song().mp3Path).canonicalFilePath()
            == QFileInfo(unresolvedMp3).canonicalFilePath());
    QVERIFY(!window.playlistPlayback()->context());
    QVERIFY(!window.isPreviewing());
    QVERIFY(!dialog->stopPreviewButton()->isEnabled());

    // A preview started while a playlist song plays drops its context, so
    // Autoplay cannot advance from the preview either.
    window.playlistView()->itemList()->setCurrentRow(0);
    QTest::mouseClick(window.playlistView()->playButton(), Qt::LeftButton);
    QVERIFY(window.playlistPlayback()->context().has_value());
    QTest::mouseClick(dialog->playPreviewButton(), Qt::LeftButton);
    QVERIFY(!window.playlistPlayback()->context());
    QTRY_COMPARE_WITH_TIMEOUT(player.state(), KaraokePlayer::State::Finished, 4000);
    QTest::qWait(300);
    QVERIFY(QFileInfo(player.song().mp3Path).canonicalFilePath()
            == QFileInfo(unresolvedMp3).canonicalFilePath());

    // Stop Preview stops cleanly; another row does not start by itself.
    dialog->songTable()->selectRow(rowFor(*dialog, QStringLiteral("SGB39/3903.mp3")));
    QTest::mouseClick(dialog->playPreviewButton(), Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(player.state(), KaraokePlayer::State::Playing, 4000);
    dialog->songTable()->selectRow(rowFor(*dialog, QStringLiteral("SGB39/3902.mp3")));
    QTest::qWait(100);
    QVERIFY(QFileInfo(player.song().mp3Path).canonicalFilePath()
            == QFileInfo(otherMp3).canonicalFilePath());
    // The main window cannot turn a preview into singing: Key/Tempo are locked
    // and Enter does not open the fullscreen lyrics.
    for (QPushButton* button : {window.keyUpButton(), window.keyDownButton(), window.keyResetButton(),
                                window.tempoUpButton(), window.tempoDownButton(), window.tempoResetButton()})
        QVERIFY(!button->isEnabled());
    QTest::keyClick(&window, Qt::Key_Return);
    QVERIFY(!window.lyricsVisible());
    // Nor can Enter in the home screen's search box or playlist replace it.
    QTreeView* results = window.libraryView()->resultsList();
    QVERIFY(results->model()->rowCount() > 0);
    results->setCurrentIndex(results->model()->index(0, 0));
    QTest::keyClick(window.libraryView()->searchBox(), Qt::Key_Return);
    window.playlistView()->itemList()->setCurrentRow(0);
    QTest::keyClick(window.playlistView()->itemList(), Qt::Key_Return);
    QVERIFY(window.isPreviewing());
    QVERIFY(!window.lyricsVisible());
    QVERIFY(!window.playlistPlayback()->context());
    QVERIFY(QFileInfo(player.song().mp3Path).canonicalFilePath()
            == QFileInfo(otherMp3).canonicalFilePath());
    QTest::mouseClick(dialog->stopPreviewButton(), Qt::LeftButton);
    QCOMPARE(player.state(), KaraokePlayer::State::Stopped);
    QVERIFY(!window.isPreviewing());
    QVERIFY(!dialog->stopPreviewButton()->isEnabled());
    QVERIFY(dialog->previewView()->frame().isNull());

    // Closing the review screen stops its preview.
    QTest::mouseClick(dialog->playPreviewButton(), Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(player.state(), KaraokePlayer::State::Playing, 4000);
    dialog->close();
    QCOMPARE(player.state(), KaraokePlayer::State::Stopped);
    QVERIFY(!window.isPreviewing());

    // Nothing was stored: no playlist items, no Key/Tempo memory, no source change.
    QCOMPARE(playlists.items(playlistId).size(), 1);
    QVERIFY(!QFileInfo::exists(settingsPath));
    QCOMPARE(tree(root), before);
    QVERIFY(window.keyUpButton()->isEnabled());  // unlocked again after the preview

    // A replacement the player rejects stops the running preview instead of
    // leaving it playing unseen, and closing the screen leaves nothing playing.
    dialog = window.openMetadataReview();
    dialog->songTable()->selectRow(rowFor(*dialog, QStringLiteral("SGB39/3903.mp3")));
    QTest::mouseClick(dialog->playPreviewButton(), Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(player.state(), KaraokePlayer::State::Playing, 4000);
    writeFile(root + QStringLiteral("/SGB39/3902.cdg"), QByteArray(2400, '\0'));
    dialog->songTable()->selectRow(rowFor(*dialog, QStringLiteral("SGB39/3902.mp3")));
    QTest::mouseClick(dialog->playPreviewButton(), Qt::LeftButton);
    QVERIFY(!window.isPreviewing());
    QVERIFY(player.state() != KaraokePlayer::State::Playing);
    QVERIFY2(dialog->previewLabel()->text().contains(QStringLiteral("could not")),
             qPrintable(dialog->previewLabel()->text()));
    dialog->close();
    QTest::qWait(100);
    QVERIFY(player.state() != KaraokePlayer::State::Playing);
}

void TestMetadataReview::detectedTitleFillsEditWithoutSaving()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("music"));
    const QString catalogue = temporary.filePath(QStringLiteral("app/library.sqlite"));
    writeFile(root + QStringLiteral("/SGB39/3902.mp3"), QByteArray(3000, 'a'));
    writeFile(root + QStringLiteral("/SGB39/3902.cdg"), QByteArray(2400, 'b'));
    writeFile(root + QStringLiteral("/SGB39/3903.mp3"), QByteArray(3100, 'c'));
    writeFile(root + QStringLiteral("/SGB39/3903.cdg"), QByteArray(2400, 'd'));
    LibraryController controller(catalogue, {}, temporary.filePath(QStringLiteral("app/overrides.sqlite")));
    QVERIFY(controller.chooseRoot(root));
    waitForIdle(controller);

    MetadataReviewDialog dialog(&controller);
    dialog.show();
    const int one = rowFor(dialog, QStringLiteral("SGB39/3902.mp3"));
    const int two = rowFor(dialog, QStringLiteral("SGB39/3903.mp3"));
    QVERIFY(one >= 0 && two >= 0);
    const qint64 oneId = dialog.songTable()->item(one, 0)->data(Qt::UserRole).toLongLong();
    const qint64 twoId = dialog.songTable()->item(two, 0)->data(Qt::UserRole).toLongLong();
    setEvidence(catalogue, oneId, ocrEvidence({QStringLiteral("Sky High")}));
    setEvidence(catalogue, twoId, ocrEvidence({QStringLiteral("Sky High"), QStringLiteral("Shy Night")}));
    // Without a player the preview controls stay disabled.
    QVERIFY(!dialog.playPreviewButton()->isEnabled());

    dialog.refresh();
    QVERIFY(dialog.selectSong(oneId));
    QVERIFY(dialog.useDetectedTitleButton()->isVisibleTo(&dialog));
    QVERIFY(dialog.detectedTitleLabel()->text().contains(QStringLiteral("Sky High")));
    QVERIFY(dialog.detailView()->toPlainText().contains(QStringLiteral("unverified title-screen reading")));
    QVERIFY(dialog.titleEdit()->text().isEmpty());
    QTest::mouseClick(dialog.useDetectedTitleButton(), Qt::LeftButton);
    QCOMPARE(dialog.titleEdit()->text(), QStringLiteral("Sky High"));
    // Copied for review only: nothing saved, the song is still unresolved.
    QVariantMap detail = controller.reviewDetail(oneId);
    QVERIFY(detail.value(QStringLiteral("manualTitle")).isNull());
    QCOMPARE(detail.value(QStringLiteral("confidence")).toString(), QStringLiteral("unresolved"));
    QVERIFY(!dialog.clearButton()->isEnabled());

    // Two different readings are ambiguous: evidence only, no shortcut.
    QVERIFY(dialog.selectSong(twoId));
    QVERIFY(!dialog.useDetectedTitleButton()->isVisibleTo(&dialog));
    QVERIFY(dialog.detailView()->toPlainText().contains(QStringLiteral("Shy Night")));
    QVERIFY(dialog.titleEdit()->text().isEmpty());
    QVERIFY(controller.reviewDetail(twoId).value(QStringLiteral("manualTitle")).isNull());
}

void TestMetadataReview::revertToAutomaticRestoresAutomaticName()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("music"));
    writeFile(root + QStringLiteral("/DK/DK001-01 - Crazy - Nelson, Willie.mp3"), QByteArray(3200, 'e'));
    writeFile(root + QStringLiteral("/DK/DK001-01 - Crazy - Nelson, Willie.cdg"), QByteArray(2400, 'f'));
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")), {},
                                 temporary.filePath(QStringLiteral("app/overrides.sqlite")));
    QVERIFY(controller.chooseRoot(root));
    waitForIdle(controller);

    MetadataReviewDialog dialog(&controller);
    dialog.show();
    dialog.filterBox()->setCurrentIndex(dialog.filterBox()->findText(QStringLiteral("All songs")));
    const int row = rowFor(dialog, QStringLiteral("DK/DK001-01 - Crazy - Nelson, Willie.mp3"));
    QVERIFY(row >= 0);
    dialog.songTable()->selectRow(row);
    const qint64 songId = dialog.selectedSongId();
    const QString autoTitle = controller.reviewDetail(songId).value(QStringLiteral("title")).toString();
    QCOMPARE(autoTitle, QStringLiteral("Crazy"));
    QCOMPARE(dialog.clearButton()->text(), QStringLiteral("Revert to Automatic"));
    QVERIFY(!dialog.clearButton()->toolTip().isEmpty());
    QVERIFY(!dialog.clearButton()->isEnabled());

    dialog.titleEdit()->setText(QStringLiteral("Crazy (Live)"));
    QTest::mouseClick(dialog.saveButton(), Qt::LeftButton);
    QVERIFY(dialog.selectSong(songId));
    QCOMPARE(controller.reviewDetail(songId).value(QStringLiteral("title")).toString(),
             QStringLiteral("Crazy (Live)"));
    QVERIFY(dialog.clearButton()->isEnabled());

    QTest::mouseClick(dialog.clearButton(), Qt::LeftButton);
    QVERIFY(dialog.selectSong(songId));
    const QVariantMap detail = controller.reviewDetail(songId);
    QCOMPARE(detail.value(QStringLiteral("title")).toString(), autoTitle);
    QVERIFY(detail.value(QStringLiteral("manualTitle")).isNull());
    QVERIFY(!dialog.clearButton()->isEnabled());
}
