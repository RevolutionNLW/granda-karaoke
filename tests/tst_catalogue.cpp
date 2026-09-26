#include "library/Catalogue.h"
#include "library/CatalogueTools.h"
#include "library/LibraryScanner.h"
#include "library/MetadataResolver.h"

#include <QCryptographicHash>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>
#include <QtTest>

namespace {

void appendLe16(QByteArray& bytes, quint16 value)
{
    bytes.append(char(value));
    bytes.append(char(value >> 8));
}

void appendLe32(QByteArray& bytes, quint32 value)
{
    appendLe16(bytes, quint16(value));
    appendLe16(bytes, quint16(value >> 16));
}

quint32 crc32(const QByteArray& bytes)
{
    quint32 value = 0xffffffffU;
    for (const uchar byte : bytes) {
        value ^= byte;
        for (int bit = 0; bit < 8; ++bit)
            value = (value & 1U) ? (value >> 1U) ^ 0xedb88320U : value >> 1U;
    }
    return value ^ 0xffffffffU;
}

struct Member { QByteArray name; quint16 method = 0; QByteArray data; };

QByteArray makeZip(const QList<Member>& members)
{
    QByteArray zip;
    struct Central { Member member; quint32 offset; };
    QList<Central> central;
    for (const Member& member : members) {
        central.append({member, quint32(zip.size())});
        appendLe32(zip, 0x04034b50);
        appendLe16(zip, 20); appendLe16(zip, 0); appendLe16(zip, member.method);
        appendLe16(zip, 0); appendLe16(zip, 0); appendLe32(zip, crc32(member.data));
        appendLe32(zip, quint32(member.data.size())); appendLe32(zip, quint32(member.data.size()));
        appendLe16(zip, quint16(member.name.size())); appendLe16(zip, 0);
        zip += member.name; zip += member.data;
    }
    const quint32 offset = quint32(zip.size());
    for (const Central& entry : central) {
        appendLe32(zip, 0x02014b50); appendLe16(zip, 20); appendLe16(zip, 20);
        appendLe16(zip, 0); appendLe16(zip, entry.member.method); appendLe16(zip, 0); appendLe16(zip, 0);
        appendLe32(zip, crc32(entry.member.data)); appendLe32(zip, quint32(entry.member.data.size()));
        appendLe32(zip, quint32(entry.member.data.size())); appendLe16(zip, quint16(entry.member.name.size()));
        appendLe16(zip, 0); appendLe16(zip, 0); appendLe16(zip, 0); appendLe16(zip, 0);
        appendLe32(zip, 0); appendLe32(zip, entry.offset); zip += entry.member.name;
    }
    const quint32 size = quint32(zip.size()) - offset;
    appendLe32(zip, 0x06054b50); appendLe16(zip, 0); appendLe16(zip, 0);
    appendLe16(zip, quint16(members.size())); appendLe16(zip, quint16(members.size()));
    appendLe32(zip, size); appendLe32(zip, offset); appendLe16(zip, 0);
    return zip;
}

void writeFile(const QString& path, const QByteArray& data)
{
    QVERIFY2(QDir().mkpath(QFileInfo(path).absolutePath()), qPrintable(path));
    QFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly), qPrintable(file.errorString()));
    QCOMPARE(file.write(data), qint64(data.size()));
}

QVariantMap snapshot(const QString& root)
{
    QVariantMap result;
    QDirIterator it(root, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        const QFileInfo info(path);
        QVariantMap item;
        item.insert(QStringLiteral("dir"), info.isDir());
        item.insert(QStringLiteral("size"), info.size());
        item.insert(QStringLiteral("mtime"), info.lastModified().toMSecsSinceEpoch());
        if (info.isFile()) {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly))
                qFatal("Could not open synthetic snapshot file");
            item.insert(QStringLiteral("sha256"), QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256));
        }
        result.insert(QDir(root).relativeFilePath(path), item);
    }
    return result;
}

QVariantMap runScan(const QString& db, const QString& root, bool tags = false)
{
    LibraryScanner scanner(db);
    scanner.setOptions({tags, 0});
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

QString logicalDatabaseContents(const QString& path)
{
    const QString connection = QStringLiteral("logical-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QStringList lines;
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(path);
        if (!database.open())
            qFatal("Could not open logical comparison database");
        const QStringList queries = {
            QStringLiteral("SELECT rel_path,rel_dir,file_name,ext,kind,size,present,tags_state,COALESCE(raw_tags_json,''),COALESCE(crc32,''),COALESCE(hex(sha256),''),COALESCE(zip_status,''),COALESCE(zip_detail,'') FROM files ORDER BY rel_path"),
            QStringLiteral("SELECT f.rel_path,z.name,z.method,z.crc32,z.compressed_size,z.uncompressed_size,z.encrypted,z.kind FROM zip_members z JOIN files f ON f.id=z.zip_file_id ORDER BY f.rel_path,z.name"),
            QStringLiteral("SELECT s.kind,COALESCE(m.rel_path,''),COALESCE(g.rel_path,''),COALESCE(z.rel_path,''),COALESCE(s.zip_mp3_member,''),COALESCE(s.zip_graphics_member,''),s.playable,COALESCE(s.unplayable_reason,''),s.parsed_json FROM sources s LEFT JOIN files m ON m.id=s.mp3_file_id LEFT JOIN files g ON g.id=s.graphics_file_id LEFT JOIN files z ON z.id=s.zip_file_id ORDER BY 1,2,3,4,5"),
            QStringLiteral("SELECT title,artist,title_raw,artist_raw,disc_id,disc_prefix,track,display_title,display_artist,search_text,metadata_source,confidence,playable,(SELECT count(*) FROM sources s WHERE s.song_id=songs.id) FROM songs ORDER BY search_text")};
        for (const QString& sql : queries) {
            QSqlQuery query(database);
            if (!query.exec(sql))
                qFatal("Logical comparison query failed");
            while (query.next()) {
                QStringList values;
                for (int i = 0; i < query.record().count(); ++i)
                    values.append(query.value(i).toString());
                lines.append(values.join(QChar(0x1f)));
            }
            lines.append(QStringLiteral("--"));
        }
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
    return lines.join(QLatin1Char('\n'));
}

void makeLibrary(const QString& root)
{
    const QByteArray audioA("audio-A");
    const QByteArray graphicsA("graphics-A");
    writeFile(root + QStringLiteral("/SF001/SF001-01 - Reeves, Jim - Welcome.mp3"), audioA);
    writeFile(root + QStringLiteral("/SF001/SF001-01 - Reeves, Jim - Welcome.CDG"), graphicsA);
    writeFile(root + QStringLiteral("/SF001/SF001-02 - Reeves, Jim - Distant Drums.MP3"), QByteArray("audio-B"));
    writeFile(root + QStringLiteral("/SF001/sf001-02 - reeves, jim - distant drums.cdg"), QByteArray("graphics-B"));
    writeFile(root + QStringLiteral("/DK002/DK002-01 - The Winner - ABBA.mp3"), QByteArray("same-size-1"));
    writeFile(root + QStringLiteral("/DK002/DK002-01 - The Winner - ABBA.cdg"), QByteArray("same-cdg-1"));
    writeFile(root + QStringLiteral("/DK002/DK002-02 - Dancing Queen - ABBA.mp3"), QByteArray("same-size-2"));
    writeFile(root + QStringLiteral("/DK002/DK002-02 - Dancing Queen - ABBA.cdg"), QByteArray("same-cdg-2"));
    writeFile(root + QStringLiteral("/misc/unsupported.mp3"), QByteArray("mcg-audio"));
    writeFile(root + QStringLiteral("/misc/unsupported.mcg"), QByteArray("CAVSMC"));
    writeFile(root + QStringLiteral("/misc/orphan.mp3"), QByteArray("orphan"));
    writeFile(root + QStringLiteral("/misc/lonely.cdg"), QByteArray("orphan-other-stem"));
    writeFile(root + QStringLiteral("/misc/empty.mp3"), {});
    writeFile(root + QStringLiteral("/SGB39/02.mp3"), QByteArray("numeric-audio"));
    writeFile(root + QStringLiteral("/SGB39/02.cdg"), QByteArray("numeric-graphics"));
    writeFile(root + QStringLiteral("/._AppleDouble"), QByteArray("ignored"));
    writeFile(root + QStringLiteral("/Thumbs.db"), QByteArray("ignored"));
    writeFile(root + QStringLiteral("/duplicate.zip"), makeZip({
        {"inside/SF001-01 - Reeves, Jim - Welcome.mp3", 0, audioA},
        {"inside/SF001-01 - Reeves, Jim - Welcome.cdg", 0, graphicsA}}));
    writeFile(root + QStringLiteral("/same-size-different.zip"), makeZip({
        {"SF001-01 - Reeves, Jim - Welcome.mp3", 0, QByteArray("xxxxxxx")},
        {"SF001-01 - Reeves, Jim - Welcome.cdg", 0, QByteArray("yyyyyyyyyy")}}));
    writeFile(root + QStringLiteral("/deflate64.zip"), makeZip({
        {"method9.mp3", 9, QByteArray("audio")}, {"method9.cdg", 9, QByteArray("cdg")}}));
    QByteArray truncated = makeZip({{"bad.mp3", 0, QByteArray("x")}});
    truncated.chop(22);
    writeFile(root + QStringLiteral("/truncated.zip"), truncated);
    writeFile(root + QStringLiteral("/nested.zip"), makeZip({
        {"one.zip", 0, QByteArray("nested")}, {"cover.jpg", 0, QByteArray("jpg")}}));
}

} // namespace

class TestCatalogue : public QObject {
    Q_OBJECT

private slots:
    void scanQueriesAndReadOnly();
    void incrementalAndMissing();
    void guardsAndNewerSchema();
    void cancelAndResume();
    void pauseResumeAndCancelWhilePaused();
    void activeRootFiltersSearchAndPlayback();
    void libraryReadyBeforeAndAfterTags();
    void rootDisappearsWithoutMarkingMissing();
    void rootUnpluggedDuringTagsKeepsWorkPending();
    void databaseOnlyOpenWithMissingRoot();
    void scannerRobustness();
    void resolverRulesAndEvaluation();
    void searchPerformance();
};

void TestCatalogue::scanQueriesAndReadOnly()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("library"));
    const QString db = temporary.filePath(QStringLiteral("app/catalogue.sqlite"));
    makeLibrary(root);
    const QVariantMap before = snapshot(root);
    const QVariantMap first = runScan(db, root);
    QCOMPARE(first.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
    const QVariantMap second = runScan(db, root);
    QCOMPARE(second.value(QStringLiteral("counts")).toMap().value(QStringLiteral("sourceFileReads")).toULongLong(), 0ULL);
    QCOMPARE(snapshot(root), before);

    Catalogue catalogue(db);
    QString error;
    QVERIFY2(catalogue.open(&error, {root}), qPrintable(error));
    const QVariantMap stats = catalogue.stats(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(stats.value(QStringLiteral("playable")).toLongLong(), 5LL);
    QCOMPARE(stats.value(QStringLiteral("duplicatesMerged")).toLongLong(), 1LL);
    const QList<CatalogueSearchRow> rows = catalogue.search(QStringLiteral("Reeves Welcome"), 10, false, &error);
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows.first().displayArtist, QStringLiteral("Jim Reeves"));
    const PlaybackPaths paths = catalogue.playbackPathsFor(rows.first().songId, &error);
    QVERIFY(paths.playable());
    QVERIFY(paths.mp3Path.endsWith(QStringLiteral(".mp3"), Qt::CaseInsensitive));

    const QList<CatalogueSearchRow> unplayable = catalogue.search(QStringLiteral("unsupported"), 10, true, &error);
    QCOMPARE(unplayable.size(), 1);
    QCOMPARE(catalogue.playbackPathsFor(unplayable.first().songId, &error).reason,
             QStringLiteral("mcg_graphics_unsupported"));
    const QList<CatalogueSearchRow> dk = catalogue.search(QStringLiteral("ABBA Winner"), 10, false, &error);
    QCOMPARE(dk.size(), 1);
    QCOMPARE(dk.first().displayTitle, QStringLiteral("The Winner"));
    QCOMPARE(dk.first().displayArtist, QStringLiteral("ABBA"));
    const QList<CatalogueSearchRow> numeric = catalogue.search(QStringLiteral("SGB39 2"), 10, false, &error);
    QCOMPARE(numeric.size(), 1);
    QCOMPARE(numeric.first().displayTitle, QStringLiteral("Disc SGB39 - Track 02"));
    const QVariantMap zipStatuses = stats.value(QStringLiteral("zipsByStatus")).toMap();
    QCOMPARE(zipStatuses.value(QStringLiteral("zip_nested")).toLongLong(), 1LL);
    QCOMPARE(zipStatuses.value(QStringLiteral("zip_damaged")).toLongLong(), 1LL);
    const QVariantMap reasons = stats.value(QStringLiteral("unplayableByReason")).toMap();
    QVERIFY(reasons.value(QStringLiteral("zip_compression_unsupported")).toLongLong() >= 1);
}

void TestCatalogue::incrementalAndMissing()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("library"));
    const QString db = temporary.filePath(QStringLiteral("app/catalogue.sqlite"));
    makeLibrary(root);
    runScan(db, root, true);
    const QString changed = root + QStringLiteral("/misc/unsupported.mp3");
    QTest::qWait(2);
    writeFile(changed, QByteArray("changed audio"));
    QVERIFY(QFile::remove(root + QStringLiteral("/DK002/DK002-02 - Dancing Queen - ABBA.cdg")));
    const QVariantMap summary = runScan(db, root, true);
    QCOMPARE(summary.value(QStringLiteral("counts")).toMap().value(QStringLiteral("changed")).toLongLong(), 1LL);
    QCOMPARE(summary.value(QStringLiteral("counts")).toMap().value(QStringLiteral("sourceFileReads")).toULongLong(), 1ULL);
    Catalogue catalogue(db);
    QString error;
    QVERIFY(catalogue.open(&error));
    const QVariantMap explained = catalogue.explain(QStringLiteral("DK002/DK002-02 - Dancing Queen - ABBA.cdg"), &error);
    QCOMPARE(explained.value(QStringLiteral("present")).toBool(), false);
}

void TestCatalogue::guardsAndNewerSchema()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("library"));
    QVERIFY(QDir().mkpath(root));
    LibraryScanner scanner(root + QStringLiteral("/catalogue.sqlite"));
    QString failure;
    QObject::connect(&scanner, &LibraryScanner::failed, [&](const QString& value) { failure = value; });
    scanner.scan(root);
    QVERIFY(failure.contains(QStringLiteral("must not be inside")));
    QVERIFY(!QFileInfo::exists(root + QStringLiteral("/catalogue.sqlite")));

    Catalogue unsafeCache(temporary.filePath(QStringLiteral("safe.sqlite")),
                          root + QStringLiteral("/cache"));
    QString cacheError;
    QVERIFY(!unsafeCache.open(&cacheError, {root}));
    QVERIFY(cacheError.contains(QStringLiteral("Cache directory")));
    QVERIFY(!QFileInfo::exists(root + QStringLiteral("/cache")));

    const QString newer = temporary.filePath(QStringLiteral("newer.sqlite"));
    const QString connection = QStringLiteral("newer-schema-test");
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(newer);
        QVERIFY(database.open());
        QSqlQuery query(database);
        QVERIFY(query.exec(QStringLiteral("PRAGMA user_version=999")));
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
    Catalogue catalogue(newer);
    QString error;
    QVERIFY(!catalogue.open(&error));
    QVERIFY(error.contains(QStringLiteral("newer than supported")));
    QVERIFY(QFileInfo::exists(newer));
    QVERIFY(!QFileInfo::exists(newer + QStringLiteral(".corrupt-")));

    const QString corrupt = temporary.filePath(QStringLiteral("corrupt.sqlite"));
    writeFile(corrupt, QByteArray("definitely not sqlite"));
    Catalogue recovered(corrupt);
    error.clear();
    QVERIFY2(recovered.open(&error), qPrintable(error));
    QCOMPARE(recovered.stats(&error).value(QStringLiteral("songs")).toLongLong(), 0LL);
    const QStringList quarantined = QDir(temporary.path()).entryList(
        {QStringLiteral("corrupt.sqlite.corrupt-*")}, QDir::Files);
    QCOMPARE(quarantined.size(), 1);
}

void TestCatalogue::cancelAndResume()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("library"));
    const QString db = temporary.filePath(QStringLiteral("app/catalogue.sqlite"));
    const QString uninterruptedDb = temporary.filePath(QStringLiteral("app/uninterrupted.sqlite"));
    makeLibrary(root);
    LibraryScanner scanner(db);
    scanner.setOptions({false, 0});
    bool requested = false;
    QVariantMap cancelled;
    QObject::connect(&scanner, &LibraryScanner::progress,
                     [&](const QString& phase, qint64 done, qint64, const QString&) {
        if (!requested && phase == QLatin1String("walk") && done >= 3) {
            requested = true;
            scanner.requestCancel();
        }
    });
    QObject::connect(&scanner, &LibraryScanner::finished,
                     [&](const QVariantMap& value) { cancelled = value; });
    scanner.scan(root);
    QCOMPARE(cancelled.value(QStringLiteral("status")).toString(), QStringLiteral("cancelled"));
    const QVariantMap resumed = runScan(db, root);
    QCOMPARE(resumed.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
    runScan(uninterruptedDb, root);
    QCOMPARE(logicalDatabaseContents(db), logicalDatabaseContents(uninterruptedDb));
    Catalogue catalogue(db);
    QString error;
    QVERIFY(catalogue.open(&error));
    QCOMPARE(catalogue.stats(&error).value(QStringLiteral("playable")).toLongLong(), 5LL);
}

void TestCatalogue::pauseResumeAndCancelWhilePaused()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("library"));
    const QString db = temporary.filePath(QStringLiteral("app/catalogue.sqlite"));
    for (int i = 0; i < 200; ++i) {
        const QString base = root + QStringLiteral("/AT%1-01 - Artist - Song %1")
                                        .arg(i, 3, 10, QLatin1Char('0'));
        writeFile(base + QStringLiteral(".mp3"), "audio");
        writeFile(base + QStringLiteral(".cdg"), "lyrics");
    }

    QThread thread;
    auto* scanner = new LibraryScanner(db);
    scanner->setOptions({false, 0});
    scanner->setPaused(true);
    scanner->moveToThread(&thread);
    QSignalSpy progress(scanner, &LibraryScanner::progress);
    QSignalSpy finished(scanner, &LibraryScanner::finished);
    connect(&thread, &QThread::started, scanner, [scanner, root] { scanner->scan(root); });
    connect(&thread, &QThread::finished, scanner, &QObject::deleteLater);
    thread.start();
    QTest::qWait(150);
    QCOMPARE(progress.count(), 0);
    QCOMPARE(finished.count(), 0);

    scanner->setPaused(false);
    QTRY_VERIFY_WITH_TIMEOUT(progress.count() > 0, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
    QCOMPARE(finished.first().first().toMap().value(QStringLiteral("status")).toString(),
             QStringLiteral("completed"));
    thread.quit();
    QVERIFY(thread.wait(1000));

    QThread cancelThread;
    auto* cancelledScanner = new LibraryScanner(temporary.filePath(
        QStringLiteral("app/cancelled.sqlite")));
    cancelledScanner->setPaused(true);
    cancelledScanner->moveToThread(&cancelThread);
    QSignalSpy cancelled(cancelledScanner, &LibraryScanner::finished);
    connect(&cancelThread, &QThread::started, cancelledScanner,
            [cancelledScanner, root] { cancelledScanner->scan(root); });
    connect(&cancelThread, &QThread::finished, cancelledScanner, &QObject::deleteLater);
    cancelThread.start();
    QTest::qWait(100);
    QElapsedTimer timer;
    timer.start();
    cancelledScanner->requestCancel();
    QTRY_COMPARE_WITH_TIMEOUT(cancelled.count(), 1, 1000);
    QVERIFY(timer.elapsed() < 1000);
    QCOMPARE(cancelled.first().first().toMap().value(QStringLiteral("status")).toString(),
             QStringLiteral("cancelled"));
    cancelThread.quit();
    QVERIFY(cancelThread.wait(1000));
}

void TestCatalogue::activeRootFiltersSearchAndPlayback()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString firstRoot = temporary.filePath(QStringLiteral("first"));
    const QString secondRoot = temporary.filePath(QStringLiteral("second"));
    const QString db = temporary.filePath(QStringLiteral("app/catalogue.sqlite"));
    writeFile(firstRoot + QStringLiteral("/AT001-01 - First Artist - First Song.mp3"), "audio");
    writeFile(firstRoot + QStringLiteral("/AT001-01 - First Artist - First Song.cdg"), "lyrics");
    writeFile(secondRoot + QStringLiteral("/AT002-01 - Second Artist - Second Song.mp3"), "audio");
    writeFile(secondRoot + QStringLiteral("/AT002-01 - Second Artist - Second Song.cdg"), "lyrics");
    runScan(db, firstRoot);
    runScan(db, secondRoot);

    Catalogue catalogue(db);
    QString error;
    QVERIFY2(catalogue.open(&error), qPrintable(error));
    QCOMPARE(catalogue.searchActive(QStringLiteral("First"), 10, &error).size(), 1);
    QCOMPARE(catalogue.searchActive(QStringLiteral("Second"), 10, &error).size(), 0);
    const auto roots = catalogue.roots(&error);
    QCOMPARE(roots.size(), 2);
    QVERIFY(catalogue.setActiveRoot(roots.at(1).id, &error));
    QCOMPARE(catalogue.searchActive(QStringLiteral("First"), 10, &error).size(), 0);
    const auto second = catalogue.searchActive(QStringLiteral("Second"), 10, &error);
    QCOMPARE(second.size(), 1);
    const PlaybackPaths paths = catalogue.activePlaybackPathsFor(second.first().songId, &error);
    QVERIFY(paths.playable());
    QVERIFY(paths.mp3Path.startsWith(Catalogue::canonicalPath(secondRoot)));
    QCOMPARE(catalogue.activeSongCount(&error), 1LL);
}

void TestCatalogue::libraryReadyBeforeAndAfterTags()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("library"));
    const QString db = temporary.filePath(QStringLiteral("app/catalogue.sqlite"));
    writeFile(root + QStringLiteral("/AT001-01 - Artist - Song.mp3"), "audio");
    writeFile(root + QStringLiteral("/AT001-01 - Artist - Song.cdg"), "lyrics");
    LibraryScanner scanner(db);
    scanner.setOptions({true, 0});
    QSignalSpy ready(&scanner, &LibraryScanner::libraryReady);
    QSignalSpy finished(&scanner, &LibraryScanner::finished);
    scanner.scan(root);
    QCOMPARE(finished.count(), 1);
    QCOMPARE(ready.count(), 2);
}

void TestCatalogue::rootUnpluggedDuringTagsKeepsWorkPending()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("library"));
    const QString away = temporary.filePath(QStringLiteral("unplugged"));
    const QString db = temporary.filePath(QStringLiteral("app/catalogue.sqlite"));
    for (int track = 1; track <= 5; ++track) {
        const QString stem = QStringLiteral("/AT800/AT800-%1 - Some Artist - Song %1")
                                 .arg(track, 2, 10, QLatin1Char('0'));
        writeFile(root + stem + QStringLiteral(".mp3"), QByteArray("audio-") + QByteArray::number(track));
        writeFile(root + stem + QStringLiteral(".cdg"), QByteArray("lyrics-") + QByteArray::number(track));
    }

    // "Unplug" the drive right after the library becomes searchable, i.e. just
    // before tag reading starts.
    QVariantMap summary;
    {
        LibraryScanner scanner(db);
        scanner.setOptions({true, 0});
        QObject::connect(&scanner, &LibraryScanner::libraryReady, [&] {
            if (QFileInfo(root).isDir())
                QVERIFY(QDir().rename(root, away));
        });
        QObject::connect(&scanner, &LibraryScanner::finished,
                         [&](const QVariantMap& value) { summary = value; });
        scanner.scan(root);
    }
    QCOMPARE(summary.value(QStringLiteral("status")).toString(), QStringLiteral("offline"));

    auto tagStates = [&] {
        const QString connection = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QMap<QString, int> states;
        {
            QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
            database.setDatabaseName(db);
            database.open();
            QSqlQuery query(QStringLiteral("SELECT tags_state, count(*) FROM files WHERE kind='mp3' GROUP BY 1"), database);
            while (query.next())
                states.insert(query.value(0).toString(), query.value(1).toInt());
        }
        QSqlDatabase::removeDatabase(connection);
        return states;
    };
    // Nothing was recorded as read while the drive was away.
    QCOMPARE(tagStates().value(QStringLiteral("pending")), 5);
    QCOMPARE(tagStates().value(QStringLiteral("read")), 0);

    // Plugged back in: the next scan resumes the pending work and completes.
    QVERIFY(QDir().rename(away, root));
    summary = runScan(db, root, true);
    QCOMPARE(summary.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
    QCOMPARE(tagStates().value(QStringLiteral("read")), 5);
    QCOMPARE(summary.value(QStringLiteral("counts")).toMap().value(QStringLiteral("changed")).toLongLong(), 0LL);
}

void TestCatalogue::rootDisappearsWithoutMarkingMissing()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("library"));
    const QString moved = temporary.filePath(QStringLiteral("library-unplugged"));
    const QString db = temporary.filePath(QStringLiteral("app/catalogue.sqlite"));
    makeLibrary(root);
    runScan(db, root);

    LibraryScanner scanner(db);
    scanner.setOptions({false, 0});
    bool renamed = false;
    QVariantMap summary;
    QString failure;
    QObject::connect(&scanner, &LibraryScanner::progress,
                     [&](const QString& phase, qint64 done, qint64, const QString&) {
        if (!renamed && phase == QLatin1String("walk") && done >= 1) {
            renamed = QDir().rename(root, moved);
        }
    });
    QObject::connect(&scanner, &LibraryScanner::finished,
                     [&](const QVariantMap& value) { summary = value; });
    QObject::connect(&scanner, &LibraryScanner::failed,
                     [&](const QString& value) { failure = value; });
    scanner.scan(root);
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
    QVERIFY(renamed);
    QCOMPARE(summary.value(QStringLiteral("status")).toString(), QStringLiteral("offline"));
    QVERIFY(QDir().rename(moved, root));

    Catalogue catalogue(db);
    QString error;
    QVERIFY(catalogue.open(&error));
    QCOMPARE(catalogue.stats(&error).value(QStringLiteral("orphans")).toLongLong(), 3LL);
    const QList<CatalogueSearchRow> cached = catalogue.searchActive(
        QStringLiteral("Reeves Welcome"), 10, &error);
    QCOMPARE(cached.size(), 1);
    QVERIFY(catalogue.activePlaybackPathsFor(cached.first().songId, &error).playable());
    const QVariantMap explained = catalogue.explain(QStringLiteral("SF001/SF001-01 - Reeves, Jim - Welcome.mp3"), &error);
    QCOMPARE(explained.value(QStringLiteral("present")).toBool(), true);
    QCOMPARE(explained.value(QStringLiteral("reason")).toString(), QStringLiteral("root_offline"));
    catalogue.close();
    QCOMPARE(runScan(db, root).value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
}

void TestCatalogue::scannerRobustness()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("library"));
    const QString db = temporary.filePath(QStringLiteral("app/catalogue.sqlite"));
    writeFile(root + QStringLiteral("/ok/AT900-01 - Artist One - Song One.mp3"), "audio");
    writeFile(root + QStringLiteral("/ok/AT900-01 - Artist One - Song One.cdg"), "lyrics");
    writeFile(root + QStringLiteral("/empty/AT901-01 - Empty Artist - Empty Song.mp3"), {});
    writeFile(root + QStringLiteral("/empty/AT901-01 - Empty Artist - Empty Song.cdg"), {});
    const QString restricted = root + QStringLiteral("/restricted");
    writeFile(restricted + QStringLiteral("/AT902-01 - Hidden Artist - Hidden Song.mp3"), "audio");
    writeFile(restricted + QStringLiteral("/AT902-01 - Hidden Artist - Hidden Song.cdg"), "lyrics");
    QByteArray shifted = makeZip({{"inside.mp3", 0, "audio"},
                                  {"inside.cdg", 0, "lyrics"}});
    shifted.remove(0, 10);
    writeFile(root + QStringLiteral("/damaged.zip"), shifted);
#ifndef Q_OS_WIN
    const QString linkedTarget = temporary.filePath(QStringLiteral("linked-target"));
    writeFile(linkedTarget + QStringLiteral("/AT903-01 - Linked Artist - Linked Song.mp3"), "audio");
    writeFile(linkedTarget + QStringLiteral("/AT903-01 - Linked Artist - Linked Song.cdg"), "lyrics");
    QVERIFY(QFile::link(linkedTarget, root + QStringLiteral("/linked-folder")));
#endif

    QCOMPARE(runScan(db, root).value(QStringLiteral("status")).toString(),
             QStringLiteral("completed"));
    QVERIFY(QFile::setPermissions(restricted, QFileDevice::Permissions()));
    QVERIFY(!QDir(restricted).isReadable());
    const QVariantMap summary = runScan(db, root);
    QCOMPARE(summary.value(QStringLiteral("status")).toString(), QStringLiteral("incomplete"));
    const QVariantMap counts = summary.value(QStringLiteral("counts")).toMap();
    QCOMPARE(counts.value(QStringLiteral("walkComplete")).toBool(), false);
    QCOMPARE(counts.value(QStringLiteral("skippedUnreadableDirectories")).toLongLong(), 1LL);

    Catalogue catalogue(db);
    QString error;
    QVERIFY2(catalogue.open(&error), qPrintable(error));
    QCOMPARE(catalogue.roots(&error).first().online, true);
    QCOMPARE(catalogue.explain(QStringLiteral("restricted/AT902-01 - Hidden Artist - Hidden Song.mp3"),
                               &error).value(QStringLiteral("present")).toBool(), true);
    QCOMPARE(catalogue.explain(QStringLiteral("empty/AT901-01 - Empty Artist - Empty Song.mp3"),
                               &error).value(QStringLiteral("reason")).toString(),
             QStringLiteral("file_empty"));
    QCOMPARE(catalogue.explain(QStringLiteral("damaged.zip"), &error)
                 .value(QStringLiteral("reason")).toString(),
             QStringLiteral("zip_damaged"));
#ifndef Q_OS_WIN
    QCOMPARE(catalogue.search(QStringLiteral("Linked Artist"), 10, true, &error).size(), 0);
#endif
    catalogue.close();
    QVERIFY(QFile::setPermissions(restricted, QFileDevice::ReadOwner
                                               | QFileDevice::WriteOwner
                                               | QFileDevice::ExeOwner));
}

void TestCatalogue::databaseOnlyOpenWithMissingRoot()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("library"));
    const QString moved = temporary.filePath(QStringLiteral("library-offline"));
    const QString db = temporary.filePath(QStringLiteral("app/catalogue.sqlite"));
    writeFile(root + QStringLiteral("/AT800-01 - Artist - Song.mp3"), "audio");
    writeFile(root + QStringLiteral("/AT800-01 - Artist - Song.cdg"), "lyrics");
    runScan(db, root);
    QVERIFY(QDir().rename(root, moved));

    Catalogue catalogue(db);
    QString error;
    QVERIFY2(catalogue.open(&error), qPrintable(error));
    QCOMPARE(catalogue.stats(&error).value(QStringLiteral("songs")).toLongLong(), 1LL);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    catalogue.close();
    QVERIFY(QDir().rename(moved, root));
}

void TestCatalogue::resolverRulesAndEvaluation()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("library"));
    const QString db = temporary.filePath(QStringLiteral("app/catalogue.sqlite"));
    auto pair = [&](const QString& relative) {
        writeFile(root + QLatin1Char('/') + relative + QStringLiteral(".mp3"), "audio");
        writeFile(root + QLatin1Char('/') + relative + QStringLiteral(".cdg"), "lyrics");
    };
    pair(QStringLiteral("title-first/TA100-01 - Shared Trouble, A - North Band"));
    pair(QStringLiteral("title-first/TA100-02 - Echo, Echo - Baker, Alice"));
    pair(QStringLiteral("title-first/TA100-03 - 40,000 Stories - Carter, Beth"));
    pair(QStringLiteral("title-first/TA100-04 - Summer Song - Stone, Carol & Guest"));
    pair(QStringLiteral("artist-first/AT100-01 - VOCALS-North Band - Opening Song"));
    pair(QStringLiteral("artist-first/AT100-02 - Singer, Alice-VOCALS - Second Song"));
    pair(QStringLiteral("artist-first/AT100-03 - Solo - Bright Night"));
    pair(QStringLiteral("artist-first/AT100-04 - Player, Bob W-Vocal - Fourth Song"));
    pair(QStringLiteral("artist-first/AT100-05 - Surname, Given Feat.Guest Singer - Fifth Song"));
    pair(QStringLiteral("artist-first/AT100-06 - Gem Band [vocals] - Sixth Song"));
    pair(QStringLiteral("artist-first/AT100-07 - Nova, Mira (vocal) - Seventh Song"));
    pair(QStringLiteral("artist-first/AT100-08 - Echo Act W~vocal - Eighth Song"));
    pair(QStringLiteral("artist-first/AT100-09 - Choir Wvocal - Ninth Song"));
    pair(QStringLiteral("artist-first/AT100-10 - Stage Group marker - Tenth Song"));
    pair(QStringLiteral("artist-first/AT100-11 - lower group vocal - Eleventh Song"));
    pair(QStringLiteral("artist-first/AT100-12 - VOCALS-Front Group - Twelfth Song"));
    pair(QStringLiteral("artist-first/AT100-13 - Rear Group-VOCALS - Thirteenth Song"));
    pair(QStringLiteral("artist-first/AT100-14 - 14 Mercer, Rena - Fourteenth Song"));
    pair(QStringLiteral("artist-first/AT100-15 - Family, Given Jr. - Fifteenth Song"));
    pair(QStringLiteral("artist-first/AT100-16 - Family, Given Sr. - Sixteenth Song"));
    pair(QStringLiteral("artist-first/AT100-17 - Family, Given II - Seventeenth Song"));
    pair(QStringLiteral("artist-first/AT100-18 - Family, Given III - Eighteenth Song"));
    pair(QStringLiteral("title-first/TA100-05 - TA100-05-Fifth Song - Cedar Band"));
    pair(QStringLiteral("title-first/TA100-06 - Sixth Song TA100-06 - Birch Band"));
    pair(QStringLiteral("comma-title/Gentle, Guiding Beacon - Plain Band"));
    pair(QStringLiteral("recurrence-support/RS200-01 - Person, Ana - First Anchor"));
    pair(QStringLiteral("recurrence-support/RS200-02 - Person, Bea - Second Anchor"));
    pair(QStringLiteral("recurrence-support/RS200-03 - Person, Cia - Third Anchor"));
    pair(QStringLiteral("recurrence-support/RS200-04 - Repeat Act - Copper Song"));
    pair(QStringLiteral("recurrence-support/RS200-05 - Repeat Act - Silver Song"));
    pair(QStringLiteral("recurrence-support/RS200-06 - Repeat Act - Bronze Song"));
    pair(QStringLiteral("recurrence-disc/RD300-01 - First Label - Person, Ana"));
    pair(QStringLiteral("recurrence-disc/RD300-02 - Second Label - Person, Bea"));
    pair(QStringLiteral("recurrence-disc/RD300-03 - Third Label - Person, Cia"));
    pair(QStringLiteral("recurrence-disc/RD300-04 - Repeat Act - Golden Song"));
    pair(QStringLiteral("prior/MRH132-18 - Signal - Easy Love"));
    pair(QStringLiteral("isolated/Unknown Artist - Unknown Title"));
    QCOMPARE(runScan(db, root).value(QStringLiteral("status")).toString(),
             QStringLiteral("completed"));

    const QString markerConnection = QStringLiteral("slash-vocal-marker-test");
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                          markerConnection);
        database.setDatabaseName(db);
        QVERIFY(database.open());
        QSqlQuery update(database);
        update.prepare(QStringLiteral(
            "UPDATE sources SET parsed_json=json_set(parsed_json,'$.fields[0]',?) "
            "WHERE mp3_file_id=(SELECT id FROM files WHERE rel_path=?)"));
        update.addBindValue(QStringLiteral("Stage Group w/vocals"));
        update.addBindValue(QStringLiteral(
            "artist-first/AT100-10 - Stage Group marker - Tenth Song.mp3"));
        QVERIFY(update.exec());
        QCOMPARE(update.numRowsAffected(), 1);
        database.close();
    }
    QSqlDatabase::removeDatabase(markerConnection);

    Catalogue catalogue(db);
    QString error;
    QVERIFY2(catalogue.open(&error), qPrintable(error));
    QVERIFY2(MetadataResolver::resolve(catalogue, -1, &error), qPrintable(error));
    auto explained = [&](const QString& relative) {
        return catalogue.explain(relative + QStringLiteral(".mp3"), &error);
    };
    QVariantMap row = explained(QStringLiteral(
        "title-first/TA100-01 - Shared Trouble, A - North Band"));
    QCOMPARE(row.value(QStringLiteral("artist")).toString(), QStringLiteral("North Band"));
    QCOMPARE(row.value(QStringLiteral("title")).toString(), QStringLiteral("Shared Trouble, A"));
    QCOMPARE(row.value(QStringLiteral("confidence")).toString(), QStringLiteral("high"));
    row = explained(QStringLiteral("title-first/TA100-02 - Echo, Echo - Baker, Alice"));
    QCOMPARE(row.value(QStringLiteral("artist")).toString(), QStringLiteral("Alice Baker"));
    row = explained(QStringLiteral("artist-first/AT100-01 - VOCALS-North Band - Opening Song"));
    QCOMPARE(row.value(QStringLiteral("artist")).toString(), QStringLiteral("North Band"));
    row = explained(QStringLiteral("artist-first/AT100-02 - Singer, Alice-VOCALS - Second Song"));
    QCOMPARE(row.value(QStringLiteral("artist")).toString(), QStringLiteral("Alice Singer"));
    row = explained(QStringLiteral("artist-first/AT100-03 - Solo - Bright Night"));
    QCOMPARE(row.value(QStringLiteral("artist")).toString(), QStringLiteral("Solo"));
    QCOMPARE(row.value(QStringLiteral("confidence")).toString(), QStringLiteral("high"));
    row = explained(QStringLiteral("artist-first/AT100-04 - Player, Bob W-Vocal - Fourth Song"));
    QCOMPARE(row.value(QStringLiteral("artist")).toString(), QStringLiteral("Bob Player"));
    row = explained(QStringLiteral(
        "artist-first/AT100-05 - Surname, Given Feat.Guest Singer - Fifth Song"));
    QCOMPARE(row.value(QStringLiteral("artist")).toString(),
             QStringLiteral("Surname, Given Feat.Guest Singer"));
    const QList<QPair<QString, QString>> vocalCases = {
        {QStringLiteral("artist-first/AT100-06 - Gem Band [vocals] - Sixth Song"),
         QStringLiteral("Gem Band")},
        {QStringLiteral("artist-first/AT100-07 - Nova, Mira (vocal) - Seventh Song"),
         QStringLiteral("Mira Nova")},
        {QStringLiteral("artist-first/AT100-08 - Echo Act W~vocal - Eighth Song"),
         QStringLiteral("Echo Act")},
        {QStringLiteral("artist-first/AT100-09 - Choir Wvocal - Ninth Song"),
         QStringLiteral("Choir")},
        {QStringLiteral("artist-first/AT100-10 - Stage Group marker - Tenth Song"),
         QStringLiteral("Stage Group")},
        {QStringLiteral("artist-first/AT100-11 - lower group vocal - Eleventh Song"),
         QStringLiteral("lower group")},
        {QStringLiteral("artist-first/AT100-12 - VOCALS-Front Group - Twelfth Song"),
         QStringLiteral("Front Group")},
        {QStringLiteral("artist-first/AT100-13 - Rear Group-VOCALS - Thirteenth Song"),
         QStringLiteral("Rear Group")}};
    for (const auto& vocalCase : vocalCases)
        QCOMPARE(explained(vocalCase.first).value(QStringLiteral("artist")).toString(),
                 vocalCase.second);
    row = explained(QStringLiteral("artist-first/AT100-14 - 14 Mercer, Rena - Fourteenth Song"));
    QCOMPARE(row.value(QStringLiteral("artist")).toString(), QStringLiteral("Rena Mercer"));
    QCOMPARE(explained(QStringLiteral("artist-first/AT100-15 - Family, Given Jr. - Fifteenth Song"))
                 .value(QStringLiteral("artist")).toString(), QStringLiteral("Given Family Jr."));
    QCOMPARE(explained(QStringLiteral("artist-first/AT100-16 - Family, Given Sr. - Sixteenth Song"))
                 .value(QStringLiteral("artist")).toString(), QStringLiteral("Given Family Sr."));
    QCOMPARE(explained(QStringLiteral("artist-first/AT100-17 - Family, Given II - Seventeenth Song"))
                 .value(QStringLiteral("artist")).toString(), QStringLiteral("Given Family II"));
    QCOMPARE(explained(QStringLiteral("artist-first/AT100-18 - Family, Given III - Eighteenth Song"))
                 .value(QStringLiteral("artist")).toString(), QStringLiteral("Given Family III"));
    row = explained(QStringLiteral("title-first/TA100-05 - TA100-05-Fifth Song - Cedar Band"));
    QCOMPARE(row.value(QStringLiteral("title")).toString(), QStringLiteral("Fifth Song"));
    QCOMPARE(row.value(QStringLiteral("artist")).toString(), QStringLiteral("Cedar Band"));
    row = explained(QStringLiteral("title-first/TA100-06 - Sixth Song TA100-06 - Birch Band"));
    QCOMPARE(row.value(QStringLiteral("title")).toString(), QStringLiteral("Sixth Song"));
    row = explained(QStringLiteral("comma-title/Gentle, Guiding Beacon - Plain Band"));
    QCOMPARE(row.value(QStringLiteral("artist")).toString(),
             QStringLiteral("Gentle, Guiding Beacon"));
    row = explained(QStringLiteral("recurrence-disc/RD300-04 - Repeat Act - Golden Song"));
    QCOMPARE(row.value(QStringLiteral("artist")).toString(), QStringLiteral("Repeat Act"));
    QCOMPARE(row.value(QStringLiteral("confidence")).toString(), QStringLiteral("medium"));
    const QList<QVariantMap> samples = catalogue.sample(1000, {}, &error);
    auto rawByPath = [&](const QString& path) {
        for (const QVariantMap& sample : samples) {
            if (sample.value(QStringLiteral("path")).toString() == path)
                return sample;
        }
        return QVariantMap{};
    };
    QVariantMap raw = rawByPath(QStringLiteral(
        "title-first/TA100-05 - TA100-05-Fifth Song - Cedar Band.mp3"));
    QCOMPARE(raw.value(QStringLiteral("titleRaw")).toString(),
             QStringLiteral("TA100-05-Fifth Song"));
    QCOMPARE(raw.value(QStringLiteral("artistRaw")).toString(), QStringLiteral("Cedar Band"));
    raw = rawByPath(QStringLiteral(
        "artist-first/AT100-06 - Gem Band [vocals] - Sixth Song.mp3"));
    QCOMPARE(raw.value(QStringLiteral("titleRaw")).toString(), QStringLiteral("Sixth Song"));
    QCOMPARE(raw.value(QStringLiteral("artistRaw")).toString(),
             QStringLiteral("Gem Band [vocals]"));
    row = explained(QStringLiteral("prior/MRH132-18 - Signal - Easy Love"));
    QCOMPARE(row.value(QStringLiteral("artist")).toString(), QStringLiteral("Signal"));
    QCOMPARE(row.value(QStringLiteral("confidence")).toString(), QStringLiteral("low"));
    row = explained(QStringLiteral("isolated/Unknown Artist - Unknown Title"));
    QCOMPARE(row.value(QStringLiteral("artist")).toString(), QStringLiteral("Unknown Artist"));
    QCOMPARE(row.value(QStringLiteral("title")).toString(), QStringLiteral("Unknown Title"));
    QCOMPARE(row.value(QStringLiteral("confidence")).toString(), QStringLiteral("low"));

    const QString gold = temporary.filePath(QStringLiteral("gold.tsv"));
    writeFile(gold,
              "# rel_path\\tartist_field\n"
              "title-first/TA100-01 - Shared Trouble, A - North Band.mp3\t2\n"
              "isolated/Unknown Artist - Unknown Title.mp3\t1\n");
    const QVariantMap evaluation = CatalogueTools::evaluateGold(catalogue, gold, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(evaluation.value(QStringLiteral("evaluated")).toLongLong(), 2LL);
    QCOMPARE(evaluation.value(QStringLiteral("invalidRows")).toLongLong(), 0LL);
    QCOMPARE(evaluation.value(QStringLiteral("mismatches")).toList().size(), 0);
    const QVariantMap reparsed = CatalogueTools::reparse(catalogue, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(reparsed.value(QStringLiteral("reparsed")).toLongLong(), 37LL);
    QVERIFY(!reparsed.value(QStringLiteral("before")).toMap().isEmpty());
    QVERIFY(!reparsed.value(QStringLiteral("after")).toMap().isEmpty());
    catalogue.close();

    const QString connection = QStringLiteral("tag-evaluation-test");
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(db);
        QVERIFY(database.open());
        QSqlQuery songs(database);
        QVERIFY(songs.exec(QStringLiteral(
            "SELECT f.id,s.parsed_json FROM files f JOIN sources s ON s.mp3_file_id=f.id "
            "WHERE f.rel_dir='title-first'")));
        QSqlQuery update(database);
        update.prepare(QStringLiteral("UPDATE files SET raw_tags_json=?,tags_state='read' WHERE id=?"));
        while (songs.next()) {
            const QJsonArray fields = QJsonDocument::fromJson(songs.value(1).toByteArray())
                                          .object().value(QStringLiteral("fields")).toArray();
            QJsonObject tags;
            tags.insert(QStringLiteral("title"), fields.at(0).toString());
            tags.insert(QStringLiteral("artist"), fields.at(1).toString());
            update.bindValue(0, QString::fromUtf8(QJsonDocument(tags).toJson(QJsonDocument::Compact)));
            update.bindValue(1, songs.value(0));
            QVERIFY(update.exec());
        }
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
    QVERIFY2(catalogue.open(&error), qPrintable(error));
    const QVariantMap tagEvaluation = MetadataResolver::evaluateTags(catalogue, 0, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(tagEvaluation.value(QStringLiteral("eligible")).toLongLong(), 4LL);
    QCOMPARE(tagEvaluation.value(QStringLiteral("mismatchCount")).toLongLong(), 0LL);
    QCOMPARE(tagEvaluation.value(QStringLiteral("mismatchesListed")).toLongLong(), 0LL);
}

void TestCatalogue::searchPerformance()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString dbPath = temporary.filePath(QStringLiteral("catalogue.sqlite"));
    Catalogue catalogue(dbPath);
    QString error;
    QVERIFY2(catalogue.open(&error), qPrintable(error));
    catalogue.close();

    const QString connection = QStringLiteral("performance-test");
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(dbPath);
        QVERIFY(database.open());
        QVERIFY(database.transaction());
        QSqlQuery insert(database);
        insert.prepare(QStringLiteral("INSERT INTO songs(title,artist,title_raw,artist_raw,display_title,display_artist,search_text,metadata_source,confidence,playable) VALUES(?,?,?,?,?,?,?,?,?,1)"));
        for (int i = 0; i < 50000; ++i) {
            const QString title = QStringLiteral("Song %1 Needle").arg(i);
            const QString artist = QStringLiteral("Artist %1").arg(i % 500);
            const QVariantList values = {title, artist, title, artist, title, artist,
                                         title.toLower() + QLatin1Char(' ') + artist.toLower(),
                                         QStringLiteral("fallback"), QStringLiteral("none")};
            for (int column = 0; column < values.size(); ++column)
                insert.bindValue(column, values.at(column));
            QVERIFY(insert.exec());
        }
        QVERIFY(database.commit());
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
    QVERIFY(catalogue.open(&error));
    QElapsedTimer timer;
    timer.start();
    const QList<CatalogueSearchRow> results = catalogue.search(QStringLiteral("needle artist 42"), 100, false, &error);
    const qint64 elapsed = timer.elapsed();
    qInfo().noquote() << QStringLiteral("50,000-song token search: %1 ms").arg(elapsed);
    QVERIFY2(!results.isEmpty(), qPrintable(error));
    QVERIFY2(elapsed < 100, qPrintable(QStringLiteral("50k-song search took %1 ms").arg(elapsed)));
}

QTEST_GUILESS_MAIN(TestCatalogue)
#include "tst_catalogue.moc"
