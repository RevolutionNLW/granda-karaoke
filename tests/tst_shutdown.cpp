// Closing the program while the library scanner is busy.
//
// The release-blocking crash: closing during a metadata reprocess on a large
// library. The resolver's in-memory stages never looked at the stop request,
// so the scanner thread ran on for seconds, ~LibraryController gave up after
// 5 s, terminate() could not stop it, and destroying the running QThread
// aborted the program. These tests run the resolver on a realistic large
// synthetic catalogue (never real media) and stop it inside each stage.

#include "LibraryController.h"
#include "Shutdown.h"
#include "cdg/CdgTitleFrames.h"
#include "library/Catalogue.h"
#include "library/CatalogueTools.h"
#include "library/LibraryScanner.h"
#include "library/MetadataResolver.h"

#include "CdgTestData.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

#include <atomic>

namespace {

constexpr qint64 kStride = 1000000;  // id offset between cloned copies
constexpr int kCopies = 1500;        // 12 songs each: 18,000 songs
// Generous for sanitizer builds; the real catalogue stops within ~70 ms.
constexpr qint64 kPromptMs = 400;

bool writeFile(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QByteArray cdgBytes(std::uint8_t column)
{
    const std::vector<std::uint8_t> data = testcdg::titleScreenStream(column);
    return QByteArray(reinterpret_cast<const char*>(data.data()), qsizetype(data.size()));
}

// Runs fn with a private connection to the database at path.
template <typename Fn>
bool withDatabase(const QString& path, Fn fn)
{
    const QString name = QStringLiteral("fixture-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    bool ok = false;
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        database.setDatabaseName(path);
        if (database.open())
            ok = fn(database);
        database.close();
    }
    QSqlDatabase::removeDatabase(name);
    return ok;
}

bool exec(QSqlDatabase& database, const QString& sql)
{
    QSqlQuery query(database);
    if (query.exec(sql))
        return true;
    qWarning().noquote() << "Fixture SQL failed:" << query.lastError().text() << sql.left(200);
    return false;
}

QVariant scalar(const QString& path, const QString& sql)
{
    QVariant value;
    withDatabase(path, [&](QSqlDatabase& database) {
        QSqlQuery query(database);
        if (query.exec(sql) && query.next())
            value = query.value(0);
        return true;
    });
    return value;
}

QString meta(const QString& path, const QString& key)
{
    return scalar(path, QStringLiteral("SELECT value FROM catalogue_meta WHERE key='%1'").arg(key)).toString();
}

// Copies every base row (id < kStride) of a table kCopies times, each copy in
// its own folder, with the given column expressions (k is the copy number).
bool cloneRows(QSqlDatabase& database, const QString& table, const QHash<QString, QString>& overrides)
{
    QStringList columns;
    QStringList values;
    QSqlQuery info(database);
    if (!info.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table)))
        return false;
    while (info.next()) {
        const QString column = info.value(1).toString();
        columns.append(column);
        values.append(overrides.value(column, column));
    }
    return exec(database, QStringLiteral(
        "WITH RECURSIVE n(k) AS (SELECT 1 UNION ALL SELECT k+1 FROM n WHERE k<%1) "
        "INSERT INTO %2(%3) SELECT %4 FROM %2, n WHERE %2.id<%5")
        .arg(kCopies).arg(table, columns.join(QLatin1Char(',')), values.join(QLatin1Char(',')))
        .arg(kStride));
}

// A realistic large catalogue in a couple of seconds: a small synthetic
// library is scanned for real, then its rows are cloned in SQL into many
// folders. Named songs feed the title dictionary; unresolved ones have
// title-screen readings and one has a proven duplicate, so every resolver
// stage has work proportional to the library.
bool buildFixture(const QString& dir)
{
    const QString root = QDir(dir).filePath(QStringLiteral("music"));
    const QStringList named = {
        QStringLiteral("SF001-01 - Presley, Elvis - Suspicious Minds"),
        QStringLiteral("SF001-02 - Houston, Whitney - One Moment In Time"),
        QStringLiteral("SF001-03 - Queen - Bohemian Rhapsody"),
        QStringLiteral("SF001-04 - Abba - Dancing Queen"),
        QStringLiteral("SF001-05 - Oasis - Wonderwall"),
        QStringLiteral("SF001-06 - Adele - Hello")};
    const QStringList titles = {QStringLiteral("SUSPICIOUS MINDS"), QStringLiteral("ONE MOMENT IN TIME"),
                                QStringLiteral("BOHEMIAN RHAPSODY"), QStringLiteral("DANCING QUEEN"),
                                QStringLiteral("WONDERWALL")};
    for (int i = 0; i < named.size(); ++i) {
        const QString stem = root + QStringLiteral("/Named/") + named.at(i);
        if (!writeFile(stem + QStringLiteral(".mp3"), QByteArray(4096, char('a' + i)))
            || !writeFile(stem + QStringLiteral(".cdg"), cdgBytes(std::uint8_t(1 + i))))
            return false;
    }
    for (int i = 1; i <= 6; ++i) {
        const QString stem = root + QStringLiteral("/SF 123/0%1").arg(i);
        if (!writeFile(stem + QStringLiteral(".mp3"), QByteArray(4096, char('m' + i)))
            || !writeFile(stem + QStringLiteral(".cdg"), cdgBytes(std::uint8_t(10 + i))))
            return false;
    }
    const QString path = QDir(dir).filePath(QStringLiteral("app/library.sqlite"));
    QDir().mkpath(QFileInfo(path).absolutePath());
    {
        LibraryScanner scanner(path);
        ScanOptions options;
        options.readTags = false;
        options.identifyDuplicates = false;
        scanner.setOptions(options);
        QString failure;
        QObject::connect(&scanner, &LibraryScanner::failed, [&](const QString& value) { failure = value; });
        scanner.scan(root);
        if (!failure.isEmpty()) {
            qWarning().noquote() << "Fixture scan failed:" << failure;
            return false;
        }
    }
    const QString cache = QDir(dir).filePath(QStringLiteral("app/enrichment-cache.sqlite"));
    bool ok = withDatabase(path, [&](QSqlDatabase& database) {
        QSqlQuery attach(database);
        attach.prepare(QStringLiteral("ATTACH DATABASE ? AS enrich"));
        attach.addBindValue(cache);
        if (!attach.exec())
            return false;
        // Title-screen readings for five unresolved songs, as a previous
        // maintenance run would have stored them.
        if (!exec(database, QStringLiteral("UPDATE files SET quick_sha256=randomblob(32) WHERE kind='cdg'")))
            return false;
        for (int i = 0; i < titles.size(); ++i) {
            const QString frames = QStringLiteral(
                R"([{"timeMs":1000,"lines":[{"text":"SUNFLY","confidence":1,"box":[0.1,0.1,0.8,0.1]},)"
                R"({"text":"%1","confidence":1,"box":[0.1,0.45,0.8,0.1]}]}])").arg(titles.at(i));
            if (!exec(database, QStringLiteral(
                    "INSERT INTO enrich.title_screens SELECT quick_sha256,size,'test','ok','%1',1 "
                    "FROM files WHERE rel_path='SF 123/0%2.cdg'").arg(frames).arg(i + 1)))
                return false;
        }
        // "SF 123/06" is a proven copy of Adele - Hello.
        if (!exec(database, QStringLiteral(
                "UPDATE files SET content_sha256=X'0102030405',cdg_packets=5000 "
                "WHERE rel_path IN ('SF 123/06.cdg','Named/SF001-06 - Adele - Hello.cdg')")))
            return false;
        const QString offset = QStringLiteral("+k*%1").arg(kStride);
        if (!database.transaction()
            || !cloneRows(database, QStringLiteral("files"), {
                   {QStringLiteral("id"), QStringLiteral("id") + offset},
                   {QStringLiteral("rel_path"), QStringLiteral("'c'||k||'/'||rel_path")},
                   {QStringLiteral("rel_dir"), QStringLiteral("'c'||k||'/'||rel_dir")},
                   {QStringLiteral("content_sha256"),
                    QStringLiteral("CASE WHEN content_sha256 IS NULL THEN NULL "
                                   "ELSE CAST(k||'-'||hex(content_sha256) AS BLOB) END")}})
            || !cloneRows(database, QStringLiteral("songs"), {
                   {QStringLiteral("id"), QStringLiteral("id") + offset},
                   {QStringLiteral("best_source_id"), QStringLiteral("best_source_id") + offset}})
            || !cloneRows(database, QStringLiteral("sources"), {
                   {QStringLiteral("id"), QStringLiteral("id") + offset},
                   {QStringLiteral("song_id"), QStringLiteral("song_id") + offset},
                   {QStringLiteral("mp3_file_id"), QStringLiteral("mp3_file_id") + offset},
                   {QStringLiteral("graphics_file_id"), QStringLiteral("graphics_file_id") + offset}})
            || !database.commit())
            return false;
        // A track list beside the unresolved disc, so the sidecar stage has
        // entries to match against every song.
        if (!exec(database, QStringLiteral(
                "INSERT INTO files(root_id,rel_path,rel_dir,file_name,ext,kind,size,mtime_ms,present) "
                "SELECT root_id,'SF 123/SF123.txt','SF 123','SF123.txt','txt','sidecar',100,0,1 "
                "FROM files WHERE rel_path='SF 123/01.cdg'"))
            || !exec(database, QStringLiteral(
                "INSERT INTO sidecar_files(file_id,size,mtime_ms,state,disc_id) "
                "SELECT id,100,0,'track_list','SF123' FROM files WHERE rel_path='SF 123/SF123.txt'"))
            || !exec(database, QStringLiteral(
                "WITH RECURSIVE t(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM t WHERE n<6) "
                "INSERT INTO sidecar_entries(file_id,disc_id,track,fields_json,line) "
                "SELECT f.id,'SF123',n,'[\"Singer '||n||'\",\"Listed Song '||n||'\"]',n "
                "FROM files f, t WHERE f.rel_path='SF 123/SF123.txt'")))
            return false;
        // Written by older rules: exactly what makes the program reprocess at
        // start-up. reprocess_pending starts off; the resolver sets it as its
        // very first step, so tests can see the moment it has started.
        return exec(database, QStringLiteral("UPDATE songs SET resolver_version=0"))
            && exec(database, QStringLiteral("UPDATE catalogue_meta SET value='4' WHERE key='resolver_version'"))
            && exec(database, QStringLiteral("INSERT OR REPLACE INTO catalogue_meta VALUES('reprocess_pending','0')"))
            && exec(database, QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)"));
    });
    ok = ok && withDatabase(cache, [](QSqlDatabase& database) {
        return exec(database, QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)"));
    });
    return ok;
}

// The resolver's first step marks the reprocess pending (the fixture starts
// with it off): seeing it proves the scanner is inside the resolver.
bool waitUntilResolving(const QString& path, int timeoutMs = 20000)
{
    QElapsedTimer timer;
    timer.start();
    while (meta(path, QStringLiteral("reprocess_pending")) != QLatin1String("1")) {
        if (timer.elapsed() > timeoutMs)
            return false;
        QTest::qWait(5);
    }
    return true;
}

bool copyFixture(const QString& from, const QString& to)
{
    QDir().mkpath(to);
    for (const QString& name : {QStringLiteral("library.sqlite"), QStringLiteral("enrichment-cache.sqlite")}) {
        QFile::remove(QDir(to).filePath(name));
        if (!QFile::copy(QDir(from).filePath(name), QDir(to).filePath(name)))
            return false;
    }
    return true;
}

} // namespace

class TestShutdown : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void resolverStopsPromptlyInsideEveryStage_data();
    void resolverStopsPromptlyInsideEveryStage();
    void titleScreenReadingStopsWithinAFrame();
    void reparsingStoredNamesStopsPromptlyAndChangesNothing_data();
    void reparsingStoredNamesStopsPromptlyAndChangesNothing();
    void closingDuringAReprocessNeverDestroysARunningScanner();
    void destroyingTheLibraryAloneAlsoStopsItPromptly();
    void repeatedLaunchAndQuit();
    void closingEndsTheProgramNormallyWhileTheResolverIsBusy();
    void aStuckScannerEndsTheProgramWithoutDestroyingAnything();

private:
    QString freshCopy();

    QTemporaryDir m_dir;
    QString m_fixture;  // folder holding the pristine library.sqlite + enrichment cache
    int m_songs = 0;
    int m_copy = 0;
};

void TestShutdown::initTestCase()
{
    QVERIFY(m_dir.isValid());
    QElapsedTimer timer;
    timer.start();
    QVERIFY(buildFixture(m_dir.filePath(QStringLiteral("fixture"))));
    m_fixture = m_dir.filePath(QStringLiteral("fixture/app"));
    const QString path = QDir(m_fixture).filePath(QStringLiteral("library.sqlite"));
    m_songs = scalar(path, QStringLiteral("SELECT count(*) FROM songs")).toInt();
    QCOMPARE(m_songs, 12 * (kCopies + 1));
    QCOMPARE(scalar(QDir(m_fixture).filePath(QStringLiteral("enrichment-cache.sqlite")),
                    QStringLiteral("SELECT count(*) FROM title_screens")).toInt(), 5);
    qInfo() << "Fixture:" << m_songs << "songs built in" << timer.elapsed() << "ms";
}

QString TestShutdown::freshCopy()
{
    const QString to = m_dir.filePath(QStringLiteral("run-%1/app").arg(++m_copy));
    if (!copyFixture(m_fixture, to))
        return {};
    return QDir(to).filePath(QStringLiteral("library.sqlite"));
}

void TestShutdown::resolverStopsPromptlyInsideEveryStage_data()
{
    QTest::addColumn<QString>("stage");
    QTest::addColumn<int>("after");  // stop when this many items of the stage were checked
    // At the very first check of each stage: no stage has an unchecked start.
    for (const char* stage : {"candidates", "sidecars", "names", "evidence", "base", "duplicates",
                              "title_screens", "write"})
        QTest::newRow(qPrintable(QStringLiteral("entering %1").arg(QLatin1String(stage))))
            << QString::fromLatin1(stage) << 1;
    // Deep inside each stage.
    QTest::newRow("loading candidates") << QStringLiteral("candidates") << 5000;
    QTest::newRow("track lists") << QStringLiteral("sidecars") << 9000;
    QTest::newRow("repairing names") << QStringLiteral("names") << 20000;
    QTest::newRow("preparing evidence") << QStringLiteral("evidence") << 30000;
    QTest::newRow("base names") << QStringLiteral("base") << 5000;
    QTest::newRow("duplicate evidence") << QStringLiteral("duplicates") << 800;
    QTest::newRow("title-screen rows") << QStringLiteral("title_screens") << 7000;
    QTest::newRow("title-screen dictionary") << QStringLiteral("title_screens") << 15000;
    QTest::newRow("title-screen readings") << QStringLiteral("title_screens") << 27000;
    QTest::newRow("writing (mid-batch)") << QStringLiteral("write") << 1250;
}

void TestShutdown::resolverStopsPromptlyInsideEveryStage()
{
    QFETCH(QString, stage);
    QFETCH(int, after);
    const QString path = freshCopy();
    QVERIFY(!path.isEmpty());

    // A full run first, on another copy: every stage is consulted in
    // proportion to its work, so a stage without checks would show here.
    if (QByteArray(QTest::currentDataTag()) == "entering candidates") {
        const QString full = freshCopy();
        Catalogue catalogue(full);
        QVERIFY(catalogue.open());
        QHash<QString, int> checks;
        QString current;
        MetadataResolver::Options options;
        options.stageStarted = [&](const QString& name) { current = name; };
        options.cancelled = [&] { ++checks[current]; return false; };
        QElapsedTimer timer;
        timer.start();
        QCOMPARE(MetadataResolver::resolve(catalogue, -1, options), MetadataResolver::Status::Completed);
        qInfo() << "Uncancelled resolve of" << m_songs << "songs:" << timer.elapsed() << "ms; checks" << checks;
        for (const char* name : {"candidates", "sidecars", "names", "evidence", "base", "write"})
            QVERIFY2(checks.value(QString::fromLatin1(name)) >= m_songs, name);
        QVERIFY(checks.value(QStringLiteral("duplicates")) >= kCopies);
        QVERIFY(checks.value(QStringLiteral("title_screens")) >= 5 * kCopies);
        QCOMPARE(meta(full, QStringLiteral("reprocess_pending")), QStringLiteral("0"));
        QCOMPARE(meta(full, QStringLiteral("resolver_version")), QString::number(MetadataResolver::Version));
    }

    Catalogue catalogue(path);
    QVERIFY(catalogue.open());
    QString current;
    int seen = 0;
    QElapsedTimer sinceStop;
    MetadataResolver::Options options;
    options.stageStarted = [&](const QString& name) { current = name; };
    options.cancelled = [&] {
        if (sinceStop.isValid())
            return true;
        if (current == stage && ++seen >= after) {
            sinceStop.start();  // the program asks to stop here
            return true;
        }
        return false;
    };
    QString error;
    const MetadataResolver::Status status = MetadataResolver::resolve(catalogue, -1, options, &error);
    const qint64 stopMs = sinceStop.isValid() ? sinceStop.elapsed() : -1;
    QVERIFY2(sinceStop.isValid(), qPrintable(QStringLiteral("never reached %1 x%2").arg(stage).arg(after)));
    QCOMPARE(status, MetadataResolver::Status::Cancelled);
    QVERIFY(error.isEmpty());
    qInfo().noquote() << stage << "stopped" << stopMs << "ms after being asked";
    QVERIFY2(stopMs < kPromptMs, qPrintable(QString::number(stopMs)));

    // Nothing half-done is left behind: the write lock is free at once
    // (no transaction left open), no partial batch, the work still pending.
    QVERIFY2(withDatabase(path, [](QSqlDatabase& other) {
                 return exec(other, QStringLiteral("PRAGMA busy_timeout=0"))
                     && exec(other, QStringLiteral("BEGIN IMMEDIATE"))
                     && exec(other, QStringLiteral("ROLLBACK"));
             }),
             "the cancelled resolve left a write transaction open");
    catalogue.close();
    QCOMPARE(scalar(path, QStringLiteral("PRAGMA integrity_check")).toString(), QStringLiteral("ok"));
    QCOMPARE(meta(path, QStringLiteral("reprocess_pending")), QStringLiteral("1"));
    QCOMPARE(meta(path, QStringLiteral("resolver_version")), QStringLiteral("4"));
    const int written = scalar(path, QStringLiteral("SELECT count(*) FROM songs WHERE resolver_version=%1")
                                         .arg(MetadataResolver::Version)).toInt();
    if (stage == QLatin1String("write")) {
        // Only whole batches of 500 written before the stop are kept; the
        // open one is dropped whole (1250th row: two batches kept).
        QCOMPARE(written, ((after - 1) / 500) * 500);
    } else {
        QCOMPARE(written, 0);
    }

    // The next run picks the work up and finishes it.
    Catalogue again(path);
    QVERIFY(again.open());
    QCOMPARE(MetadataResolver::resolve(again, -1, MetadataResolver::Options{}), MetadataResolver::Status::Completed);
    again.close();
    QCOMPARE(meta(path, QStringLiteral("reprocess_pending")), QStringLiteral("0"));
    QCOMPARE(scalar(path, QStringLiteral("SELECT count(*) FROM songs WHERE resolver_version=%1")
                              .arg(MetadataResolver::Version)).toInt(), m_songs);
}

void TestShutdown::titleScreenReadingStopsWithinAFrame()
{
    // Reading one title screen decodes up to 45 s of the CDG; a stop request
    // is seen at the next 100 ms step of the stream.
    const std::vector<std::uint8_t> stream = testcdg::titleScreenStream(3, 40, 50);
    QVERIFY(!cdg::findTitleFrames(stream).empty());
    int asked = 0;
    const std::vector<cdg::TitleFrame> frames = cdg::findTitleFrames(stream, 45000, 3, [&asked] {
        return ++asked >= 3;
    });
    QVERIFY(frames.empty());
    QCOMPARE(asked, 3);
}

void TestShutdown::closingDuringAReprocessNeverDestroysARunningScanner()
{
    // The crash itself: the program closes while the start-up reprocess is
    // deep inside the resolver on a large library.
    const QString path = freshCopy();
    QVERIFY(!path.isEmpty());
    auto controller = std::make_unique<LibraryController>(path);
    QVERIFY(controller->isAvailable());
    QTRY_VERIFY(controller->isScanning());  // the queued start-up reprocess
    QVERIFY(controller->scannerRunning());
    QVERIFY(waitUntilResolving(path));       // inside the resolver now
    QTest::qWait(150);                       // deep in its in-memory stages (they take seconds)

    QElapsedTimer timer;
    timer.start();
    QVERIFY(controller->stopScanner(5000));
    const qint64 stopMs = timer.elapsed();
    qInfo() << "Scanner stopped" << stopMs << "ms after closing";
    QVERIFY2(stopMs < kPromptMs, qPrintable(QString::number(stopMs)));
    QVERIFY(!controller->scannerRunning());  // joined before anything is destroyed
    QVERIFY(controller->stopScanner(0));     // asking again is harmless
    controller->requestMetadataReprocess();  // and no new work is taken
    QVERIFY(!controller->scannerRunning());
    timer.restart();
    controller.reset();
    QVERIFY2(timer.elapsed() < 2000, qPrintable(QString::number(timer.elapsed())));

    // It was stopped part-way: nothing new committed as finished, the
    // catalogue intact, and the reprocess still pending for next time.
    QCOMPARE(scalar(path, QStringLiteral("PRAGMA integrity_check")).toString(), QStringLiteral("ok"));
    QCOMPARE(meta(path, QStringLiteral("reprocess_pending")), QStringLiteral("1"));
    QCOMPARE(meta(path, QStringLiteral("resolver_version")), QStringLiteral("4"));
}

void TestShutdown::destroyingTheLibraryAloneAlsoStopsItPromptly()
{
    const QString path = freshCopy();
    auto controller = std::make_unique<LibraryController>(path);
    QVERIFY(waitUntilResolving(path));
    QElapsedTimer timer;
    timer.start();
    controller.reset();  // no stopScanner() first
    QVERIFY2(timer.elapsed() < kPromptMs + 500, qPrintable(QString::number(timer.elapsed())));
    QCOMPARE(meta(path, QStringLiteral("reprocess_pending")), QStringLiteral("1"));
}

void TestShutdown::repeatedLaunchAndQuit()
{
    // The same library opened and closed again and again: closed before the
    // start-up reprocess begins, or at a moment inside the resolver.
    const QString path = freshCopy();
    struct Cycle { bool waitForResolver; int thenMs; };
    const QList<Cycle> cycles = {{false, 0}, {true, 0}, {false, 0}, {true, 40}, {true, 120}, {true, 0}, {true, 250}};
    for (const Cycle& cycle : cycles) {
        // Off before each launch, so its flip proves this launch's resolver started.
        QVERIFY(withDatabase(path, [](QSqlDatabase& database) {
            return exec(database, QStringLiteral("UPDATE catalogue_meta SET value='0' WHERE key='reprocess_pending'"));
        }));
        QElapsedTimer timer;
        {
            LibraryController controller(path);
            QVERIFY(controller.isAvailable());
            if (cycle.waitForResolver) {
                QVERIFY(waitUntilResolving(path));
                QTest::qWait(cycle.thenMs);
            }
            timer.start();
            shutdown::stopLibraryOrExit(controller, 99);  // as the program does
            QVERIFY(!controller.scannerRunning());
        }
        const QString label = QStringLiteral("resolving=%1 +%2 ms").arg(cycle.waitForResolver).arg(cycle.thenMs);
        QVERIFY2(timer.elapsed() < kPromptMs + 500,
                 qPrintable(QStringLiteral("%1: closing took %2 ms").arg(label).arg(timer.elapsed())));
        QCOMPARE(scalar(path, QStringLiteral("PRAGMA quick_check")).toString(), QStringLiteral("ok"));
        QCOMPARE(meta(path, QStringLiteral("resolver_version")), QStringLiteral("4"));  // never finished
        QCOMPARE(meta(path, QStringLiteral("reprocess_pending")),
                 cycle.waitForResolver ? QStringLiteral("1") : QStringLiteral("0"));
        // Only whole 500-song batches can have been kept, never a part of one.
        const int written = scalar(path, QStringLiteral("SELECT count(*) FROM songs WHERE resolver_version=%1")
                                             .arg(MetadataResolver::Version)).toInt();
        QVERIFY2(written % 500 == 0 && written < m_songs, qPrintable(QStringLiteral("%1: %2").arg(label).arg(written)));
    }
}

void TestShutdown::closingEndsTheProgramNormallyWhileTheResolverIsBusy()
{
    // The whole program's closing, in its own process: the scanner is inside
    // the resolver, and the process ends normally and promptly.
    const QString path = freshCopy();
    QProcess helper;
    helper.setProcessChannelMode(QProcess::MergedChannels);
    QElapsedTimer timer;
    timer.start();
    helper.start(QStringLiteral(FKS_SHUTDOWN_HELPER), {QStringLiteral("busy"), path});
    QVERIFY(helper.waitForFinished(60000));
    const QString output = QString::fromLocal8Bit(helper.readAll());
    QVERIFY2(helper.exitStatus() == QProcess::NormalExit, qPrintable(output));
    QVERIFY2(helper.exitCode() == 0, qPrintable(output));
    QVERIFY2(output.contains(QStringLiteral("STOPPED")), qPrintable(output));
    QVERIFY2(!output.contains(QStringLiteral("did not stop")), qPrintable(output));
    QCOMPARE(scalar(path, QStringLiteral("PRAGMA integrity_check")).toString(), QStringLiteral("ok"));
    QCOMPARE(meta(path, QStringLiteral("reprocess_pending")), QStringLiteral("1"));
}

void TestShutdown::aStuckScannerEndsTheProgramWithoutDestroyingAnything()
{
    // The exceptional case: a scanner stuck where it cannot notice a stop
    // (e.g. a read from a music drive that stopped answering). The program
    // must not destroy the thread or anything it uses: it ends at once, with
    // everything the user owns already saved.
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path = temporary.filePath(QStringLiteral("app/library.sqlite"));
    const QString userState = temporary.filePath(QStringLiteral("app/user-state.sqlite"));
    QProcess helper;
    helper.setProcessChannelMode(QProcess::MergedChannels);
    QElapsedTimer timer;
    timer.start();
    helper.start(QStringLiteral(FKS_SHUTDOWN_HELPER), {QStringLiteral("stuck"), path, userState});
    QVERIFY(helper.waitForFinished(30000));
    const qint64 elapsed = timer.elapsed();
    const QString output = QString::fromLocal8Bit(helper.readAll());
    QVERIFY2(helper.exitStatus() == QProcess::NormalExit, qPrintable(output));  // no crash, no abort
    QVERIFY2(helper.exitCode() == 42, qPrintable(output));  // the program's own exit code
    QVERIFY2(output.contains(QStringLiteral("STUCK")), qPrintable(output));
    QVERIFY2(output.contains(QStringLiteral("did not stop within 400 ms")), qPrintable(output));
    QVERIFY2(!output.contains(QStringLiteral("RETURNED")), qPrintable(output));
    QVERIFY2(elapsed < 15000, qPrintable(QString::number(elapsed)));
    // What the user saved before closing is there.
    QCOMPARE(scalar(userState, QStringLiteral("SELECT value FROM preferences WHERE key='test.durable'")).toString(),
             QStringLiteral("saved"));
    QCOMPARE(scalar(userState, QStringLiteral("PRAGMA integrity_check")).toString(), QStringLiteral("ok"));
    QCOMPARE(scalar(path, QStringLiteral("PRAGMA integrity_check")).toString(), QStringLiteral("ok"));
}

void TestShutdown::reparsingStoredNamesStopsPromptlyAndChangesNothing_data()
{
    QTest::addColumn<int>("after");
    QTest::newRow("first name") << 1;
    QTest::newRow("reading names") << 9000;
    QTest::newRow("storing names") << 12 * (kCopies + 1) + 9000;
}

void TestShutdown::reparsingStoredNamesStopsPromptlyAndChangesNothing()
{
    // After a parser upgrade every stored name is parsed again: about 12 s on
    // the real library, so it must stop when asked, all or nothing.
    QFETCH(int, after);
    const QString path = freshCopy();
    QVERIFY(withDatabase(path, [](QSqlDatabase& database) {
        return exec(database, QStringLiteral("UPDATE catalogue_meta SET value='6' WHERE key='parser_version'"));
    }));
    const QString before = scalar(path, QStringLiteral("SELECT total(length(parsed_json))||':'||"
                                                       "group_concat(substr(parsed_json,1,40),'') FROM sources")).toString();
    Catalogue catalogue(path);
    QVERIFY(catalogue.open());
    int asked = 0;
    QElapsedTimer sinceStop;
    bool cancelled = false;
    qint64 count = -1;
    QString error;
    QVERIFY(CatalogueTools::reparseStoredNames(catalogue, &count, &error, [&] {
        if (sinceStop.isValid() || ++asked >= after) {
            if (!sinceStop.isValid())
                sinceStop.start();
            return true;
        }
        return false;
    }, &cancelled));
    QVERIFY(cancelled);
    QVERIFY(error.isEmpty());
    QVERIFY2(sinceStop.elapsed() < kPromptMs, qPrintable(QString::number(sinceStop.elapsed())));
    QVERIFY(withDatabase(path, [](QSqlDatabase& other) {  // no transaction left open
        return exec(other, QStringLiteral("PRAGMA busy_timeout=0"))
            && exec(other, QStringLiteral("BEGIN IMMEDIATE")) && exec(other, QStringLiteral("ROLLBACK"));
    }));
    catalogue.close();
    QCOMPARE(meta(path, QStringLiteral("parser_version")), QStringLiteral("6"));
    QCOMPARE(scalar(path, QStringLiteral("SELECT total(length(parsed_json))||':'||"
                                         "group_concat(substr(parsed_json,1,40),'') FROM sources")).toString(),
             before);
}

QTEST_GUILESS_MAIN(TestShutdown)
#include "tst_shutdown.moc"
