#include "library/Catalogue.h"
#include "library/LibraryScanner.h"
#include "library/MetadataOverrideStore.h"
#include "library/MetadataResolver.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QTemporaryDir>
#include <QUuid>
#include <QElapsedTimer>
#include <QtTest>

#include <atomic>
#include <thread>

namespace {

void writeFile(const QString& path, const QByteArray& contents)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size())
        qFatal("Could not write synthetic fixture: %s", qPrintable(path));
}

QVariantMap runScan(const QString& databasePath, const QString& root,
                    const QString& overridesPath = {})
{
    LibraryScanner scanner(databasePath, {}, overridesPath);
    ScanOptions options;
    options.readTags = false;
    scanner.setOptions(options);
    QVariantMap summary;
    QString failure;
    QObject::connect(&scanner, &LibraryScanner::finished,
                     [&](const QVariantMap& value) { summary = value; });
    QObject::connect(&scanner, &LibraryScanner::failed,
                     [&](const QString& value) { failure = value; });
    scanner.scan(root);
    if (!failure.isEmpty())
        qFatal("Synthetic scan failed: %s", qPrintable(failure));
    return summary;
}

void createV4Database(const QString& path)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    const QString connection = QStringLiteral("v4-fixture-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(path);
        if (!database.open())
            qFatal("Could not create v4 fixture");
        QSqlQuery query(database);
        const QStringList statements = {
            QStringLiteral("CREATE TABLE library_roots(id INTEGER PRIMARY KEY,path TEXT NOT NULL UNIQUE,added_at INTEGER NOT NULL,last_scan_started INTEGER,last_scan_completed INTEGER,online INTEGER NOT NULL DEFAULT 1,active INTEGER NOT NULL DEFAULT 0)"),
            QStringLiteral("CREATE TABLE songs(id INTEGER PRIMARY KEY,title TEXT,artist TEXT,title_raw TEXT,artist_raw TEXT,disc_id TEXT,disc_prefix TEXT,track INTEGER NOT NULL DEFAULT 0,display_title TEXT,display_artist TEXT,search_text TEXT NOT NULL DEFAULT '',metadata_source TEXT NOT NULL DEFAULT 'fallback',confidence TEXT NOT NULL DEFAULT 'none',best_source_id INTEGER,playable INTEGER NOT NULL DEFAULT 0)"),
            QStringLiteral("INSERT INTO songs(id,title,artist,display_title,display_artist,search_text,confidence) VALUES(42,'Old Title','Old Artist','Old Title','Old Artist','old title old artist','none')"),
            QStringLiteral("PRAGMA user_version=4")};
        for (const QString& statement : statements) {
            if (!query.exec(statement))
                qFatal("Could not create v4 fixture statement: %s",
                       qPrintable(query.lastError().text()));
        }
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
}

QString rawSnapshot(const QString& path)
{
    const QString connection = QStringLiteral("raw-snapshot-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QStringList rows;
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(path);
        if (!database.open())
            qFatal("Could not open raw snapshot database");
        const QStringList statements = {
            QStringLiteral("SELECT id,rel_path,rel_dir,file_name,size,mtime_ms,COALESCE(raw_tags_json,'') FROM files ORDER BY id"),
            QStringLiteral("SELECT id,zip_file_id,name,method,crc32,compressed_size,uncompressed_size,encrypted,damaged,kind FROM zip_members ORDER BY id"),
            QStringLiteral("SELECT id,parsed_json FROM sources ORDER BY id"),
            QStringLiteral("SELECT id FROM songs ORDER BY id")};
        for (const QString& statement : statements) {
            QSqlQuery query(database);
            if (!query.exec(statement))
                qFatal("Could not snapshot raw catalogue layer");
            while (query.next()) {
                QStringList values;
                for (int column = 0; column < query.record().count(); ++column)
                    values.append(query.value(column).toString());
                rows.append(values.join(QChar(0x1f)));
            }
            rows.append(QStringLiteral("--"));
        }
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
    return rows.join(QLatin1Char('\n'));
}

QVariantList songLayer(const QString& path, qint64 songId)
{
    const QString connection = QStringLiteral("song-layer-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QVariantList values;
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(path);
        if (!database.open())
            qFatal("Could not inspect song layer");
        QSqlQuery query(database);
        query.prepare(QStringLiteral(
            "SELECT auto_title,auto_artist,auto_source,auto_confidence,resolver_version,"
            "evidence_json,conflict,manual_title,manual_artist,title,artist,metadata_source,"
            "confidence,display_title,display_artist FROM songs WHERE id=?"));
        query.addBindValue(songId);
        if (!query.exec() || !query.next())
            qFatal("Could not inspect resolved song layer");
        for (int column = 0; column < query.record().count(); ++column)
            values.append(query.value(column));
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
    return values;
}

} // namespace

class TestMetadataFoundation : public QObject {
    Q_OBJECT

private slots:
    void migrationCreatesBackupAndPreservesRows();
    void backupFailureRefusesMigration();
    void layeredMetadataSearchAndImmediateOverrides();
    void overrideStoreSafetyRecoveryAndCatalogueRebuild();
    void cancellableDatabaseOnlyReprocessPreservesRawLayer();
    void trustedImportsSurviveReprocessing();
    void unsafeLegacyCatalogueIsNotTouched();
    void pausedScanDoesNotBlockCorrections();
    void emptyOverrideStoreNeverErasesCorrections();
    void overrideStoreMigratesFromVersionOne();
    void overridesFollowAMovedMusicFolder();
    void movesNeedTheSameSongAndClearsAreAllOrNothing();
};

void TestMetadataFoundation::migrationCreatesBackupAndPreservesRows()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path = temporary.filePath(QStringLiteral("app/library.sqlite"));
    createV4Database(path);

    Catalogue catalogue(path);
    QString error;
    QVERIFY2(catalogue.open(&error), qPrintable(error));
    catalogue.close();

    const QStringList backups = QDir(QFileInfo(path).absolutePath()).entryList(
        {QStringLiteral("library.sqlite.pre-v5-*.bak")}, QDir::Files);
    QCOMPARE(backups.size(), 1);
    const QString connection = QStringLiteral("verify-migration-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(path);
        QVERIFY(database.open());
        QSqlQuery query(database);
        QVERIFY(query.exec(QStringLiteral("PRAGMA user_version")) && query.next());
        QCOMPARE(query.value(0).toInt(), 5);
        QVERIFY(query.exec(QStringLiteral(
            "SELECT id,title,artist,confidence,resolver_version,manual_title FROM songs"))
            && query.next());
        QCOMPARE(query.value(0).toLongLong(), 42LL);
        QCOMPARE(query.value(1).toString(), QStringLiteral("Old Title"));
        QCOMPARE(query.value(2).toString(), QStringLiteral("Old Artist"));
        QCOMPARE(query.value(3).toString(), QStringLiteral("unresolved"));
        QCOMPARE(query.value(4).toInt(), 0);
        QVERIFY(query.value(5).isNull());
        QVERIFY(query.exec(QStringLiteral(
            "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='catalogue_meta'"))
            && query.next());
        QCOMPARE(query.value(0).toInt(), 1);
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
}

void TestMetadataFoundation::backupFailureRefusesMigration()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path = temporary.filePath(QStringLiteral("app/library.sqlite"));
    createV4Database(path);
    const QDateTime now = QDateTime::currentDateTime();
    for (int offset = 0; offset <= 1; ++offset) {
        const QString blocked = path + QStringLiteral(".pre-v5-")
            + now.addSecs(offset).toString(QStringLiteral("yyyyMMdd-HHmmss"))
            + QStringLiteral(".bak");
        QVERIFY(QDir().mkpath(blocked));
    }

    Catalogue catalogue(path);
    QString error;
    QVERIFY(!catalogue.open(&error));
    QVERIFY2(error.contains(QStringLiteral("backup"), Qt::CaseInsensitive), qPrintable(error));

    const QString connection = QStringLiteral("verify-refusal-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(path);
        QVERIFY(database.open());
        QSqlQuery query(database);
        QVERIFY(query.exec(QStringLiteral("PRAGMA user_version")) && query.next());
        QCOMPARE(query.value(0).toInt(), 4);
        QVERIFY(query.exec(QStringLiteral("PRAGMA table_info(songs)")));
        bool foundAutoTitle = false;
        while (query.next())
            foundAutoTitle = foundAutoTitle || query.value(1).toString() == QLatin1String("auto_title");
        QVERIFY(!foundAutoTitle);
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
}

void TestMetadataFoundation::layeredMetadataSearchAndImmediateOverrides()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("library"));
    const QString path = temporary.filePath(QStringLiteral("app/library.sqlite"));
    const QString named = QStringLiteral("SGB39/Deep_Folder/SGB39-02 - Sinatra, Frank - My_Way");
    const QString numeric = QStringLiteral("SGB39/3905");
    for (const QString& stem : {named, numeric}) {
        writeFile(root + QLatin1Char('/') + stem + QStringLiteral(".mp3"), QByteArray("audio"));
        writeFile(root + QLatin1Char('/') + stem + QStringLiteral(".cdg"), QByteArray("lyrics"));
    }
    QCOMPARE(runScan(path, root).value(QStringLiteral("status")).toString(),
             QStringLiteral("completed"));

    Catalogue catalogue(path);
    QString error;
    QVERIFY2(catalogue.open(&error), qPrintable(error));
    const QList<CatalogueSearchRow> namedRows = catalogue.search(
        QStringLiteral("my way"), 10, true, &error);
    QCOMPARE(namedRows.size(), 1);
    const qint64 songId = namedRows.first().songId;
    const QVariantList layer = songLayer(path, songId);
    QCOMPARE(layer.at(0).toString(), QStringLiteral("My Way"));
    QCOMPARE(layer.at(1).toString(), QStringLiteral("Sinatra, Frank"));
    // One personal-name field decides the order for a lone song (Milestone 4 rule).
    QCOMPARE(layer.at(2).toString(), QStringLiteral("filename"));
    QVERIFY(layer.at(3).toString() == QLatin1String("low")
            || layer.at(3).toString() == QLatin1String("medium")
            || layer.at(3).toString() == QLatin1String("high"));
    QCOMPARE(layer.at(4).toInt(), MetadataResolver::Version);
    const QJsonObject evidence = QJsonDocument::fromJson(layer.at(5).toByteArray()).object();
    QCOMPARE(evidence.value(QStringLiteral("v")).toInt(), 5);
    QCOMPARE(evidence.value(QStringLiteral("rule")).toString(),
             QStringLiteral("filename_evidence"));
    QCOMPARE(layer.at(14).toString(), QStringLiteral("Frank Sinatra"));

    QCOMPARE(catalogue.search(QStringLiteral("frank sinatra"), 10, true, &error).size(), 1);
    QCOMPARE(catalogue.search(QStringLiteral("deep folder"), 10, true, &error).size(), 1);
    QCOMPARE(catalogue.search(QStringLiteral("sgb39 02"), 10, true, &error).size(), 1);
    QCOMPARE(catalogue.search(QStringLiteral("sgb3902"), 10, true, &error).size(), 1);
    QCOMPARE(catalogue.search(QStringLiteral("sgb39"), 10, true, &error).size(), 2);
    // The unresolved numeric file stays findable by its raw name and fallback.
    QCOMPARE(catalogue.search(QStringLiteral("3905"), 10, true, &error).size(), 1);
    QCOMPARE(catalogue.search(QStringLiteral("disc sgb39 track 05"), 10, true,
                              &error).size(), 1);

    const auto before = catalogue.songRef(songId, &error);
    QVERIFY(before);
    const QVariantList automatic = songLayer(path, songId);
    QVERIFY2(catalogue.setManualOverride(songId, QStringLiteral("Francis Albert Sinatra"),
                                         QStringLiteral("My Way (Corrected)"), 1234, &error),
             qPrintable(error));
    QCOMPARE(catalogue.search(QStringLiteral("francis albert corrected"), 10, true,
                              &error).size(), 1);
    QCOMPARE(catalogue.search(QStringLiteral("deep folder sgb3902"), 10, true,
                              &error).size(), 1);
    const auto changed = catalogue.songRef(songId, &error);
    QVERIFY(changed);
    QCOMPARE(changed->songId, before->songId);
    QCOMPARE(changed->rootPath, before->rootPath);
    QCOMPARE(changed->mp3RelPath, before->mp3RelPath);
    QVERIFY2(MetadataResolver::resolve(catalogue, -1, &error), qPrintable(error));
    QVariantList manualLayer = songLayer(path, songId);
    QCOMPARE(manualLayer.at(9).toString(), QStringLiteral("My Way (Corrected)"));
    QCOMPARE(manualLayer.at(10).toString(), QStringLiteral("Francis Albert Sinatra"));
    QCOMPARE(manualLayer.at(11).toString(), QStringLiteral("manual"));
    QCOMPARE(manualLayer.at(12).toString(), QStringLiteral("high"));
    // What Frankie sees follows the correction even after a plain resolve.
    QCOMPARE(manualLayer.at(13).toString(), QStringLiteral("My Way (Corrected)"));
    QCOMPARE(manualLayer.at(14).toString(), QStringLiteral("Francis Albert Sinatra"));
    QVERIFY2(catalogue.clearManualOverride(songId, &error), qPrintable(error));
    const QVariantList cleared = songLayer(path, songId);
    QVERIFY(cleared.at(7).isNull());
    QVERIFY(cleared.at(8).isNull());
    // Clearing restores exactly the automatic presentation, including the
    // "Last, First" display rule.
    for (int column = 9; column <= 14; ++column)
        QCOMPARE(cleared.at(column), automatic.at(column));
    QCOMPARE(catalogue.search(QStringLiteral("3905"), 10, true, &error).size(), 1);
    QCOMPARE(catalogue.search(QStringLiteral("sinatra my way"), 10, true, &error).size(), 1);
}

void TestMetadataFoundation::overrideStoreSafetyRecoveryAndCatalogueRebuild()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("library"));
    const QString path = temporary.filePath(QStringLiteral("app/library.sqlite"));
    const QString overridesPath = temporary.filePath(QStringLiteral("app/metadata-overrides.sqlite"));
    writeFile(root + QStringLiteral("/SGB39/3902.mp3"), QByteArray("audio"));
    writeFile(root + QStringLiteral("/SGB39/3902.cdg"), QByteArray("lyrics"));

    MetadataOverrideStore unsafe(root + QStringLiteral("/metadata-overrides.sqlite"));
    QString error;
    QVERIFY(!unsafe.open(&error, {root}));
    QVERIFY(error.contains(QStringLiteral("must not be inside")));

    const QString corruptPath = temporary.filePath(QStringLiteral("app/corrupt-overrides.sqlite"));
    writeFile(corruptPath, QByteArray("not sqlite"));
    MetadataOverrideStore recovered(corruptPath);
    error.clear();
    QVERIFY2(recovered.open(&error, {root}), qPrintable(error));
    recovered.close();
    QCOMPARE(QDir(QFileInfo(corruptPath).absolutePath()).entryList(
                 {QStringLiteral("corrupt-overrides.corrupt-*.sqlite")}, QDir::Files).size(), 1);

    QCOMPARE(runScan(path, root, overridesPath).value(QStringLiteral("status")).toString(),
             QStringLiteral("completed"));
    Catalogue catalogue(path);
    QVERIFY2(catalogue.open(&error), qPrintable(error));
    const QList<CatalogueSearchRow> rawRows = catalogue.search(
        QStringLiteral("3902"), 10, true, &error);
    QCOMPARE(rawRows.size(), 1);
    auto value = catalogue.metadataOverrideSnapshot(rawRows.first().songId, &error);
    QVERIFY(value);
    value->artist = QStringLiteral("Frank Sinatra");
    value->title = QStringLiteral("My Way");
    value->updatedAt = QDateTime::currentMSecsSinceEpoch();
    MetadataOverrideStore store(overridesPath);
    QVERIFY2(store.open(&error, {root}), qPrintable(error));
    QVERIFY2(store.setOverride(*value, &error), qPrintable(error));
    QVERIFY2(catalogue.applyManualOverrides(store.all(&error), &error), qPrintable(error));
    QCOMPARE(catalogue.search(QStringLiteral("frank sinatra my way"), 10, true,
                              &error).size(), 1);
    catalogue.close();
    store.close();

    QVERIFY(QFile::remove(path));
    QFile::remove(path + QStringLiteral("-wal"));
    QFile::remove(path + QStringLiteral("-shm"));
    QCOMPARE(runScan(path, root, overridesPath).value(QStringLiteral("status")).toString(),
             QStringLiteral("completed"));
    QVERIFY2(catalogue.open(&error), qPrintable(error));
    QCOMPARE(catalogue.search(QStringLiteral("frank sinatra my way"), 10, true,
                              &error).size(), 1);
    QCOMPARE(catalogue.search(QStringLiteral("3902 sgb39 02"), 10, true,
                              &error).size(), 1);
}

void TestMetadataFoundation::cancellableDatabaseOnlyReprocessPreservesRawLayer()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("library"));
    const QString offline = temporary.filePath(QStringLiteral("library-offline"));
    const QString path = temporary.filePath(QStringLiteral("app/library.sqlite"));
    for (int i = 1; i <= 510; ++i) {
        const QString stem = QStringLiteral("RP100-%1 - Artist %1 - Title %1")
                                 .arg(i, 3, 10, QLatin1Char('0'));
        writeFile(root + QLatin1Char('/') + stem + QStringLiteral(".mp3"), QByteArray("a"));
        writeFile(root + QLatin1Char('/') + stem + QStringLiteral(".cdg"), QByteArray("g"));
    }
    QCOMPARE(runScan(path, root).value(QStringLiteral("status")).toString(),
             QStringLiteral("completed"));
    const QString before = rawSnapshot(path);
    QVERIFY(QDir().rename(root, offline));

    LibraryScanner scanner(path);
    bool cancelRequested = false;
    QVariantMap first;
    QString failure;
    QObject::connect(&scanner, &LibraryScanner::progress,
                     [&](const QString& phase, qint64 done, qint64, const QString&) {
        if (!cancelRequested && phase == QLatin1String("metadata_reprocess")
            && done >= 500) {
            cancelRequested = true;
            scanner.requestCancel();
        }
    });
    QObject::connect(&scanner, &LibraryScanner::finished,
                     [&](const QVariantMap& summary) { first = summary; });
    QObject::connect(&scanner, &LibraryScanner::failed,
                     [&](const QString& message) { failure = message; });
    scanner.prepareScan();
    scanner.reprocessMetadata();
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
    QCOMPARE(first.value(QStringLiteral("status")).toString(), QStringLiteral("cancelled"));
    QCOMPARE(first.value(QStringLiteral("sourceFileReads")).toULongLong(), 0ULL);

    QVariantMap second;
    QObject::connect(&scanner, &LibraryScanner::finished,
                     [&](const QVariantMap& summary) { second = summary; });
    scanner.prepareScan();
    scanner.reprocessMetadata();
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
    QCOMPARE(second.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
    QCOMPARE(second.value(QStringLiteral("sourceFileReads")).toULongLong(), 0ULL);
    QCOMPARE(rawSnapshot(path), before);

    Catalogue catalogue(path);
    QString error;
    QVERIFY2(catalogue.open(&error), qPrintable(error));
    QCOMPARE(catalogue.catalogueMeta(QStringLiteral("resolver_version")),
             QString::number(MetadataResolver::Version));
    QCOMPARE(catalogue.catalogueMeta(QStringLiteral("reprocess_pending")),
             QStringLiteral("0"));
    QCOMPARE(catalogue.metadataStats(&error)
                 .value(QStringLiteral("songsAtOldResolverVersion")).toLongLong(), 0LL);
    catalogue.close();
    QVERIFY(QDir().rename(offline, root));
}

void TestMetadataFoundation::unsafeLegacyCatalogueIsNotTouched()
{
    // A v4 catalogue that sits inside its own recorded music folder (possible
    // only from old configurations) must be refused before anything is
    // written: no backup, no migration.
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString music = temporary.filePath(QStringLiteral("music"));
    const QString path = music + QStringLiteral("/library.sqlite");
    createV4Database(path);
    const QString connection = QStringLiteral("unsafe-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(path);
        QVERIFY(database.open());
        QSqlQuery query(database);
        QVERIFY(query.exec(QStringLiteral("DELETE FROM library_roots")));
        query.prepare(QStringLiteral("INSERT INTO library_roots(path,added_at,online,active) VALUES(?,0,1,1)"));
        query.addBindValue(Catalogue::canonicalPath(music));
        QVERIFY(query.exec());
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
    const QStringList before = QDir(music).entryList(QDir::Files | QDir::Hidden);
    Catalogue catalogue(path);
    QString error;
    QVERIFY(!catalogue.open(&error));
    QVERIFY2(error.contains(QStringLiteral("must not be inside")), qPrintable(error));
    QCOMPARE(QDir(music).entryList(QDir::Files | QDir::Hidden), before);
}

void TestMetadataFoundation::pausedScanDoesNotBlockCorrections()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("library"));
    const QString path = temporary.filePath(QStringLiteral("app/library.sqlite"));
    // Enough files that the walk is still inside a write batch when the
    // (100 ms throttled) progress report arrives.
    for (int i = 1; i <= 4000; ++i) {
        const QString stem = root + QStringLiteral("/PX%1/PX%1-01 - Singer %2 - Song %2")
                                        .arg(i / 100 + 1, 3, 10, QLatin1Char('0'))
                                        .arg(i, 4, 10, QLatin1Char('0'));
        writeFile(stem + QStringLiteral(".mp3"), QByteArray("a"));
        writeFile(stem + QStringLiteral(".cdg"), QByteArray("g"));
    }
    QCOMPARE(runScan(path, root).value(QStringLiteral("status")).toString(), QStringLiteral("completed"));

    // A rescan pauses for playback part-way through its walk; meanwhile a
    // correction is saved on another connection, as the review screen would.
    LibraryScanner scanner(path);
    ScanOptions options;
    options.readTags = false;
    scanner.setOptions(options);
    std::atomic_bool paused = false;
    std::atomic_bool written = false;
    qint64 waitedMs = -1;
    QString writeError;
    std::thread writer;
    QObject::connect(&scanner, &LibraryScanner::progress,
                     [&](const QString& phase, qint64 done, qint64, const QString&) {
        if (paused.load() || phase != QLatin1String("walk") || done < 100 || done % 500 == 0)
            return;
        paused = true;
        scanner.setPaused(true);
        writer = std::thread([&] {
            Catalogue catalogue(path);
            QString error;
            QElapsedTimer timer;
            timer.start();
            if (catalogue.open(&error)) {
                const QList<CatalogueSearchRow> rows = catalogue.search(QStringLiteral("song 0007"), 5, true, &error);
                if (!rows.isEmpty())
                    written = catalogue.setManualOverride(rows.first().songId, QStringLiteral("Fixed"),
                                                          QStringLiteral("Corrected Song"), 0, &error);
                catalogue.close();
            }
            waitedMs = timer.elapsed();
            writeError = error;
            scanner.setPaused(false);
        });
    });
    scanner.scan(root);
    writer.join();
    QVERIFY(paused.load());
    QVERIFY2(written.load(), qPrintable(writeError));
    QVERIFY2(waitedMs < 4000, qPrintable(QString::number(waitedMs)));
}

void TestMetadataFoundation::emptyOverrideStoreNeverErasesCorrections()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("library"));
    const QString path = temporary.filePath(QStringLiteral("app/library.sqlite"));
    const QString overridesPath = temporary.filePath(QStringLiteral("app/metadata-overrides.sqlite"));
    writeFile(root + QStringLiteral("/SGB39/3902.mp3"), QByteArray("audio"));
    writeFile(root + QStringLiteral("/SGB39/3902.cdg"), QByteArray("lyrics"));
    QCOMPARE(runScan(path, root, overridesPath).value(QStringLiteral("status")).toString(),
             QStringLiteral("completed"));
    QString error;
    {
        Catalogue catalogue(path);
        QVERIFY2(catalogue.open(&error), qPrintable(error));
        const qint64 songId = catalogue.search(QStringLiteral("3902"), 5, true, &error).first().songId;
        QVERIFY2(catalogue.setManualOverride(songId, QStringLiteral("Frank Sinatra"),
                                             QStringLiteral("My Way"), 0, &error), qPrintable(error));
    }
    // The store holds nothing (lost, or replaced after damage): the worker's
    // sync must keep the catalogue's corrections, not clear them.
    QVERIFY(!QFileInfo::exists(overridesPath) || QFile::remove(overridesPath));
    QCOMPARE(runScan(path, root, overridesPath).value(QStringLiteral("status")).toString(),
             QStringLiteral("completed"));
    Catalogue catalogue(path);
    QVERIFY2(catalogue.open(&error), qPrintable(error));
    QCOMPARE(catalogue.search(QStringLiteral("frank sinatra my way"), 5, true, &error).size(), 1);
}

void TestMetadataFoundation::overridesFollowAMovedMusicFolder()
{
    // The same collection at three folders, all still listed as present in
    // the catalogue (old drive letters are never rescanned); B is in use.
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString rootA = temporary.filePath(QStringLiteral("music-a"));
    const QString rootB = temporary.filePath(QStringLiteral("music-b"));
    const QString rootC = temporary.filePath(QStringLiteral("music-c"));
    const QString path = temporary.filePath(QStringLiteral("app/library.sqlite"));
    const QString overridesPath = temporary.filePath(QStringLiteral("app/metadata-overrides.sqlite"));
    const QString named = QStringLiteral("SGB39/SGB39-02 - Sinatra, Frank - My Way.mp3");
    const QString other = QStringLiteral("SGB39/SGB39-03 - Sinatra, Frank - New York.mp3");
    for (const QString& root : {rootA, rootB, rootC}) {
        for (const QString& mp3 : {named, other}) {
            writeFile(root + QLatin1Char('/') + mp3, QByteArray("audio"));
            writeFile(root + QLatin1Char('/') + mp3.chopped(4) + QStringLiteral(".cdg"),
                      QByteArray("lyrics"));
        }
        QCOMPARE(runScan(path, root).value(QStringLiteral("status")).toString(),
                 QStringLiteral("completed"));
    }
    Catalogue catalogue(path);
    QString error;
    QVERIFY2(catalogue.open(&error), qPrintable(error));
    const QString canonicalA = Catalogue::canonicalPath(rootA);
    const QString canonicalB = Catalogue::canonicalPath(rootB);
    auto activate = [&](const QString& root) {
        for (const CatalogueRoot& known : catalogue.roots(&error)) {
            if (known.path == Catalogue::canonicalPath(root))
                return catalogue.setActiveRoot(known.id, &error);
        }
        return false;
    };
    QVERIFY2(activate(rootB), qPrintable(error));
    const qint64 songA = catalogue.findSongByMp3Path(rootA, named);
    const qint64 songB = catalogue.findSongByMp3Path(rootB, named);
    const qint64 otherA = catalogue.findSongByMp3Path(rootA, other);
    const qint64 otherB = catalogue.findSongByMp3Path(rootB, other);
    QVERIFY(songA > 0 && songB > 0 && songA != songB && otherA > 0 && otherB > 0);
    const QString automaticTitle = catalogue.songRef(songB)->title;
    const QString automaticOther = catalogue.songRef(otherB)->title;
    auto titleOf = [&](qint64 song) { return catalogue.songRef(song)->title; };

    MetadataOverrideStore store(overridesPath);
    QVERIFY2(store.open(&error, {rootA, rootB, rootC}), qPrintable(error));
    auto storedTitle = [&](const QString& root, const QString& relPath) {
        const auto value = store.overrideFor(root, relPath, &error);
        return value && value->title ? *value->title : QString();
    };
    int copies = 0;
    const Catalogue::CopyOverride copy = [&](const MovedMetadataOverride& move) {
        ++copies;
        return store.copyOverride(move, &error);
    };
    QList<MetadataOverride> replaced;
    auto sync = [&] {
        copies = 0;
        return catalogue.applyManualOverrides(store.all(&error), &error, copy, &replaced)
            && store.removeOverrides(replaced, &error);
    };

    auto value = catalogue.metadataOverrideSnapshot(songA, &error);
    QVERIFY(value);
    QCOMPARE(value->discId, QStringLiteral("SGB39"));
    value->artist = QStringLiteral("Francis Albert Sinatra");
    value->title = QStringLiteral("From A");
    value->label = QStringLiteral("Sunfly");
    value->series = QStringLiteral("Gold");
    value->trustedDiscId = QStringLiteral("SF001");
    value->trustedTrack = 7;
    value->originalKey = 3;
    value->origin = QStringLiteral("import");
    value->createdAt = 50;
    value->updatedAt = 100;
    QVERIFY2(store.setOverride(*value, &error), qPrintable(error));
    // A file at the same path whose automatic disc/track do not match what
    // the correction was made for is not the same song.
    auto unsafe = catalogue.metadataOverrideSnapshot(otherA, &error);
    QVERIFY(unsafe);
    unsafe->discId = QStringLiteral("NOPE01");
    unsafe->title = QStringLiteral("Unsafe");
    unsafe->updatedAt = 100;
    QVERIFY2(store.setOverride(*unsafe, &error), qPrintable(error));

    // A copy that cannot be made leaves every value where it was.
    QVERIFY2(catalogue.applyManualOverrides(store.all(&error), &error,
                                            [](const MovedMetadataOverride&) { return false; },
                                            &replaced), qPrintable(error));
    QVERIFY(replaced.isEmpty());
    QCOMPARE(titleOf(songB), automaticTitle);
    QCOMPARE(titleOf(songA), QStringLiteral("From A"));

    // Moved: copied to B with every value and timestamp, then removed at A.
    QVERIFY2(sync(), qPrintable(error));
    QCOMPARE(copies, 1);
    QCOMPARE(replaced.size(), 1);
    QCOMPARE(titleOf(songB), QStringLiteral("From A"));
    QCOMPARE(titleOf(otherB), automaticOther);
    QCOMPARE(titleOf(otherA), QStringLiteral("Unsafe"));
    QVERIFY(!store.overrideFor(rootA, named, &error));
    QVERIFY(store.overrideFor(rootA, other, &error));
    const auto atB = store.overrideFor(rootB, named, &error);
    QVERIFY(atB);
    QCOMPARE(atB->rootPath, canonicalB);
    QCOMPARE(atB->artist, value->artist);
    QCOMPARE(atB->title, value->title);
    QCOMPARE(atB->label, value->label);
    QCOMPARE(atB->series, value->series);
    QCOMPARE(atB->trustedDiscId, value->trustedDiscId);
    QCOMPARE(atB->trustedTrack, value->trustedTrack);
    QCOMPARE(atB->originalKey, value->originalKey);
    QCOMPARE(atB->origin, value->origin);
    QCOMPARE(atB->createdAt, 50LL);
    QCOMPARE(atB->updatedAt, 100LL);
    QCOMPARE(atB->discId, value->discId);
    QCOMPARE(atB->track, value->track);
    QCOMPARE(atB->autoTitle, value->autoTitle);
    QCOMPARE(atB->fileName, value->fileName);
    QVERIFY2(sync(), qPrintable(error));  // settled: nothing more moves
    QCOMPARE(copies, 0);
    QVERIFY(replaced.isEmpty());
    QCOMPARE(store.all(&error).size(), 2);

    // A copy never replaces a row already at the new identity, nor copies a
    // row that changed since it was read; a removal skips a changed row.
    MetadataOverride stale = *value;
    stale.title = QStringLiteral("Old A");
    stale.updatedAt = 200;
    QVERIFY2(store.setOverride(stale, &error), qPrintable(error));
    QVERIFY(!store.copyOverride({stale, canonicalB, named}, &error));
    QVERIFY(!store.copyOverride({*value, canonicalB, named}, &error));
    QVERIFY2(store.removeOverrides({*value}, &error), qPrintable(error));
    QCOMPARE(storedTitle(rootA, named), QStringLiteral("Old A"));
    QCOMPARE(storedTitle(rootB, named), QStringLiteral("From A"));

    // A song's own value wins over one that would move onto it (even a newer
    // one, e.g. re-entered before this fix), and the superseded row goes, so
    // clearing the song's own value cannot bring it back.
    QVERIFY2(sync(), qPrintable(error));
    QCOMPARE(copies, 0);
    QCOMPARE(titleOf(songB), QStringLiteral("From A"));
    QVERIFY(!store.overrideFor(rootA, named, &error));
    QVERIFY(store.clearOverride(rootB, named, &error));
    QVERIFY2(sync(), qPrintable(error));
    QCOMPARE(titleOf(songB), automaticTitle);

    // Two old folders: the newer value moves, the older one goes.
    MetadataOverride fromA = *value;
    fromA.title = QStringLiteral("Older at A");
    fromA.updatedAt = 300;
    QVERIFY2(store.setOverride(fromA, &error), qPrintable(error));
    MetadataOverride fromC = *catalogue.metadataOverrideSnapshot(
        catalogue.findSongByMp3Path(rootC, named), &error);
    fromC.title = QStringLiteral("Newer at C");
    fromC.updatedAt = 400;
    QVERIFY2(store.setOverride(fromC, &error), qPrintable(error));
    QVERIFY2(sync(), qPrintable(error));
    QCOMPARE(copies, 1);
    QCOMPARE(titleOf(songB), QStringLiteral("Newer at C"));
    QCOMPARE(storedTitle(rootB, named), QStringLiteral("Newer at C"));
    QVERIFY(!store.overrideFor(rootA, named, &error));
    QVERIFY(!store.overrideFor(rootC, named, &error));

    // A is in use again: the value follows the song back.
    QVERIFY2(activate(rootA), qPrintable(error));
    QVERIFY2(sync(), qPrintable(error));
    QCOMPARE(titleOf(songA), QStringLiteral("Newer at C"));
    QCOMPARE(titleOf(songB), automaticTitle);
    QCOMPARE(titleOf(otherA), QStringLiteral("Unsafe"));
    QCOMPARE(storedTitle(canonicalA, named), QStringLiteral("Newer at C"));
    QVERIFY(!store.overrideFor(rootB, named, &error));
    QCOMPARE(store.all(&error).size(), 2);
}

void TestMetadataFoundation::movesNeedTheSameSongAndClearsAreAllOrNothing()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString rootA = temporary.filePath(QStringLiteral("music-a"));
    const QString rootB = temporary.filePath(QStringLiteral("music-b"));
    const QString path = temporary.filePath(QStringLiteral("app/library.sqlite"));
    const QString overridesPath = temporary.filePath(QStringLiteral("app/metadata-overrides.sqlite"));
    const QString named = QStringLiteral("SGB39/SGB39-02 - Sinatra, Frank - My Way.mp3");
    for (const QString& root : {rootA, rootB}) {
        writeFile(root + QLatin1Char('/') + named, QByteArray("audio"));
        writeFile(root + QLatin1Char('/') + named.chopped(4) + QStringLiteral(".cdg"), QByteArray("lyrics"));
        QCOMPARE(runScan(path, root).value(QStringLiteral("status")).toString(),
                 QStringLiteral("completed"));
    }
    Catalogue catalogue(path);
    QString error;
    QVERIFY2(catalogue.open(&error), qPrintable(error));
    for (const CatalogueRoot& root : catalogue.roots(&error)) {
        if (root.path == Catalogue::canonicalPath(rootB))
            QVERIFY2(catalogue.setActiveRoot(root.id, &error), qPrintable(error));
    }
    const qint64 songA = catalogue.findSongByMp3Path(rootA, named);
    const qint64 songB = catalogue.findSongByMp3Path(rootB, named);
    QVERIFY(songA > 0 && songB > 0);
    const QString automaticTitle = catalogue.songRef(songB)->title;
    MetadataOverrideStore store(overridesPath);
    QVERIFY2(store.open(&error, {rootA, rootB}), qPrintable(error));
    const Catalogue::CopyOverride copy = [&](const MovedMetadataOverride& move) {
        return store.copyOverride(move, &error);
    };
    QList<MetadataOverride> replaced;

    // Without a disc number, the same title by another artist is another song.
    auto value = catalogue.metadataOverrideSnapshot(songA, &error);
    QVERIFY(value && !value->autoTitle.isEmpty());
    value->discId.clear();
    value->track = 0;
    value->autoArtist = QStringLiteral("Someone Else");
    value->title = QStringLiteral("Corrected");
    value->updatedAt = 100;
    QVERIFY2(store.setOverride(*value, &error), qPrintable(error));
    QVERIFY2(catalogue.applyManualOverrides(store.all(&error), &error, copy, &replaced), qPrintable(error));
    QVERIFY(replaced.isEmpty());
    QCOMPARE(catalogue.songRef(songB)->title, automaticTitle);
    QCOMPARE(catalogue.songRef(songA)->title, QStringLiteral("Corrected"));
    QVERIFY(!store.overrideFor(rootB, named, &error));
    QVERIFY(catalogue.movedCopiesOf(songB, store.all(&error), &error).isEmpty());
    // The same title and artist (in any case) is the same song.
    value->autoArtist = catalogue.metadataOverrideSnapshot(songA, &error)->autoArtist.toUpper();
    value->updatedAt = 200;
    QVERIFY2(store.setOverride(*value, &error), qPrintable(error));
    QCOMPARE(catalogue.movedCopiesOf(songB, store.all(&error), &error).size(), 1);
    QVERIFY2(catalogue.applyManualOverrides(store.all(&error), &error, copy, &replaced), qPrintable(error));
    QCOMPARE(replaced.size(), 1);
    QCOMPARE(catalogue.songRef(songB)->title, QStringLiteral("Corrected"));

    // The copy at A is left behind (its tidy-up has not run). Clearing B
    // removes it too, but only all together and only if it is unchanged.
    QVERIFY(store.overrideFor(rootA, named, &error));
    const QList<MetadataOverride> copies = catalogue.movedCopiesOf(songB, store.all(&error), &error);
    QCOMPARE(copies.size(), 1);
    MetadataOverride changed = copies.first();
    changed.updatedAt = 1;
    QVERIFY(!store.clearOverrideAndCopies(rootB, named, {changed}, &error));
    QVERIFY(store.overrideFor(rootA, named, &error));
    QVERIFY(store.overrideFor(rootB, named, &error));
    const QString connection = QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        QSqlDatabase raw = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        raw.setDatabaseName(overridesPath);
        QVERIFY(raw.open());
        QSqlQuery trigger(raw);
        QVERIFY(trigger.exec(QStringLiteral(
            "CREATE TRIGGER test_keep_own BEFORE DELETE ON metadata_overrides WHEN old.root_path='%1' "
            "BEGIN SELECT RAISE(ABORT,'forced failure'); END").arg(Catalogue::canonicalPath(rootB))));
        QVERIFY(!store.clearOverrideAndCopies(rootB, named, copies, &error));
        QVERIFY(store.overrideFor(rootA, named, &error));  // rolled back
        QVERIFY(store.overrideFor(rootB, named, &error));
        QVERIFY(trigger.exec(QStringLiteral("DROP TRIGGER test_keep_own")));
        raw.close();
    }
    QSqlDatabase::removeDatabase(connection);
    QVERIFY2(store.clearOverrideAndCopies(rootB, named, copies, &error), qPrintable(error));
    QVERIFY(store.all(&error).isEmpty());
}

QTEST_GUILESS_MAIN(TestMetadataFoundation)
void TestMetadataFoundation::trustedImportsSurviveReprocessing()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("library"));
    const QString path = temporary.filePath(QStringLiteral("app/library.sqlite"));
    const QString overridesPath = temporary.filePath(QStringLiteral("app/metadata-overrides.sqlite"));
    // A newly added song with an unhelpful name, and an old one to relabel.
    writeFile(root + QStringLiteral("/New Songs/new-01.mp3"), QByteArray("audio-new"));
    writeFile(root + QStringLiteral("/New Songs/new-01.cdg"), QByteArray("lyrics-new"));
    writeFile(root + QStringLiteral("/Sunfly/SF123-04 - Johnny Cash - Ring Of Fire.mp3"), QByteArray("a1"));
    writeFile(root + QStringLiteral("/Sunfly/SF123-04 - Johnny Cash - Ring Of Fire.cdg"), QByteArray("c1"));

    // The import supplies clean metadata by file before the song is ever scanned.
    QString error;
    {
        MetadataOverrideStore store(overridesPath);
        QVERIFY2(store.open(&error, {root}), qPrintable(error));
        MetadataOverride imported;
        imported.rootPath = root;
        imported.mp3RelPath = QStringLiteral("New Songs/new-01.mp3");
        imported.artist = QStringLiteral("Patsy Cline");
        imported.title = QStringLiteral("Crazy");
        imported.label = QStringLiteral("Legends");
        imported.series = QStringLiteral("Country Gold");
        imported.trustedDiscId = QStringLiteral("LEG099");
        imported.trustedTrack = 7;
        imported.origin = QStringLiteral("import");
        QVERIFY2(store.setOverride(imported, &error), qPrintable(error));
        MetadataOverride invalid = imported;
        invalid.origin = QStringLiteral("guess");
        QVERIFY(!store.setOverride(invalid, &error));
    }
    QCOMPARE(runScan(path, root, overridesPath).value(QStringLiteral("status")).toString(),
             QStringLiteral("completed"));

    auto shown = [&](const QString& search) {
        Catalogue catalogue(path);
        QString openError;
        if (!catalogue.open(&openError))
            qFatal("open failed");
        const QList<CatalogueSearchRow> rows = catalogue.search(search, 10, true, &openError);
        if (rows.size() != 1)
            return QStringList{QString::number(rows.size())};
        const QVariantMap detail = catalogue.reviewDetail(rows.first().songId, &openError);
        return QStringList{rows.first().displayArtist, rows.first().displayTitle, rows.first().label,
                           rows.first().series, rows.first().discId, QString::number(rows.first().track),
                           rows.first().confidence, detail.value(QStringLiteral("source")).toString(),
                           detail.value(QStringLiteral("labelSource")).toString(),
                           QString::number(rows.first().songId)};
    };
    const QStringList expected = {QStringLiteral("Patsy Cline"), QStringLiteral("Crazy"),
                                  QStringLiteral("Legends"), QStringLiteral("Country Gold"),
                                  QStringLiteral("LEG099"), QStringLiteral("7"), QStringLiteral("high"),
                                  QStringLiteral("import"), QStringLiteral("import")};
    QStringList imported = shown(QStringLiteral("patsy cline crazy"));
    QCOMPARE(imported.mid(0, 9), expected);
    const qint64 songId = imported.at(9).toLongLong();
    // Findable by trusted label, series and disc, and still by the raw name.
    QCOMPARE(shown(QStringLiteral("legends country gold leg099")).value(9), imported.at(9));
    QCOMPARE(shown(QStringLiteral("new 01")).value(9), imported.at(9));

    // Resolver upgrades and reprocessing never touch trusted values.
    {
        Catalogue catalogue(path);
        QVERIFY2(catalogue.open(&error), qPrintable(error));
        QVERIFY2(MetadataResolver::resolve(catalogue, -1, &error), qPrintable(error));
    }
    QCOMPARE(shown(QStringLiteral("patsy cline crazy")).mid(0, 9), expected);
    QCOMPARE(runScan(path, root, overridesPath).value(QStringLiteral("status")).toString(),
             QStringLiteral("completed"));
    QCOMPARE(shown(QStringLiteral("patsy cline crazy")).mid(0, 9), expected);

    // A label-only correction keeps the automatic name and confidence.
    {
        Catalogue catalogue(path);
        QVERIFY2(catalogue.open(&error), qPrintable(error));
        const QList<CatalogueSearchRow> rows = catalogue.search(QStringLiteral("ring of fire"), 10, true, &error);
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows.first().label, QStringLiteral("Sunfly"));
        const QString confidence = rows.first().confidence;
        MetadataOverride relabel;
        relabel.label = QStringLiteral("Sunfly Gold Collection");
        QVERIFY2(catalogue.setTrustedMetadata(rows.first().songId, relabel, 0, &error), qPrintable(error));
        const QList<CatalogueSearchRow> after = catalogue.search(QStringLiteral("ring of fire"), 10, true, &error);
        QCOMPARE(after.first().label, QStringLiteral("Sunfly Gold Collection"));
        QCOMPARE(after.first().displayTitle, QStringLiteral("Ring Of Fire"));
        QCOMPARE(after.first().confidence, confidence);
        QVERIFY2(MetadataResolver::resolve(catalogue, -1, &error), qPrintable(error));
        QCOMPARE(catalogue.search(QStringLiteral("ring of fire"), 10, true, &error).first().label,
                 QStringLiteral("Sunfly Gold Collection"));

        // Clearing the import returns every value to the automatic one.
        QVERIFY2(catalogue.clearManualOverride(songId, &error), qPrintable(error));
        const QList<CatalogueSearchRow> cleared = catalogue.search(QStringLiteral("new 01"), 10, true, &error);
        QCOMPARE(cleared.size(), 1);
        QCOMPARE(cleared.first().displayTitle, QStringLiteral("new-01"));
        QCOMPARE(cleared.first().label, QString());
        QCOMPARE(cleared.first().discId, QString());
        QCOMPARE(catalogue.search(QStringLiteral("patsy cline"), 10, true, &error).size(), 0);
    }
}

void TestMetadataFoundation::overrideStoreMigratesFromVersionOne()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path = temporary.filePath(QStringLiteral("app/metadata-overrides.sqlite"));
    const QString music = Catalogue::canonicalPath(temporary.filePath(QStringLiteral("music")));
    QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
    const QString connection = QStringLiteral("v1-overrides-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(path);
        QVERIFY(database.open());
        QSqlQuery query(database);
        QVERIFY(query.exec(QStringLiteral(
            "CREATE TABLE metadata_overrides(root_path TEXT NOT NULL,mp3_rel_path TEXT NOT NULL,"
            "artist TEXT,title TEXT,created_at INTEGER NOT NULL,updated_at INTEGER NOT NULL,"
            "auto_artist TEXT,auto_title TEXT,disc_id TEXT,track INTEGER NOT NULL DEFAULT 0,"
            "file_name TEXT,PRIMARY KEY(root_path,mp3_rel_path))")));
        // Stored rows hold the root as Catalogue::canonicalPath writes it
        // (on Windows with its drive letter).
        query.prepare(QStringLiteral(
            "INSERT INTO metadata_overrides VALUES(?,'SGB39/3902.mp3','Frank Sinatra','My Way',1,2,"
            "'','Disc SGB39 - Track 02','SGB39',2,'3902.mp3')"));
        query.addBindValue(music);
        QVERIFY(query.exec());
        QVERIFY(query.exec(QStringLiteral("PRAGMA user_version=1")));
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
    MetadataOverrideStore store(path);
    QString error;
    QVERIFY2(store.open(&error), qPrintable(error));
    const QList<MetadataOverride> all = store.all(&error);
    QCOMPARE(all.size(), 1);
    QCOMPARE(*all.first().title, QStringLiteral("My Way"));
    QCOMPARE(all.first().origin, QStringLiteral("manual"));
    QVERIFY(!all.first().label && !all.first().trustedDiscId && !all.first().trustedTrack);
    QVERIFY(!all.first().originalKey);
    // Version 3 adds the original key: it is stored and read back, and only
    // a real key (0-23) is accepted.
    MetadataOverride value = all.first();
    value.originalKey = 21;
    QVERIFY2(store.setOverride(value, &error), qPrintable(error));
    QCOMPARE(*store.overrideFor(music, QStringLiteral("SGB39/3902.mp3"))->originalKey, 21);
    value.originalKey = 24;
    QVERIFY(!store.setOverride(value, &error));
    // A key alone is a value worth keeping.
    MetadataOverride keyOnly;
    keyOnly.rootPath = music;
    keyOnly.mp3RelPath = QStringLiteral("SGB39/3903.mp3");
    keyOnly.originalKey = 0;
    QVERIFY(keyOnly.hasValues());
    QVERIFY2(store.setOverride(keyOnly, &error), qPrintable(error));
    QCOMPARE(store.all(&error).size(), 2);
    store.close();
    // Migrated and at the current version; a version-2 store gains the column too.
    QSqlDatabase check = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
    check.setDatabaseName(path);
    QVERIFY(check.open());
    {
        QSqlQuery version(check);
        QVERIFY(version.exec(QStringLiteral("PRAGMA user_version")) && version.next());
        QCOMPARE(version.value(0).toInt(), MetadataOverrideStore::SchemaVersion);
        QCOMPARE(MetadataOverrideStore::SchemaVersion, 3);
        QSqlQuery rebuild(check);
        QVERIFY(rebuild.exec(QStringLiteral("ALTER TABLE metadata_overrides DROP COLUMN original_key")));
        QVERIFY(rebuild.exec(QStringLiteral("PRAGMA user_version=2")));
    }
    check.close();
    check = QSqlDatabase();
    QSqlDatabase::removeDatabase(connection);
    MetadataOverrideStore fromTwo(path);
    QVERIFY2(fromTwo.open(&error), qPrintable(error));
    QCOMPARE(fromTwo.all(&error).size(), 2);
    QVERIFY(!fromTwo.overrideFor(music, QStringLiteral("SGB39/3902.mp3"))->originalKey);
    QCOMPARE(*fromTwo.overrideFor(music, QStringLiteral("SGB39/3902.mp3"))->title,
             QStringLiteral("My Way"));
}

#include "tst_metadatafoundation.moc"
