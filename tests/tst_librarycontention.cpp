// The library scan and other writers sharing the catalogue: the scan must
// never stop with "database is locked" because another connection (the
// user's change, a play being recorded) holds SQLite's write lock for a
// moment, however slow the music drive. Everything is synthetic.

#include "LibraryController.h"
#include "library/WritePriority.h"

#include "TestMedia.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QUuid>

#include <atomic>
#include <optional>
#include <thread>

namespace {

// Every file under a folder with its size and modification time.
QMap<QString, QPair<qint64, qint64>> snapshot(const QString& root)
{
    QMap<QString, QPair<qint64, qint64>> files;
    QDirIterator it(root, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QFileInfo info(it.next());
        files.insert(QDir(root).relativeFilePath(info.filePath()),
                     {info.isDir() ? -1 : info.size(), info.lastModified().toMSecsSinceEpoch()});
    }
    return files;
}

QString songName(int i)
{
    return QStringLiteral("KT400-%1 - Synth Band - Song %1.mp3").arg(i, 3, 10, QLatin1Char('0'));
}

// A music folder of `count` small songs (the MP3s are not real audio).
bool writeSongs(const QString& root, int count)
{
    if (!QDir().mkpath(root))
        return false;
    for (int i = 0; i < count; ++i) {
        const QString mp3 = root + QLatin1Char('/') + songName(i);
        if (!testmedia::writeFile(mp3, QByteArray("not really audio ").repeated(64) + QByteArray::number(i))
            || !testmedia::writeCdg(mp3.chopped(4) + QStringLiteral(".cdg"), testmedia::markerCdg(1000, 500)))
            return false;
    }
    return true;
}

// A connection of its own to the catalogue, as another part of the program
// (or another thread) would have. Used only on the thread that made it.
class Connection {
public:
    explicit Connection(const QString& path)
        : m_name(QUuid::createUuid().toString(QUuid::WithoutBraces))
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_name);
        database.setDatabaseName(path);
        database.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=30000"));
        m_open = database.open();
    }
    ~Connection()
    {
        QSqlDatabase::database(m_name, false).close();
        QSqlDatabase::removeDatabase(m_name);
    }
    bool isOpen() const { return m_open; }
    bool exec(const QString& sql, QString* error = nullptr)
    {
        QSqlQuery query(QSqlDatabase::database(m_name, false));
        if (query.exec(sql))
            return true;
        if (error)
            *error = query.lastError().text();
        return false;
    }
    // Takes the write lock and writes. The scan is told a user's change is
    // waiting (so it steps aside first) only while the lock is being taken:
    // it then carries on while this connection still holds the lock, as it
    // would for a writer it was never told about.
    bool takeWriteLock(QString* error)
    {
        const writepriority::UserWrite user;
        return exec(QStringLiteral("BEGIN IMMEDIATE"), error)
            && exec(QStringLiteral("INSERT OR REPLACE INTO catalogue_meta(key,value) "
                                   "VALUES('contention_test','%1')")
                        .arg(QDateTime::currentMSecsSinceEpoch()),
                    error);
    }

private:
    QString m_name;
    bool m_open = false;
};

// Another writer on a thread of its own: takes the write lock again and
// again, holds it a little and lets it go (like plays being recorded, or
// another part of the program saving its work).
class BusyWriter {
public:
    BusyWriter(QString path, int holdMs, int gapMs)
        : m_path(std::move(path)), m_holdMs(holdMs), m_gapMs(gapMs)
    {
    }
    ~BusyWriter() { stop(); }
    void start()
    {
        m_thread = std::thread([this] {
            Connection connection(m_path);
            if (!connection.isOpen()) {
                m_failures.fetch_add(1);
                return;
            }
            while (!m_stop.load()) {
                QString error;
                if (connection.exec(QStringLiteral("BEGIN IMMEDIATE"), &error)
                    && connection.exec(QStringLiteral(
                        "INSERT OR REPLACE INTO catalogue_meta(key,value) VALUES('busy_writer','%1')")
                        .arg(m_writes.load()), &error)) {
                    QThread::msleep(m_holdMs);
                    if (connection.exec(QStringLiteral("COMMIT"), &error))
                        m_writes.fetch_add(1);
                    else
                        m_failures.fetch_add(1);
                } else {
                    connection.exec(QStringLiteral("ROLLBACK"));
                    m_failures.fetch_add(1);
                }
                QThread::msleep(m_gapMs);
            }
        });
    }
    void stop()
    {
        m_stop.store(true);
        if (m_thread.joinable())
            m_thread.join();
    }
    int writes() const { return m_writes.load(); }
    int failures() const { return m_failures.load(); }

private:
    QString m_path;
    int m_holdMs;
    int m_gapMs;
    std::thread m_thread;
    std::atomic_bool m_stop = false;
    std::atomic_int m_writes = 0;
    std::atomic_int m_failures = 0;
};

// Collects the scan's "Library scan failed" warnings.
QStringList g_scanFailures;
QtMessageHandler g_previousHandler = nullptr;
void collectScanFailures(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    if (message.contains(QLatin1String("Library scan failed"))
        || message.contains(QLatin1String("Library scan stopped")))
        g_scanFailures.append(message);
    if (g_previousHandler)
        g_previousHandler(type, context, message);
}

} // namespace

class TestLibraryContention : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void aScanWaitsForAWriterHoldingTheLibrary();
    void aScanOutlastsAHoldLongerThanSqlitesOwnWait();
    void aSlowScanSurvivesConstantContentionAndEdits();
    void busyIsToldApartFromRealFailures();
    void backgroundWorkWaitsForTheLockOrStops();
};

void TestLibraryContention::initTestCase()
{
    g_previousHandler = qInstallMessageHandler(collectScanFailures);
}

void TestLibraryContention::init()
{
    g_scanFailures.clear();
}

void TestLibraryContention::aScanWaitsForAWriterHoldingTheLibrary()
{
    // The scan of a slow drive steps aside for a user's change, then carries
    // on while the change still holds the write lock. It must wait for the
    // lock, not fail: on v0.1.1 the next row failed at once with "Could not
    // upsert file: database is locked" and the whole scan was lost.
    QTemporaryDir temporary;
    const QString root = temporary.filePath(QStringLiteral("music"));
    QVERIFY(writeSongs(root, 150));
    const QMap<QString, QPair<qint64, qint64>> before = snapshot(root);
    const QString catalogue = temporary.filePath(QStringLiteral("app/library.sqlite"));
    LibraryController controller(catalogue);
    QSignalSpy finished(&controller, &LibraryController::scanFinished);
    QVERIFY(controller.chooseRoot(root));
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 30000);

    QSignalSpy progress(&controller, &LibraryController::progressChanged);
    controller.setScanReadDelayForTesting(10);  // 300 files, about three seconds
    controller.requestRefreshScan();
    QTRY_VERIFY_WITH_TIMEOUT(std::any_of(progress.cbegin(), progress.cend(), [](const QList<QVariant>& p) {
        return p.at(0).toString() == QLatin1String("walk") && p.at(1).toLongLong() >= 20;
    }), 10000);
    Connection writer(catalogue);
    QVERIFY(writer.isOpen());
    for (int round = 0; round < 3 && controller.isScanning(); ++round) {
        QString error;
        QVERIFY2(writer.takeWriteLock(&error), qPrintable(error));
        QThread::msleep(400);  // the scan meets the lock at its next file
        QVERIFY2(writer.exec(QStringLiteral("COMMIT"), &error), qPrintable(error));
        QTest::qWait(150);
    }
    QTRY_VERIFY_WITH_TIMEOUT(!controller.isScanning(), 60000);
    QVERIFY2(g_scanFailures.isEmpty(), qPrintable(g_scanFailures.join(QLatin1Char('\n'))));
    QCOMPARE(finished.count(), 2);
    QCOMPARE(finished.last().first().toMap().value(QStringLiteral("status")).toString(),
             QStringLiteral("completed"));
    QVERIFY(controller.findSongByMp3Path(root, songName(149)) > 0);
    QCOMPARE(snapshot(root), before);
}

void TestLibraryContention::aScanOutlastsAHoldLongerThanSqlitesOwnWait()
{
    // SQLite itself waits five seconds for the lock. Background work held up
    // for longer tries again, rather than giving up the scan.
    QTemporaryDir temporary;
    const QString root = temporary.filePath(QStringLiteral("music"));
    QVERIFY(writeSongs(root, 100));
    const QString catalogue = temporary.filePath(QStringLiteral("app/library.sqlite"));
    LibraryController controller(catalogue);
    QSignalSpy finished(&controller, &LibraryController::scanFinished);
    QVERIFY(controller.chooseRoot(root));
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 30000);

    QSignalSpy progress(&controller, &LibraryController::progressChanged);
    controller.setScanReadDelayForTesting(10);
    controller.requestRefreshScan();
    QTRY_VERIFY_WITH_TIMEOUT(std::any_of(progress.cbegin(), progress.cend(), [](const QList<QVariant>& p) {
        return p.at(0).toString() == QLatin1String("walk") && p.at(1).toLongLong() >= 20;
    }), 10000);
    Connection writer(catalogue);
    QVERIFY(writer.isOpen());
    QString error;
    QVERIFY2(writer.takeWriteLock(&error), qPrintable(error));
    QThread::msleep(6500);
    QVERIFY2(writer.exec(QStringLiteral("COMMIT"), &error), qPrintable(error));
    QTRY_VERIFY_WITH_TIMEOUT(!controller.isScanning(), 60000);
    QVERIFY2(g_scanFailures.isEmpty(), qPrintable(g_scanFailures.join(QLatin1Char('\n'))));
    QCOMPARE(finished.count(), 2);
    QCOMPARE(finished.last().first().toMap().value(QStringLiteral("status")).toString(),
             QStringLiteral("completed"));
}

void TestLibraryContention::aSlowScanSurvivesConstantContentionAndEdits()
{
    // A Windows-like slow drive, another writer taking the lock all the time
    // and the user changing keys and names meanwhile: the scan completes,
    // every change is kept, and no change waits long.
    QTemporaryDir temporary;
    const QString root = temporary.filePath(QStringLiteral("music"));
    constexpr int kSongs = 120;
    QVERIFY(writeSongs(root, kSongs));
    const QMap<QString, QPair<qint64, qint64>> before = snapshot(root);
    const QString appDir = temporary.filePath(QStringLiteral("app"));
    const QString catalogue = appDir + QStringLiteral("/library.sqlite");
    LibraryController controller(catalogue, {}, appDir + QStringLiteral("/metadata-overrides.sqlite"));
    QSignalSpy finished(&controller, &LibraryController::scanFinished);
    QVERIFY(controller.chooseRoot(root));
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 30000);
    QList<qint64> songs;
    for (int i = 0; i < 3; ++i) {
        songs.append(controller.findSongByMp3Path(root, songName(i)));
        QVERIFY(songs.last() > 0);
    }

    BusyWriter other(catalogue, 15, 10);
    other.start();
    controller.setScanReadDelayForTesting(15);
    controller.requestRefreshScan();
    std::optional<int> expected[3];
    int edits = 0;
    qint64 slowest = 0;
    QElapsedTimer scanTime;
    scanTime.start();
    while (controller.isScanning() && scanTime.elapsed() < 60000) {
        const int which = edits % 3;
        const std::optional<int> key = edits % 4 == 3 ? std::nullopt : std::optional<int>((edits * 5) % 24);
        QElapsedTimer timer;
        timer.start();
        QString error;
        QVERIFY2(controller.setManualOriginalKey(songs.at(which), key, &error), qPrintable(error));
        slowest = std::max(slowest, timer.elapsed());
        expected[which] = key;
        ++edits;
        QTest::qWait(60);
    }
    other.stop();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.isScanning(), 60000);
    QVERIFY2(g_scanFailures.isEmpty(), qPrintable(g_scanFailures.join(QLatin1Char('\n'))));
    QVERIFY2(edits >= 10, qPrintable(QStringLiteral("only %1 edits overlapped the scan").arg(edits)));
    QVERIFY2(slowest < 4000, qPrintable(QStringLiteral("an edit waited %1 ms").arg(slowest)));
    QVERIFY2(other.writes() > 0, qPrintable(QString::number(other.writes())));  // it did compete
    QCOMPARE(other.failures(), 0);
    QCOMPARE(finished.count(), 2);
    QCOMPARE(finished.last().first().toMap().value(QStringLiteral("status")).toString(),
             QStringLiteral("completed"));
    // Corrections the catalogue could not show at once follow within seconds.
    const auto manualKey = [&controller](qint64 songId) {
        const std::optional<SongKeyInfo> details = controller.songKeyDetails(songId);
        return details ? details->manualKeyIndex : -1;  // nothing known: no key by hand
    };
    for (int i = 0; i < 3; ++i)
        QTRY_COMPARE_WITH_TIMEOUT(manualKey(songs.at(i)), expected[i] ? *expected[i] : -1, 10000);
    QCOMPARE(snapshot(root), before);
}

void TestLibraryContention::busyIsToldApartFromRealFailures()
{
    using writepriority::isBusy;
    const auto sqlError = [](const char* text, const char* code) {
        return QSqlError(QStringLiteral("Unable to fetch row"), QString::fromLatin1(text),
                         QSqlError::StatementError, QString::fromLatin1(code));
    };
    QVERIFY(isBusy(sqlError("database is locked", "5")));
    QVERIFY(isBusy(sqlError("database table is locked", "6")));
    QVERIFY(isBusy(sqlError("database is locked", "517")));  // SQLITE_BUSY_SNAPSHOT
    QVERIFY(!isBusy(sqlError("database disk image is malformed", "11")));
    QVERIFY(!isBusy(sqlError("disk I/O error", "10")));
    QVERIFY(!isBusy(sqlError("database or disk is full", "13")));
    QVERIFY(!isBusy(sqlError("attempt to write a readonly database", "8")));
    QVERIFY(!isBusy(QSqlError()));
    QVERIFY(isBusy(QStringLiteral("Could not begin walk batch: database is locked Unable to begin transaction")));
    QVERIFY(!isBusy(QStringLiteral("Could not store ID3 tags: database disk image is malformed")));
    QVERIFY(!isBusy(QStringLiteral("Library root is not a directory: /Volumes/Busy Songs")));

    // The real thing: a lock another connection holds is busy; a damaged
    // file is not, and is never retried as if it were.
    QTemporaryDir temporary;
    const QString path = temporary.filePath(QStringLiteral("test.sqlite"));
    {
        Connection holder(path);
        QVERIFY(holder.isOpen());
        QVERIFY(holder.exec(QStringLiteral("PRAGMA journal_mode=WAL")));
        QVERIFY(holder.exec(QStringLiteral("CREATE TABLE catalogue_meta(key TEXT PRIMARY KEY,value TEXT)")));
        QString error;
        QVERIFY2(holder.takeWriteLock(&error), qPrintable(error));
        const QString name = QStringLiteral("busy-probe");
        {
            QSqlDatabase other = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
            other.setDatabaseName(path);
            other.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=0"));
            QVERIFY(other.open());
            QSqlQuery begin(other);
            QVERIFY(!begin.exec(QStringLiteral("BEGIN IMMEDIATE")));
            QVERIFY2(isBusy(begin.lastError()), qPrintable(begin.lastError().nativeErrorCode()));
            QString beginError;
            QVERIFY(!writepriority::beginWrite(other, &beginError));
            QVERIFY2(isBusy(beginError), qPrintable(beginError));
            other.close();
        }
        QSqlDatabase::removeDatabase(name);
        QVERIFY(holder.exec(QStringLiteral("COMMIT")));
    }
    const QString damaged = temporary.filePath(QStringLiteral("damaged.sqlite"));
    QVERIFY(testmedia::writeFile(damaged, QByteArray("SQLite format 3").append('\0')
                                              .append(QByteArray(4080, '\x5a'))));
    const QString name = QStringLiteral("damaged-probe");
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        database.setDatabaseName(damaged);
        QVERIFY(database.open());
        QString error;
        int attempts = 0;
        QElapsedTimer timer;
        timer.start();
        QVERIFY(!writepriority::retryWhileBusy([&](QString* attemptError) {
            ++attempts;
            QSqlQuery query(database);
            if (query.exec(QStringLiteral("SELECT count(*) FROM sqlite_master")))
                return true;
            *attemptError = query.lastError().text();
            return false;
        }, {}, &error));
        QCOMPARE(attempts, 1);
        QVERIFY2(!isBusy(error), qPrintable(error));
        QVERIFY(timer.elapsed() < 1000);
        database.close();
    }
    QSqlDatabase::removeDatabase(name);
}

void TestLibraryContention::backgroundWorkWaitsForTheLockOrStops()
{
    QTemporaryDir temporary;
    const QString path = temporary.filePath(QStringLiteral("test.sqlite"));
    Connection holder(path);
    QVERIFY(holder.isOpen());
    QVERIFY(holder.exec(QStringLiteral("PRAGMA journal_mode=WAL")));
    QVERIFY(holder.exec(QStringLiteral("CREATE TABLE catalogue_meta(key TEXT PRIMARY KEY,value TEXT)")));
    QString error;
    QVERIFY2(holder.takeWriteLock(&error), qPrintable(error));

    const QString name = QStringLiteral("background-probe");
    {
        // As the program's connections: SQLite itself waits five seconds.
        QSqlDatabase background = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        background.setDatabaseName(path);
        background.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000"));
        QVERIFY(background.open());
        const auto busyTimeout = [&background] {
            QSqlQuery query(background);
            return query.exec(QStringLiteral("PRAGMA busy_timeout")) && query.next()
                ? query.value(0).toInt() : -1;
        };
        QCOMPARE(busyTimeout(), 5000);

        // Asked to stop while waiting: it stops promptly (not after SQLite's
        // five seconds), no lock is taken, yet a transaction is open, so the
        // usual commit on the way out still succeeds.
        std::atomic_bool stopNow = false;
        std::thread stopper([&stopNow] {
            QThread::msleep(400);
            stopNow.store(true);
        });
        QElapsedTimer timer;
        timer.start();
        bool stopped = false;
        QVERIFY(writepriority::beginBackgroundWrite(background, [&stopNow] { return stopNow.load(); },
                                                    &error, &stopped));
        const qint64 waited = timer.elapsed();
        stopper.join();
        QVERIFY(stopped);
        QVERIFY2(waited >= 350 && waited < 1500, qPrintable(QString::number(waited)));
        QVERIFY(background.commit());
        QCOMPARE(busyTimeout(), 5000);  // as it was

        // The same for work retried as a whole.
        stopNow.store(false);
        std::thread stopper2([&stopNow] {
            QThread::msleep(400);
            stopNow.store(true);
        });
        timer.restart();
        int attempts = 0;
        QVERIFY(!writepriority::retryWhileBusy([&](QString* attemptError) {
            ++attempts;
            const writepriority::BriefWait brief(background);
            return writepriority::beginWrite(background, attemptError);
        }, [&stopNow] { return stopNow.load(); }, &error));
        const qint64 retried = timer.elapsed();
        stopper2.join();
        QVERIFY2(writepriority::isBusy(error), qPrintable(error));
        QVERIFY(attempts >= 2);
        QVERIFY2(retried >= 350 && retried < 1500, qPrintable(QString::number(retried)));
        QCOMPARE(busyTimeout(), 5000);

        // The lock let go while waiting (longer than SQLite's own wait):
        // the write goes ahead.
        QVERIFY(holder.exec(QStringLiteral("COMMIT")));
        QVERIFY(holder.takeWriteLock(&error));
        std::atomic_bool released = false;
        std::thread later([&] {
            QThread::msleep(700);
            released.store(true);
        });
        // The holder commits from this thread once `released` is set; the
        // background begin is tried on another thread meanwhile.
        std::atomic_bool begun = false;
        std::atomic_bool failedToBegin = false;
        std::thread worker([&] {
            const QString workerName = QStringLiteral("background-worker");
            {
                QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), workerName);
                database.setDatabaseName(path);
                database.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000"));
                QString workerError;
                bool workerStopped = false;
                if (database.open()
                    && writepriority::beginBackgroundWrite(database, {}, &workerError, &workerStopped)
                    && !workerStopped) {
                    QSqlQuery write(database);
                    begun.store(write.exec(QStringLiteral(
                                    "INSERT OR REPLACE INTO catalogue_meta(key,value) VALUES('bg','1')"))
                                && database.commit());
                } else {
                    failedToBegin.store(true);
                }
                database.close();
            }
            QSqlDatabase::removeDatabase(workerName);
        });
        QTRY_VERIFY_WITH_TIMEOUT(released.load(), 5000);
        QVERIFY(holder.exec(QStringLiteral("COMMIT")));
        later.join();
        worker.join();
        QVERIFY(!failedToBegin.load());
        QVERIFY(begun.load());
        background.close();
    }
    QSqlDatabase::removeDatabase(name);
}

QTEST_GUILESS_MAIN(TestLibraryContention)
#include "tst_librarycontention.moc"
