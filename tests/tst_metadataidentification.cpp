#include "CdgTestData.h"
#include "cdg/CdgTitleFrames.h"
#include "library/Catalogue.h"
#include "library/CatalogueTools.h"
#include "library/TitleScreenText.h"
#include "library/LibraryScanner.h"
#include "library/MetadataResolver.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

#include <memory>

namespace {

void writeFile(const QString& path, const QByteArray& contents)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size())
        qFatal("Could not write synthetic fixture: %s", qPrintable(path));
}

// A synthetic CD+G stream: `packets` tile-block instructions whose data bytes
// depend on `seed`, so different seeds give different content of equal size.
QByteArray cdgStream(int packets, char seed)
{
    QByteArray data;
    data.reserve(packets * 24);
    for (int i = 0; i < packets; ++i) {
        QByteArray packet(24, '\0');
        packet[0] = 0x09;
        packet[1] = 6;
        for (int j = 4; j < 20; ++j)
            packet[j] = static_cast<char>((seed + i * 7 + j) & 0x3f);
        data.append(packet);
    }
    return data;
}

QByteArray mp3Audio(char seed, int size = 4096)
{
    return QByteArray(size, seed);
}

QByteArray id3Frame(const char* id, const QString& text)
{
    const QByteArray body = QByteArray(1, '\0') + text.toLatin1();
    QByteArray frame(id, 4);
    const quint32 size = quint32(body.size());
    frame.append(char((size >> 24) & 0xff)).append(char((size >> 16) & 0xff))
        .append(char((size >> 8) & 0xff)).append(char(size & 0xff));
    frame.append(QByteArray(2, '\0'));
    return frame + body;
}

// An ID3v2.3 tag in front of synthetic audio.
QByteArray taggedMp3(const QString& title, const QString& artist, char seed)
{
    QByteArray frames = id3Frame("TIT2", title) + id3Frame("TPE1", artist);
    if (title == QLatin1String("Track 10"))
        frames += id3Frame("TALB", QStringLiteral("Party Classics"))
            + id3Frame("TPE2", QStringLiteral("Various Artists")) + id3Frame("TRCK", QStringLiteral("14/20"));
    const quint32 size = quint32(frames.size());
    QByteArray tag("ID3\x03\0\0", 6);
    tag.append(char((size >> 21) & 0x7f)).append(char((size >> 14) & 0x7f))
        .append(char((size >> 7) & 0x7f)).append(char(size & 0x7f));
    return tag + frames + mp3Audio(seed);
}

void writeSong(const QString& root, const QString& stem, const QByteArray& mp3,
               const QByteArray& cdg)
{
    writeFile(root + QLatin1Char('/') + stem + QStringLiteral(".mp3"), mp3);
    writeFile(root + QLatin1Char('/') + stem + QStringLiteral(".cdg"), cdg);
}

QVariantMap runScan(const QString& databasePath, const QString& root, quint64* reads = nullptr,
                    bool readTags = false,
                    std::shared_ptr<TitleScreenOcrEngine> engine = nullptr)
{
    LibraryScanner scanner(databasePath);
    ScanOptions options;
    options.readTags = readTags;
    scanner.setOptions(options);
    scanner.setTitleScreenOcr(std::move(engine));  // as in the application
    QVariantMap summary;
    QString failure;
    QObject::connect(&scanner, &LibraryScanner::finished,
                     [&](const QVariantMap& value) { summary = value; });
    QObject::connect(&scanner, &LibraryScanner::failed,
                     [&](const QString& value) { failure = value; });
    scanner.scan(root);
    if (!failure.isEmpty())
        qFatal("Synthetic scan failed: %s", qPrintable(failure));
    if (reads)
        *reads = scanner.sourceFileReads();
    return summary;
}

struct SongRow {
    QString title;
    QString artist;
    QString source;
    QString confidence;
    bool conflict = false;
    QJsonObject evidence;
};

SongRow songFor(const QString& databasePath, const QString& relPath)
{
    const QString connection = QStringLiteral("identification-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    SongRow row;
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(databasePath);
        if (!database.open())
            qFatal("Could not inspect catalogue");
        QSqlQuery query(database);
        query.prepare(QStringLiteral(
            "SELECT so.display_title,so.display_artist,so.auto_source,so.auto_confidence,"
            "so.conflict,so.evidence_json FROM songs so JOIN sources s ON s.song_id=so.id "
            "JOIN files f ON f.id=s.mp3_file_id WHERE f.rel_path=?"));
        query.addBindValue(relPath);
        if (!query.exec() || !query.next())
            qFatal("Song not catalogued: %s", qPrintable(relPath));
        row = {query.value(0).toString(), query.value(1).toString(),
               query.value(2).toString(), query.value(3).toString(),
               query.value(4).toBool(),
               QJsonDocument::fromJson(query.value(5).toByteArray()).object()};
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
    return row;
}

// Every path, size and byte under a directory: proves a run wrote nothing there.
QMap<QString, QByteArray> treeSnapshot(const QString& root)
{
    QMap<QString, QByteArray> result;
    QDirIterator it(root, QDir::Files | QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        QFile file(path);
        QByteArray value = QByteArrayLiteral("dir");
        if (it.fileInfo().isFile() && file.open(QIODevice::ReadOnly))
            value = file.readAll();
        result.insert(QDir(root).relativeFilePath(path), value);
    }
    return result;
}

// Returns canned text for known frames, so the tests never depend on a real
// recogniser, and counts how often it is asked.
class FakeTitleScreenOcr final : public TitleScreenOcrEngine {
public:
    QString name() const override { return QStringLiteral("fake-ocr-1"); }
    bool recognise(const std::vector<std::uint32_t>& argb, int, int, QList<OcrLine>* lines,
                   QString*) override
    {
        ++calls;
        *lines = byFrame.value(frameKey(argb));
        return true;
    }
    static QByteArray frameKey(const std::vector<std::uint32_t>& argb)
    {
        return QByteArray(reinterpret_cast<const char*>(argb.data()),
                          qsizetype(argb.size() * sizeof(std::uint32_t)));
    }
    void add(const std::vector<std::uint8_t>& stream, const QStringList& texts)
    {
        const std::vector<cdg::TitleFrame> frames = cdg::findTitleFrames(stream);
        if (frames.empty())
            qFatal("Synthetic title screen has no frame");
        QList<OcrLine> lines;
        double y = 0.1;
        for (const QString& text : texts) {
            OcrLine line;
            line.text = text;
            line.confidence = 1.0;
            line.y = y;
            line.height = text == text.toUpper() && !text.startsWith(QLatin1String("IN THE")) ? 0.1 : 0.05;
            line.width = 0.6;
            lines.append(line);
            y += 0.15;
        }
        byFrame.insert(frameKey(frames.front().pixels), lines);
    }
    QHash<QByteArray, QList<OcrLine>> byFrame;
    int calls = 0;
};

QVariantMap runReprocess(const QString& databasePath, std::shared_ptr<TitleScreenOcrEngine> engine)
{
    LibraryScanner scanner(databasePath);
    ScanOptions options;
    options.readTitleScreens = true;
    scanner.setOptions(options);
    scanner.setTitleScreenOcr(std::move(engine));
    QVariantMap summary;
    QString failure;
    QObject::connect(&scanner, &LibraryScanner::finished,
                     [&](const QVariantMap& value) { summary = value; });
    QObject::connect(&scanner, &LibraryScanner::failed,
                     [&](const QString& value) { failure = value; });
    scanner.prepareScan();
    scanner.reprocessMetadata();
    if (!failure.isEmpty())
        qFatal("Synthetic reprocess failed: %s", qPrintable(failure));
    return summary;
}

qint64 count(const QString& databasePath, const QString& sql)
{
    const QString connection = QStringLiteral("count-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    qint64 result = -1;
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(databasePath);
        if (database.open()) {
            QSqlQuery query(database);
            if (query.exec(sql) && query.next())
                result = query.value(0).toLongLong();
        }
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
    return result;
}

void deleteCatalogue(const QString& path)
{
    for (const QString& suffix : {QString(), QStringLiteral("-wal"), QStringLiteral("-shm")})
        QFile::remove(path + suffix);
}

} // namespace

class TestMetadataIdentification : public QObject {
    Q_OBJECT

private slots:
    void provenDuplicateNamesUnresolvedCopy();
    void trackListsNameFieldlessSongs();
    void cleanTagsOnlySuggestForFieldlessSongs();
    void repairsNamesWithCodesAndBareHyphens();
    void titleScreensNameUnresolvedSongs();
    void labelsKeepVersionsApart();
    void weakEvidenceNeverMakesHighConfidence();
    void titleScreenReadingsSurviveCatalogueRebuild();
    void enrichmentCacheMigratesAndIsNeverErased();
};

void TestMetadataIdentification::provenDuplicateNamesUnresolvedCopy()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("library"));
    const QString path = temporary.filePath(QStringLiteral("app/library.sqlite"));

    // A well-named copy (personal-name field -> medium on its own) and a bare
    // numeric copy of exactly the same CDG and audio.
    const QByteArray song = cdgStream(1500, 'a');
    writeSong(root, QStringLiteral("Named/DUP01-01 - Sinatra, Frank - My Way"),
              mp3Audio('m'), song);
    writeSong(root, QStringLiteral("Numbers/3902"), mp3Audio('m'), song);
    // Same audio bytes, retagged: the identity ignores ID3 metadata.
    writeSong(root, QStringLiteral("Numbers/3903"),
              QByteArray("ID3\x03\0\0\0\0\0\x0a", 10) + QByteArray(10, 't') + mp3Audio('m'),
              song);
    // The same CDG size with different lyrics: a size collision, never a match.
    writeSong(root, QStringLiteral("Numbers/3904"), mp3Audio('m'), cdgStream(1500, 'b'));
    // A blank stream shared by a named and a numeric song proves nothing,
    // nor does one made only of presets and palette loads (no drawing).
    const QByteArray blank(1500 * 24, '\0');
    QByteArray presets;
    for (int i = 0; i < 1500; ++i) {
        QByteArray packet(24, '\0');
        packet[0] = 0x09;
        packet[1] = char(i % 2 ? 1 : 30);
        presets.append(packet);
    }
    writeSong(root, QStringLiteral("Named/DUP07-01 - Dion, Celine - My Heart Will Go On"),
              mp3Audio('x'), presets);
    writeSong(root, QStringLiteral("Numbers/3908"), mp3Audio('y'), presets);
    // Valid tile blocks that draw nothing visible: a pattern in one colour.
    QByteArray inert;
    for (int i = 0; i < 1500; ++i) {
        QByteArray packet(24, '\0');
        packet[0] = 0x09;
        packet[1] = 6;
        packet[4] = 3;
        packet[5] = 3;
        for (int row = 8; row < 20; ++row)
            packet[row] = char(row % 2 ? 0x2a : 0x15);
        inert.append(packet);
    }
    writeSong(root, QStringLiteral("Named/DUP08-01 - Houston, Whitney - I Will Always Love You"),
              mp3Audio('u'), inert);
    writeSong(root, QStringLiteral("Numbers/3909"), mp3Audio('v'), inert);
    writeSong(root, QStringLiteral("Named/DUP02-01 - Crosby, Bing - White Christmas"),
              mp3Audio('w'), blank);
    writeSong(root, QStringLiteral("Numbers/3905"), mp3Audio('w'), blank);
    // Two named copies of one CDG disagree: the numeric copy is not named.
    const QByteArray disputed = cdgStream(1600, 'c');
    writeSong(root, QStringLiteral("Named/DUP03-01 - Presley, Elvis - Suspicious Minds"),
              mp3Audio('s'), disputed);
    writeSong(root, QStringLiteral("Named/DUP04-01 - Cash, Johnny - Ring Of Fire"),
              mp3Audio('s'), disputed);
    writeSong(root, QStringLiteral("Numbers/3906"), mp3Audio('s'), disputed);
    // A weak single-field title that contradicts a proven copy is kept and flagged.
    const QByteArray contradicted = cdgStream(1700, 'd');
    writeSong(root, QStringLiteral("Named/DUP05-01 - Nelson, Willie - Crazy"),
              mp3Audio('c'), contradicted);
    writeSong(root, QStringLiteral("Loose/Something Else"), mp3Audio('c'), contradicted);
    // Only the CDG matches (a different MP3 encode): named, but less certain.
    const QByteArray reencoded = cdgStream(1800, 'e');
    writeSong(root, QStringLiteral("Named/DUP06-01 - Cline, Patsy - Walkin' After Midnight"),
              mp3Audio('p'), reencoded);
    writeSong(root, QStringLiteral("Numbers/3907"), mp3Audio('q'), reencoded);

    const QMap<QString, QByteArray> before = treeSnapshot(root);
    quint64 reads = 0;
    const QVariantMap summary = runScan(path, root, &reads);
    QCOMPARE(summary.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
    QVERIFY(reads > 0);
    QCOMPARE(treeSnapshot(root), before);

    SongRow named = songFor(path, QStringLiteral("Numbers/3902.mp3"));
    QCOMPARE(named.title, QStringLiteral("My Way"));
    QCOMPARE(named.artist, QStringLiteral("Frank Sinatra"));
    QCOMPARE(named.source, QStringLiteral("exact_duplicate"));
    QCOMPARE(named.confidence, QStringLiteral("medium"));  // never above the donor
    QVERIFY(!named.conflict);
    const QJsonArray evidence = named.evidence.value(QStringLiteral("evidence")).toArray();
    QVERIFY(!evidence.isEmpty());
    QCOMPARE(evidence.last().toObject().value(QStringLiteral("source")).toString(),
             QStringLiteral("exact_duplicate"));
    QCOMPARE(songFor(path, QStringLiteral("Numbers/3903.mp3")).title, QStringLiteral("My Way"));

    const SongRow collision = songFor(path, QStringLiteral("Numbers/3904.mp3"));
    QCOMPARE(collision.confidence, QStringLiteral("unresolved"));
    QCOMPARE(collision.source, QStringLiteral("fallback"));

    const SongRow blankCopy = songFor(path, QStringLiteral("Numbers/3905.mp3"));
    QCOMPARE(blankCopy.confidence, QStringLiteral("unresolved"));
    QCOMPARE(songFor(path, QStringLiteral("Numbers/3908.mp3")).confidence, QStringLiteral("unresolved"));
    QCOMPARE(songFor(path, QStringLiteral("Numbers/3909.mp3")).confidence, QStringLiteral("unresolved"));

    const SongRow disputedCopy = songFor(path, QStringLiteral("Numbers/3906.mp3"));
    QCOMPARE(disputedCopy.confidence, QStringLiteral("unresolved"));
    QVERIFY(disputedCopy.conflict);

    const SongRow contradictedCopy = songFor(path, QStringLiteral("Loose/Something Else.mp3"));
    QCOMPARE(contradictedCopy.title, QStringLiteral("Something Else"));
    QCOMPARE(contradictedCopy.confidence, QStringLiteral("low"));
    QVERIFY(contradictedCopy.conflict);

    const SongRow cdgOnly = songFor(path, QStringLiteral("Numbers/3907.mp3"));
    QCOMPARE(cdgOnly.title, QStringLiteral("Walkin' After Midnight"));
    QCOMPARE(cdgOnly.confidence, QStringLiteral("low"));

    // The donors themselves are untouched, and a rescan reads nothing again.
    QCOMPARE(songFor(path, QStringLiteral("Named/DUP01-01 - Sinatra, Frank - My Way.mp3")).source,
             QStringLiteral("filename"));
    quint64 rescanReads = 0;
    runScan(path, root, &rescanReads);
    QCOMPARE(rescanReads, 0ULL);
    QCOMPARE(songFor(path, QStringLiteral("Numbers/3902.mp3")).title, QStringLiteral("My Way"));
    QCOMPARE(treeSnapshot(root), before);
}

void TestMetadataIdentification::trackListsNameFieldlessSongs()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("library"));
    const QString path = temporary.filePath(QStringLiteral("app/library.sqlite"));
    char seed = 'a';
    for (int track = 1; track <= 4; ++track) {
        writeSong(root, QStringLiteral("KPP SERIES/KPP01/KPP01-%1").arg(track, 2, 10, QLatin1Char('0')),
                  mp3Audio(seed), cdgStream(10, seed));
        ++seed;
    }
    writeFile(root + QStringLiteral("/KPP SERIES/KPP01/KPP01.txt"),
              "REM Text File Generated By MP3+G Toolz\r\n\r\n"
              "01. Kpp01-01 Martin, Ricky - Livin' La Vida Loca\r\n"
              "02. Kpp01-02 Dion, Celine - My Heart Will Go On\r\n"
              "03. Kpp01-03 Carlisle, Bob - Butterfly Kisses\r\n");
    // A download note beside other numbered songs is not a track list.
    for (int track = 1; track <= 3; ++track) {
        writeSong(root, QStringLiteral("SC/SC8819/SC8819-%1").arg(track, 2, 10, QLatin1Char('0')),
                  mp3Audio(seed), cdgStream(10, seed));
        ++seed;
    }
    writeFile(root + QStringLiteral("/SC/SC8819/note.txt"),
              "This disc is downloaded from #cd+g\r\nSC8819-01 - Jackson, Alan - Chattahoochee\r\n");
    // A list that names an already named song differently is a conflict.
    writeSong(root, QStringLiteral("DK/DK001/DK001-01 - Crazy - Nelson, Willie"),
              mp3Audio(seed), cdgStream(10, seed));
    ++seed;
    writeSong(root, QStringLiteral("DK/DK001/DK001-02 - Jolene - Parton, Dolly"),
              mp3Audio(seed), cdgStream(10, seed));
    writeFile(root + QStringLiteral("/DK/DK001/DK001.txt"),
              "DK001-01 - Walk On By - Warwick, Dionne\r\n"
              "DK001-02 - Jolene - Parton, Dolly\r\n"
              "DK001-03 - Stand By Me - King, Ben E.\r\n");

    const QMap<QString, QByteArray> before = treeSnapshot(root);
    const QVariantMap summary = runScan(path, root);
    QCOMPARE(summary.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
    QCOMPARE(summary.value(QStringLiteral("counts")).toMap()
                 .value(QStringLiteral("sidecarTrackLists")).toLongLong(), 2LL);
    QCOMPARE(treeSnapshot(root), before);

    const SongRow first = songFor(path, QStringLiteral("KPP SERIES/KPP01/KPP01-01.mp3"));
    QCOMPARE(first.title, QStringLiteral("Livin' La Vida Loca"));
    QCOMPARE(first.artist, QStringLiteral("Ricky Martin"));
    QCOMPARE(first.source, QStringLiteral("sidecar_track_list"));
    QVERIFY(first.confidence == QLatin1String("high") || first.confidence == QLatin1String("medium"));
    const QJsonArray evidence = first.evidence.value(QStringLiteral("evidence")).toArray();
    bool listed = false;
    for (const QJsonValue& value : evidence)
        listed = listed || value.toObject().value(QStringLiteral("path")).toString()
            == QLatin1String("KPP SERIES/KPP01/KPP01.txt");
    QVERIFY(listed);
    // Track 4 is not in the list: still an honest fallback.
    const SongRow unlisted = songFor(path, QStringLiteral("KPP SERIES/KPP01/KPP01-04.mp3"));
    QCOMPARE(unlisted.confidence, QStringLiteral("unresolved"));
    QCOMPARE(unlisted.title, QStringLiteral("Disc KPP01 - Track 04"));

    QCOMPARE(songFor(path, QStringLiteral("SC/SC8819/SC8819-01.mp3")).confidence,
             QStringLiteral("unresolved"));

    const SongRow disputed = songFor(path, QStringLiteral("DK/DK001/DK001-01 - Crazy - Nelson, Willie.mp3"));
    QCOMPARE(disputed.title, QStringLiteral("Crazy"));  // the file's own name is kept
    QVERIFY(disputed.conflict);
    QVERIFY(disputed.confidence != QLatin1String("high"));
    const SongRow agreed = songFor(path, QStringLiteral("DK/DK001/DK001-02 - Jolene - Parton, Dolly.mp3"));
    QVERIFY(!agreed.conflict);

    // Unchanged lists are not read again by a rescan.
    quint64 reads = 0;
    runScan(path, root, &reads);
    QCOMPARE(reads, 0ULL);
}

void TestMetadataIdentification::cleanTagsOnlySuggestForFieldlessSongs()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("library"));
    const QString path = temporary.filePath(QStringLiteral("app/library.sqlite"));
    auto song = [&](const QString& stem, const QString& title, const QString& artist, char seed) {
        writeSong(root, stem, taggedMp3(title, artist, seed), cdgStream(20 + seed % 7, seed));
    };
    song(QStringLiteral("SF 127/14"), QStringLiteral("Sweetest Thing"), QStringLiteral("U2"), 'a');
    song(QStringLiteral("SF 127/15"), QStringLiteral("moonlighting"), QStringLiteral("SFMW834"), 'b');
    song(QStringLiteral("SF 127/16"), QStringLiteral("Dance Hall Days"), QStringLiteral("Sunfly"), 'c');
    song(QStringLiteral("SF 127/17"), QStringLiteral("Track 10"), QStringLiteral("Artist"), 'd');
    song(QStringLiteral("SF 127/18"), QStringLiteral("001"), QStringLiteral("country house"), 'e');
    // A named file keeps its own name: the tag is only supporting evidence.
    song(QStringLiteral("SF 127/SF127-19 - Oasis - Wonderwall"), QStringLiteral("Wrong Title"),
         QStringLiteral("Wrong Artist"), 'f');

    QCOMPARE(runScan(path, root, nullptr, true).value(QStringLiteral("status")).toString(),
             QStringLiteral("completed"));
    const SongRow tagged = songFor(path, QStringLiteral("SF 127/14.mp3"));
    QCOMPARE(tagged.title, QStringLiteral("Sweetest Thing"));
    QCOMPARE(tagged.artist, QStringLiteral("U2"));
    QCOMPARE(tagged.source, QStringLiteral("id3"));
    QCOMPARE(tagged.confidence, QStringLiteral("low"));
    for (const QString& junk : {QStringLiteral("SF 127/15.mp3"), QStringLiteral("SF 127/16.mp3"),
                                QStringLiteral("SF 127/17.mp3"), QStringLiteral("SF 127/18.mp3")}) {
        const SongRow row = songFor(path, junk);
        QCOMPARE(row.confidence, QStringLiteral("unresolved"));
        QCOMPARE(row.source, QStringLiteral("fallback"));
    }
    const SongRow named = songFor(path, QStringLiteral("SF 127/SF127-19 - Oasis - Wonderwall.mp3"));
    QCOMPARE(named.title, QStringLiteral("Wonderwall"));
    QCOMPARE(named.artist, QStringLiteral("Oasis"));

    // Raw tag values stay searchable even when they are not displayed.
    Catalogue catalogue(path);
    QString error;
    QVERIFY2(catalogue.open(&error), qPrintable(error));
    QCOMPARE(catalogue.search(QStringLiteral("moonlighting"), 10, true, &error).size(), 1);
    QCOMPARE(catalogue.search(QStringLiteral("sweetest thing u2"), 10, true, &error).size(), 1);
    QCOMPARE(catalogue.search(QStringLiteral("party classics"), 10, true, &error).size(), 1);
    QCOMPARE(catalogue.search(QStringLiteral("various artists 14 20"), 10, true, &error).size(), 1);
}

void TestMetadataIdentification::repairsNamesWithCodesAndBareHyphens()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("library"));
    const QString path = temporary.filePath(QStringLiteral("app/library.sqlite"));
    char seed = 'a';
    auto song = [&](const QString& stem) {
        writeSong(root, stem, mp3Audio(seed), cdgStream(5, seed));
        ++seed;
    };
    // Catalogue codes and track numbers in name positions.
    song(QStringLiteral("Added/KV-127451 - Denver, John - Grandma's Feather Bed"));
    song(QStringLiteral("Elvis/MMVE3-32 - 08 - Presley, Elvis - Today, Tomorrow And Forever"));
    song(QStringLiteral("Mixed/Level 42 - Lessons In Love - XY-0012"));
    // A number in a name position may be the title ("3").
    song(QStringLiteral("CB/Cb30117-10 - Spears, Britney - 3 - V"));
    // A short code repeated as the first field in a folder is a label; a
    // repeated plain name is that folder's artist.
    song(QStringLiteral("Irish/KV - Ryan, Derek - Blue"));
    song(QStringLiteral("Irish/KV - Denver, Mike - Boston Rose"));
    song(QStringLiteral("Irish/KV - Carter, Nathan - South Australia"));
    song(QStringLiteral("Movies/Mmve20-09 - Presley, Elvis - Didja Ever - G.I. Blues"));
    song(QStringLiteral("Movies/Mmve20-10 - Presley, Elvis - Big Boots - G.I. Blues"));
    song(QStringLiteral("Movies/Mmve20-11 - Presley, Elvis - Frankfurt Special - G.I. Blues"));
    // Bare hyphens: split when the disc's own names show the order...
    song(QStringLiteral("Zoom/Zmp57-01-Everly Brothers-Bye Bye Love"));
    song(QStringLiteral("Zoom/Zmp57-02-Everly Brothers-Wake Up Little Susie"));
    song(QStringLiteral("Zoom/Zmp57-03-Everly Brothers-Cathy's Clown"));
    // ...but a lone guess keeps its whole name as a weak title, and codes are
    // never split off as names.
    song(QStringLiteral("Grab Bag/Saturday Night-Whigfield"));
    song(QStringLiteral("Grab Bag/Why Have You Left The One-Gayle Crystal Band"));
    song(QStringLiteral("SF Gold 19/sf-gold19_13_eric_clapton_layla"));

    QCOMPARE(runScan(path, root).value(QStringLiteral("status")).toString(),
             QStringLiteral("completed"));
    auto names = [&](const QString& file) {
        const SongRow row = songFor(path, file + QStringLiteral(".mp3"));
        return QStringList{row.artist, row.title, row.confidence};
    };
    QStringList row = names(QStringLiteral("Added/KV-127451 - Denver, John - Grandma's Feather Bed"));
    QCOMPARE(row.mid(0, 2), QStringList({QStringLiteral("John Denver"),
                                         QStringLiteral("Grandma's Feather Bed")}));
    QVERIFY(row.at(2) != QLatin1String("high"));
    QCOMPARE(names(QStringLiteral("Elvis/MMVE3-32 - 08 - Presley, Elvis - Today, Tomorrow And Forever")).mid(0, 2),
             QStringList({QStringLiteral("Elvis Presley"), QStringLiteral("Today, Tomorrow And Forever")}));
    row = names(QStringLiteral("Mixed/Level 42 - Lessons In Love - XY-0012"));
    QVERIFY(row.at(0) == QLatin1String("Level 42") || row.at(1) == QLatin1String("Level 42"));
    QCOMPARE(names(QStringLiteral("CB/Cb30117-10 - Spears, Britney - 3 - V")).at(1), QStringLiteral("3"));
    QCOMPARE(names(QStringLiteral("Irish/KV - Ryan, Derek - Blue")).mid(0, 2),
             QStringList({QStringLiteral("Derek Ryan"), QStringLiteral("Blue")}));
    QCOMPARE(names(QStringLiteral("Movies/Mmve20-09 - Presley, Elvis - Didja Ever - G.I. Blues")).mid(0, 2),
             QStringList({QStringLiteral("Elvis Presley"), QStringLiteral("Didja Ever")}));
    QCOMPARE(names(QStringLiteral("Zoom/Zmp57-02-Everly Brothers-Wake Up Little Susie")),
             QStringList({QStringLiteral("Everly Brothers"), QStringLiteral("Wake Up Little Susie"),
                          QStringLiteral("medium")}));
    QCOMPARE(names(QStringLiteral("Grab Bag/Saturday Night-Whigfield")),
             QStringList({QString(), QStringLiteral("Saturday Night-Whigfield"), QStringLiteral("low")}));
    // Two clear-looking names, but nothing says which is the artist.
    QCOMPARE(names(QStringLiteral("Grab Bag/Why Have You Left The One-Gayle Crystal Band")),
             QStringList({QString(), QStringLiteral("Why Have You Left The One-Gayle Crystal Band"),
                          QStringLiteral("low")}));
    row = names(QStringLiteral("SF Gold 19/sf-gold19_13_eric_clapton_layla"));
    QVERIFY(row.at(0) != QLatin1String("sf") && row.at(1) != QLatin1String("sf"));
}

void TestMetadataIdentification::titleScreensNameUnresolvedSongs()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("library"));
    const QString path = temporary.filePath(QStringLiteral("app/library.sqlite"));
    auto screen = [](int column) {
        const std::vector<std::uint8_t> data = testcdg::titleScreenStream(std::uint8_t(column));
        return QByteArray(reinterpret_cast<const char*>(data.data()), qsizetype(data.size()));
    };
    auto bytes = [](const QByteArray& data) {
        const auto* first = reinterpret_cast<const std::uint8_t*>(data.constData());
        return std::vector<std::uint8_t>(first, first + data.size());
    };
    // Well-named songs make the dictionary of known titles and artists.
    writeSong(root, QStringLiteral("Named/SF001-01 - Presley, Elvis - Suspicious Minds"),
              mp3Audio('a'), screen(1));
    writeSong(root, QStringLiteral("Named/SF001-05 - Houston, Whitney - One Moment In Time"),
              mp3Audio('b'), screen(2));
    // Unresolved songs whose title screens say things.
    const QByteArray validated = screen(3);
    const QByteArray corrected = screen(4);
    const QByteArray styled = screen(5);
    const QByteArray sibling = screen(6);
    const QByteArray unknown = screen(7);
    writeSong(root, QStringLiteral("SF 123/05"), mp3Audio('c'), validated);
    writeSong(root, QStringLiteral("SF 123/06"), mp3Audio('d'), corrected);
    writeSong(root, QStringLiteral("Numbers/7001"), mp3Audio('e'), styled);
    writeSong(root, QStringLiteral("SF001/05"), mp3Audio('f'), sibling);
    writeSong(root, QStringLiteral("SF 123/07"), mp3Audio('g'), unknown);
    writeSong(root, QStringLiteral("SF 123/08"), mp3Audio('h'),
              QByteArray(3000 * 24, '\0'));  // no title screen at all

    auto engine = std::make_shared<FakeTitleScreenOcr>();
    engine->add(bytes(validated), {QStringLiteral("SUNFLY"), QStringLiteral("SUSPICIOUS MINDS"),
                                   QStringLiteral("©1996 Sunfly Ltd")});
    engine->add(bytes(corrected), {QStringLiteral("SUNFLY"), QStringLiteral("SUSPICIOUS MlNDS")});
    engine->add(bytes(styled), {QStringLiteral("ONE MOMENT IN TIME"), QStringLiteral("IN THE STYLE OF"),
                                QStringLiteral("WHITNEY HOUSTON")});
    engine->add(bytes(sibling), {QStringLiteral("SUNFLY"), QStringLiteral("ONE MOMENT IN TIME")});
    engine->add(bytes(unknown), {QStringLiteral("SUNFLY"), QStringLiteral("LOVD CATS")});

    const QMap<QString, QByteArray> before = treeSnapshot(root);
    QCOMPARE(runScan(path, root).value(QStringLiteral("status")).toString(),
             QStringLiteral("completed"));
    QCOMPARE(songFor(path, QStringLiteral("SF 123/05.mp3")).confidence, QStringLiteral("unresolved"));
    QCOMPARE(runReprocess(path, engine).value(QStringLiteral("status")).toString(),
             QStringLiteral("completed"));
    QCOMPARE(treeSnapshot(root), before);
    const int calls = engine->calls;
    QCOMPARE(calls, 5);  // one frame per title screen; none for the blank CDG

    SongRow row = songFor(path, QStringLiteral("SF 123/05.mp3"));
    QCOMPARE(row.title, QStringLiteral("Suspicious Minds"));  // the collection's own spelling
    QCOMPARE(row.artist, QString());                          // screens rarely name the singer
    QCOMPARE(row.source, QStringLiteral("cdg_ocr"));
    QCOMPARE(row.confidence, QStringLiteral("medium"));
    row = songFor(path, QStringLiteral("SF 123/06.mp3"));
    QCOMPARE(row.title, QStringLiteral("Suspicious Minds"));
    QCOMPARE(row.confidence, QStringLiteral("medium"));
    row = songFor(path, QStringLiteral("Numbers/7001.mp3"));
    QCOMPARE(row.title, QStringLiteral("One Moment In Time"));
    QCOMPARE(row.artist, QStringLiteral("Whitney Houston"));
    QCOMPARE(row.confidence, QStringLiteral("medium"));
    row = songFor(path, QStringLiteral("SF001/05.mp3"));
    QCOMPARE(row.title, QStringLiteral("One Moment In Time"));
    QCOMPARE(row.artist, QStringLiteral("Whitney Houston"));
    QCOMPARE(row.source, QStringLiteral("combined_evidence"));
    QCOMPARE(row.confidence, QStringLiteral("high"));
    // Unconfirmed text is never shown, but it can still be searched for.
    row = songFor(path, QStringLiteral("SF 123/07.mp3"));
    QCOMPARE(row.confidence, QStringLiteral("unresolved"));
    QCOMPARE(row.title, QStringLiteral("Disc SF123 - Track 07"));
    QCOMPARE(songFor(path, QStringLiteral("SF 123/08.mp3")).confidence, QStringLiteral("unresolved"));
    {
        Catalogue catalogue(path);
        QString error;
        QVERIFY2(catalogue.open(&error), qPrintable(error));
        QCOMPARE(catalogue.search(QStringLiteral("lovd cats"), 10, true, &error).size(), 1);
        // Results travel to another installation by content, not by path.
        const QString exported = temporary.filePath(QStringLiteral("app/title-screens.json"));
        QVERIFY(CatalogueTools::exportTitleScreens(catalogue, exported, &error)
                    .value(QStringLiteral("exported")).toInt() == 6);
        QVERIFY(CatalogueTools::exportTitleScreens(catalogue, root + QStringLiteral("/x.json"), &error)
                    .isEmpty());  // never written into the music folder
        QVERIFY(!QFileInfo::exists(root + QStringLiteral("/x.json")));
    }

    // Cached: reprocessing again recognises nothing new.
    runReprocess(path, engine);
    QCOMPARE(engine->calls, calls);

    // A fresh catalogue with no OCR engine gets the same names from an import.
    const QString other = temporary.filePath(QStringLiteral("other/library.sqlite"));
    runScan(other, root);
    {
        Catalogue catalogue(other);
        QString error;
        QVERIFY2(catalogue.open(&error), qPrintable(error));
        QCOMPARE(CatalogueTools::importTitleScreens(
                     catalogue, temporary.filePath(QStringLiteral("app/title-screens.json")), &error)
                     .value(QStringLiteral("imported")).toInt(), 6);
    }
    runReprocess(other, nullptr);
    QCOMPARE(songFor(other, QStringLiteral("SF001/05.mp3")).confidence, QStringLiteral("high"));
    QCOMPARE(songFor(other, QStringLiteral("SF 123/05.mp3")).title, QStringLiteral("Suspicious Minds"));
    QCOMPARE(treeSnapshot(root), before);
}

void TestMetadataIdentification::labelsKeepVersionsApart()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("library"));
    const QString path = temporary.filePath(QStringLiteral("app/library.sqlite"));
    // The same song on two labels, with different content: two songs.
    writeSong(root, QStringLiteral("Sunfly/Sunfly main series/SF 123/SF123-04 - Johnny Cash - Ring Of Fire"),
              mp3Audio('a'), cdgStream(40, 'a'));
    writeSong(root, QStringLiteral("Legends/Legends/LEG 056/LEG056-09 - Johnny Cash - Ring Of Fire"),
              mp3Audio('b'), cdgStream(41, 'b'));
    // A Chartbuster disc filed in the Sunfly folder gets no label.
    writeSong(root, QStringLiteral("Sunfly/extras/CB30117-10 - Cash, Johnny - Hurt"),
              mp3Audio('c'), cdgStream(42, 'c'));
    QCOMPARE(runScan(path, root).value(QStringLiteral("status")).toString(),
             QStringLiteral("completed"));
    Catalogue catalogue(path);
    QString error;
    QVERIFY2(catalogue.open(&error), qPrintable(error));
    const QList<CatalogueSearchRow> versions = catalogue.search(QStringLiteral("ring of fire"), 10, true, &error);
    QCOMPARE(versions.size(), 2);
    QVERIFY(versions.at(0).songId != versions.at(1).songId);
    QSet<QString> labels;
    for (const CatalogueSearchRow& row : versions) {
        QCOMPARE(row.displayArtist, QStringLiteral("Johnny Cash"));
        QCOMPARE(row.displayTitle, QStringLiteral("Ring Of Fire"));
        labels.insert(row.label + QLatin1Char('|') + row.discId);
    }
    QCOMPARE(labels, QSet<QString>({QStringLiteral("Sunfly|SF123"), QStringLiteral("Legends|LEG056")}));
    QCOMPARE(catalogue.search(QStringLiteral("legends ring of fire"), 10, true, &error).size(), 1);
    QCOMPARE(catalogue.search(QStringLiteral("sunfly ring of fire"), 10, true, &error).size(), 1);
    // Folder names stay searchable too, so the Chartbuster disc in the Sunfly
    // folder is found by "sunfly" even though it has no label.
    QCOMPARE(catalogue.search(QStringLiteral("sunfly cash"), 10, true, &error).size(), 2);
    const QList<CatalogueSearchRow> hurt = catalogue.search(QStringLiteral("hurt"), 10, true, &error);
    QCOMPARE(hurt.size(), 1);
    QCOMPARE(hurt.first().label, QString());
    const auto ref = catalogue.songRef(versions.first().songId, &error);
    QVERIFY(ref);
    QVERIFY(!ref->label.isEmpty());
}

void TestMetadataIdentification::weakEvidenceNeverMakesHighConfidence()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("library"));
    const QString path = temporary.filePath(QStringLiteral("app/library.sqlite"));
    char seed = 'a';
    auto tagged = [&](const QString& stem, const QString& title, const QString& artist) {
        writeSong(root, stem, taggedMp3(title, artist, seed), cdgStream(30 + seed % 9, seed));
        ++seed;
    };
    auto plain = [&](const QString& stem) {
        writeSong(root, stem, mp3Audio(seed), cdgStream(30 + seed % 9, seed));
        ++seed;
    };
    // Two ID3 tags that merely repeat the first field must not make a third
    // song on the disc HIGH (review finding 6).
    tagged(QStringLiteral("XX/XX001-01 - Alpha - Song One"), QStringLiteral("Song One"), QStringLiteral("Alpha"));
    tagged(QStringLiteral("XX/XX001-02 - Beta - Song Two"), QStringLiteral("Song Two"), QStringLiteral("Beta"));
    plain(QStringLiteral("XX/XX001-03 - The Third Song - Example Band"));
    // An underscore used as a space in a title is not a field separator,
    // however strongly the disc's order is known (finding 7).
    plain(QStringLiteral("YY/YY002-01 - Presley, Elvis - Suspicious Minds"));
    plain(QStringLiteral("YY/YY002-02 - Parton, Dolly - Jolene"));
    plain(QStringLiteral("YY/YY002-03 - Bridge_Over Troubled Water"));
    // Three underscore names on one disc may split each other, but they never
    // set the order for a plainly separated name on that disc (re-review 3).
    plain(QStringLiteral("ZZ/ZZ001-01 - Bridge_Across River"));
    plain(QStringLiteral("ZZ/ZZ001-02 - Bridge_Under Moonlight"));
    plain(QStringLiteral("ZZ/ZZ001-03 - Bridge_Beyond Tomorrow"));
    plain(QStringLiteral("ZZ/ZZ001-04 - Actual Song - Example Band"));
    // Right after the disc/track code, underscores are the name's separator.
    plain(QStringLiteral("FIK/FIK015_06_Westlife_Uptown Girl"));
    // A Sunfly list says nothing about a Sound Choice disc with the same
    // number (finding 9), but SUNFLY/SF is the same label.
    plain(QStringLiteral("Mixed/SC123-01"));
    plain(QStringLiteral("Mixed/SF123-02"));
    writeFile(root + QStringLiteral("/Mixed/list.txt"),
              "SF123-01 - Presley, Elvis - Suspicious Minds\r\n"
              "SF123-02 - Parton, Dolly - Jolene\r\n"
              "SF123-03 - Cash, Johnny - Hurt\r\n");
    plain(QStringLiteral("Sunfly/Sunfly-063-04"));
    writeFile(root + QStringLiteral("/Sunfly/tracks.txt"),
              "SF 063 - 03 - Crazy - Nelson, Willie\r\n"
              "SF 063 - 04 - Jolene - Parton, Dolly\r\n"
              "SF 063 - 05 - Hurt - Cash, Johnny\r\n");

    QCOMPARE(runScan(path, root, nullptr, true).value(QStringLiteral("status")).toString(),
             QStringLiteral("completed"));
    QVERIFY(songFor(path, QStringLiteral("XX/XX001-03 - The Third Song - Example Band.mp3")).confidence
            != QLatin1String("high"));
    const SongRow bridge = songFor(path, QStringLiteral("YY/YY002-03 - Bridge_Over Troubled Water.mp3"));
    QVERIFY(bridge.artist != QLatin1String("Bridge"));
    QCOMPARE(bridge.title, QStringLiteral("Bridge Over Troubled Water"));
    QVERIFY(bridge.confidence != QLatin1String("high"));
    const SongRow plainName = songFor(path, QStringLiteral("ZZ/ZZ001-04 - Actual Song - Example Band.mp3"));
    QVERIFY(plainName.confidence != QLatin1String("high"));
    QVERIFY(plainName.evidence.value(QStringLiteral("rule")).toString() != QLatin1String("filename_disc_rule"));
    QVERIFY(songFor(path, QStringLiteral("ZZ/ZZ001-01 - Bridge_Across River.mp3")).confidence
            != QLatin1String("high"));
    QCOMPARE(songFor(path, QStringLiteral("FIK/FIK015_06_Westlife_Uptown Girl.mp3")).title,
             QStringLiteral("Uptown Girl"));
    QCOMPARE(songFor(path, QStringLiteral("Mixed/SC123-01.mp3")).confidence, QStringLiteral("unresolved"));
    QCOMPARE(songFor(path, QStringLiteral("Mixed/SF123-02.mp3")).title, QStringLiteral("Jolene"));
    QCOMPARE(songFor(path, QStringLiteral("Sunfly/Sunfly-063-04.mp3")).title, QStringLiteral("Jolene"));
}

void TestMetadataIdentification::titleScreenReadingsSurviveCatalogueRebuild()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("library"));
    const QString path = temporary.filePath(QStringLiteral("app/library.sqlite"));
    const QString cache = temporary.filePath(QStringLiteral("app/enrichment-cache.sqlite"));
    auto screen = [](int column) {
        const std::vector<std::uint8_t> data = testcdg::titleScreenStream(std::uint8_t(column));
        return QByteArray(reinterpret_cast<const char*>(data.data()), qsizetype(data.size()));
    };
    auto bytes = [](const QByteArray& data) {
        const auto* first = reinterpret_cast<const std::uint8_t*>(data.constData());
        return std::vector<std::uint8_t>(first, first + data.size());
    };
    // Named songs padded with empty packets: no other CDG shares a size with
    // an unresolved one, so only the title-screen cache can reconnect them.
    writeSong(root, QStringLiteral("Named/SF001-01 - Presley, Elvis - Suspicious Minds"),
              mp3Audio('a'), screen(1) + QByteArray(24, '\0'));
    writeSong(root, QStringLiteral("Named/SF001-05 - Houston, Whitney - One Moment In Time"),
              mp3Audio('b'), screen(2) + QByteArray(48, '\0'));
    const QByteArray validated = screen(3);
    const QByteArray sibling = screen(6) + QByteArray(72, '\0');
    writeSong(root, QStringLiteral("SF 123/05"), mp3Audio('c'), validated);
    writeSong(root, QStringLiteral("SF001/05"), mp3Audio('f'), sibling);
    // An identical CDG elsewhere is recognised once, not twice.
    writeSong(root, QStringLiteral("Copies/77"), mp3Audio('k'), validated);
    auto engine = std::make_shared<FakeTitleScreenOcr>();
    engine->add(bytes(validated), {QStringLiteral("SUNFLY"), QStringLiteral("SUSPICIOUS MINDS")});
    engine->add(bytes(sibling), {QStringLiteral("SUNFLY"), QStringLiteral("ONE MOMENT IN TIME")});

    // 1. The title screens are read once.
    runScan(path, root);
    runReprocess(path, engine);
    const int calls = engine->calls;
    QCOMPARE(calls, 2);
    QCOMPARE(songFor(path, QStringLiteral("SF 123/05.mp3")).title, QStringLiteral("Suspicious Minds"));
    QCOMPARE(songFor(path, QStringLiteral("SF001/05.mp3")).confidence, QStringLiteral("high"));
    // The readings live in the enrichment cache beside the catalogue, not in it.
    QCOMPARE(count(cache, QStringLiteral("SELECT count(*) FROM title_screens")), 2);
    QCOMPARE(count(path, QStringLiteral("SELECT count(*) FROM title_screens")), 0);
    // A song added later, unresolved but of a size no cached reading has: a
    // scan never reads it for title screens.
    writeSong(root, QStringLiteral("SF 123/08"), mp3Audio('h'), QByteArray(3001 * 24, '\0'));
    const QMap<QString, QByteArray> before = treeSnapshot(root);

    // 2-3. The catalogue is deleted and rebuilt by an ordinary scan: the songs
    //      reconnect to their readings by CDG content, with no recognition.
    deleteCatalogue(path);
    QVERIFY(!QFileInfo::exists(path));
    quint64 reads = 0;
    QCOMPARE(runScan(path, root, &reads, false, engine).value(QStringLiteral("status")).toString(),
             QStringLiteral("completed"));
    QCOMPARE(engine->calls, calls);
    QCOMPARE(count(path, QStringLiteral("SELECT count(*) FROM files WHERE rel_path='SF 123/08.cdg' "
                                        "AND quick_sha256 IS NULL")), 1);
    SongRow row = songFor(path, QStringLiteral("SF 123/05.mp3"));
    QCOMPARE(row.title, QStringLiteral("Suspicious Minds"));
    QCOMPARE(row.source, QStringLiteral("cdg_ocr"));
    QCOMPARE(row.confidence, QStringLiteral("medium"));
    QCOMPARE(songFor(path, QStringLiteral("SF001/05.mp3")).title, QStringLiteral("One Moment In Time"));
    QCOMPARE(songFor(path, QStringLiteral("SF001/05.mp3")).confidence, QStringLiteral("high"));
    // 4-5. Reprocessing (new resolver rules) re-reads the cached text only.
    runReprocess(path, engine);
    QCOMPARE(engine->calls, calls);
    QCOMPARE(songFor(path, QStringLiteral("SF 123/05.mp3")).title, QStringLiteral("Suspicious Minds"));
    // A rescan with fingerprints known reads nothing more for title screens.
    quint64 again = 0;
    runScan(path, root, &again, false, engine);
    QVERIFY2(again < reads, qPrintable(QStringLiteral("%1 %2").arg(again).arg(reads)));
    QCOMPARE(treeSnapshot(root), before);

    // Moving and renaming the files keeps the reading (content, not name).
    QVERIFY(QDir().mkpath(root + QStringLiteral("/Moved")));
    for (const char* ext : {".mp3", ".cdg"})
        QVERIFY(QFile::rename(root + QStringLiteral("/SF 123/05") + QLatin1String(ext),
                              root + QStringLiteral("/Moved/9999") + QLatin1String(ext)));
    runScan(path, root, nullptr, false, engine);
    QCOMPARE(songFor(path, QStringLiteral("Moved/9999.mp3")).title, QStringLiteral("Suspicious Minds"));
    QCOMPARE(engine->calls, calls);

    // Export still works, from the cache; an import lands in the importing
    // installation's own cache, so it too survives a catalogue rebuild.
    const QString exported = temporary.filePath(QStringLiteral("app/title-screens.json"));
    {
        Catalogue catalogue(path);
        QString error;
        QVERIFY2(catalogue.open(&error), qPrintable(error));
        QVERIFY(catalogue.enrichmentCacheIsDurable());
        QCOMPARE(CatalogueTools::exportTitleScreens(catalogue, exported, &error)
                     .value(QStringLiteral("exported")).toInt(), 3);  // incl. "no title frame"
    }
    const QString other = temporary.filePath(QStringLiteral("other/library.sqlite"));
    runScan(other, root);
    {
        Catalogue catalogue(other);
        QString error;
        QVERIFY2(catalogue.open(&error), qPrintable(error));
        QCOMPARE(CatalogueTools::importTitleScreens(catalogue, exported, &error)
                     .value(QStringLiteral("imported")).toInt(), 3);
    }
    QCOMPARE(count(temporary.filePath(QStringLiteral("other/enrichment-cache.sqlite")),
                   QStringLiteral("SELECT count(*) FROM title_screens")), 3);
    deleteCatalogue(other);
    runScan(other, root, nullptr, false, engine);
    QCOMPARE(songFor(other, QStringLiteral("Moved/9999.mp3")).title, QStringLiteral("Suspicious Minds"));
    QCOMPARE(engine->calls, calls);
}

void TestMetadataIdentification::enrichmentCacheMigratesAndIsNeverErased()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path = temporary.filePath(QStringLiteral("app/library.sqlite"));
    const QString cache = temporary.filePath(QStringLiteral("app/enrichment-cache.sqlite"));
    {
        Catalogue catalogue(path);
        QVERIFY(catalogue.open());
    }
    // Readings stored inside the catalogue by an earlier build move to the cache.
    for (const QString& suffix : {QString(), QStringLiteral("-wal"), QStringLiteral("-shm")})
        QFile::remove(cache + suffix);
    {
        const QString connection = QStringLiteral("legacy-screens");
        {
            QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
            database.setDatabaseName(path);
            QVERIFY(database.open());
            QSqlQuery query(database);
            QVERIFY(query.exec(QStringLiteral(
                "INSERT INTO title_screens VALUES(randomblob(32),1234,'engine','ok','[]',1)")));
            database.close();
        }
        QSqlDatabase::removeDatabase(connection);
    }
    {
        Catalogue catalogue(path);
        QVERIFY(catalogue.open());
        QVERIFY(catalogue.enrichmentCacheIsDurable());
    }
    QCOMPARE(count(cache, QStringLiteral("SELECT count(*) FROM title_screens")), 1);
    QCOMPARE(count(path, QStringLiteral("SELECT count(*) FROM main.title_screens")), 0);

    // A damaged cache is never emptied in place: background work leaves it
    // alone (and works on without it); the application sets it aside.
    for (const QString& suffix : {QString(), QStringLiteral("-wal"), QStringLiteral("-shm")})
        QFile::remove(cache + suffix);
    writeFile(cache, QByteArray("definitely not sqlite"));
    {
        Catalogue worker(path);
        QVERIFY(worker.open());
        QVERIFY(!worker.enrichmentCacheIsDurable());
    }
    QCOMPARE(QFile(cache).size(), qint64(21));
    {
        Catalogue application(path);
        application.setCorruptionRecoveryAllowed(true);
        QVERIFY(application.open());
        QVERIFY(application.enrichmentCacheIsDurable());
    }
    QCOMPARE(QDir(temporary.filePath(QStringLiteral("app")))
                 .entryList({QStringLiteral("enrichment-cache.sqlite.corrupt-*")}, QDir::Files).size(), 1);
}

QTEST_GUILESS_MAIN(TestMetadataIdentification)
#include "tst_metadataidentification.moc"
