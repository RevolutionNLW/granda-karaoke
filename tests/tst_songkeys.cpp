// Song keys worked out from the audio: key names and transposition, the
// detector on synthetic music in known keys (and on material with no clear
// key), the GStreamer decoding path, and background analysis in the library:
// its cache, pause, shutdown and the promise never to write to the music.
// Everything is synthetic; no real karaoke media is used.

#include "BusTestPlayer.h"
#include "KeyTestAudio.h"
#include "LibraryController.h"
#include "LibraryResultsModel.h"
#include "LibraryView.h"
#include "MainWindow.h"
#include "SongKeyAnalyser.h"
#include "library/SongKeys.h"
#include "music/KeyDetector.h"
#include "music/MusicalKey.h"

#include "TestMedia.h"

#include <QApplication>
#include <QDateTime>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QTreeView>
#include <QPushButton>
#include <QSet>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

#include <atomic>
#include <mutex>

using music::KeyAnalysis;
using music::KeyDetector;
using music::MusicalKey;

namespace {

constexpr int kRate = music::kKeyAnalysisSampleRate;
// Generous for slow or sanitizer builds; the analysis stops within ~50 ms.
constexpr qint64 kPromptMs = 400;

struct Fixture {
    const char* name;
    int tonic;   // MIDI note of the tonic (60 = C4)
    bool minor;
};
// The keys the brief asks for, major and minor.
const Fixture kFixtures[] = {
    {"C", 60, false}, {"G", 55, false}, {"F", 53, false}, {"Bb", 58, false},
    {"Am", 57, true}, {"Em", 52, true}, {"F#m", 54, true},
};

std::vector<float> bandIn(int tonic, bool minor, double seconds)
{
    keyaudio::Synth synth(kRate);
    keyaudio::band(synth, tonic, minor, seconds);
    return synth.out;
}

QString keyName(const KeyAnalysis& analysis)
{
    return analysis.key ? QString::fromStdString(analysis.key->name()) : QStringLiteral("-");
}

QString describe(const KeyAnalysis& a)
{
    return QStringLiteral("%1 %2 r=%3 second=%4 %5 margin=%6 agreement=%7 tuning=%8c confidence=%9")
        .arg(QString::fromLatin1(music::statusName(a.status)), keyName(a))
        .arg(a.correlation, 0, 'f', 3)
        .arg(a.runnerUp ? QString::fromStdString(a.runnerUp->name()) : QStringLiteral("-"))
        .arg(a.runnerUpCorrelation, 0, 'f', 3)
        .arg(a.margin, 0, 'f', 2)
        .arg(a.agreement, 0, 'f', 2)
        .arg(a.tuningCents, 0, 'f', 1)
        .arg(a.confidence, 0, 'f', 2);
}

// Counts the songs it is asked to analyse, then asks the real engine.
class CountingEngine final : public SongKeyEngine {
public:
    std::atomic_int calls = 0;
    Outcome analyse(const QString& path, const std::function<bool()>& stop,
                    KeyAnalysis* result, QString* detail) override
    {
        ++calls;
        return m_real.analyse(path, stop, result, detail);
    }

private:
    GstSongKeyEngine m_real;
};

// Works until told to stop, like a very long song on a slow drive.
class EndlessEngine final : public SongKeyEngine {
public:
    std::atomic_int calls = 0;
    std::atomic_bool working = false;
    Outcome analyse(const QString&, const std::function<bool()>& stop,
                    KeyAnalysis*, QString*) override
    {
        ++calls;
        working = true;
        while (!stop())
            QThread::msleep(5);
        working = false;
        return Outcome::Interrupted;
    }
};

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

qint64 rowCount(const QString& databasePath, const QString& sql)
{
    const QString name = QUuid::createUuid().toString(QUuid::WithoutBraces);
    qint64 count = -1;
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        database.setDatabaseName(databasePath);
        if (database.open()) {
            QSqlQuery query(database);
            if (query.exec(sql) && query.next())
                count = query.value(0).toLongLong();
        }
        database.close();
    }
    QSqlDatabase::removeDatabase(name);
    return count;
}

bool analysedAll(const LibraryController& controller, qint64 total)
{
    const auto summary = controller.songKeySummary();
    return summary && summary->total == total && summary->analysed == total
        && !controller.isAnalysingSongKeys();
}

LibraryController::SongKeyTimings quickTimings()
{
    LibraryController::SongKeyTimings timings;
    timings.startMs = 0;
    timings.restMs = 0;
    timings.afterPlaybackMs = 300;
    timings.retryMs = 300;
    return timings;
}

} // namespace

class TestSongKeys : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void keyNamesAreFamiliarAndParse();
    void transpositionKeepsTheModeAndWraps();
    void detectorFindsKnownKeys_data();
    void detectorFindsKnownKeys();
    void detectorStaysQuietWithoutAClearKey_data();
    void detectorStaysQuietWithoutAClearKey();
    void detectorChoosesTheMainKey();
    void detectorIsStable();
    void engineReadsKeysFromMp3s();
    void engineReportsUnreadableAndDamagedFiles();
    void engineStopsPromptly();
    void backgroundAnalysisIsCachedByContentAndNeverWritesToTheMusic();
    void damagedSongsAreRecordedNotRetried();
    void playbackPausesAnalysisAndItResumesLater();
    void closingDuringAnalysisStopsPromptly();
    void analysisIsOffUntilTurnedOn();
    void libraryAndPlayerBarShowTheKeys();

private:
    QTemporaryDir m_dir;
    QString m_fixtures;  // one MP3 per kFixtures entry, never inside a music folder
};

void TestSongKeys::initTestCase()
{
    qputenv("GST_PLUGIN_FEATURE_RANK",
            "osxaudiosink:NONE,wasapi2sink:NONE,wasapisink:NONE,directsoundsink:NONE,"
            "pulsesink:NONE,alsasink:NONE,pipewiresink:NONE,jackaudiosink:NONE,openalsink:NONE");
    QString error;
    QVERIFY2(KaraokePlayer::initializeGStreamer(&error), qPrintable(error));
    QVERIFY(m_dir.isValid());
    m_fixtures = m_dir.filePath(QStringLiteral("fixtures"));
    QVERIFY(QDir().mkpath(m_fixtures));
    for (const Fixture& fixture : kFixtures) {
        const QString path = QDir(m_fixtures).filePath(QString::fromLatin1(fixture.name) + QStringLiteral(".mp3"));
        QVERIFY2(keyaudio::writeMp3(path, bandIn(fixture.tonic, fixture.minor, 45), kRate), fixture.name);
    }
}

void TestSongKeys::keyNamesAreFamiliarAndParse()
{
    QSet<QString> names;
    for (int index = 0; index < 24; ++index) {
        const auto key = MusicalKey::fromIndex(index);
        QVERIFY(key);
        QCOMPARE(key->index(), index);
        const QString name = QString::fromStdString(key->name());
        for (const char* odd : {"B#", "E#", "Cb", "Fb"})
            QVERIFY2(!name.startsWith(QLatin1String(odd)), qPrintable(name));
        QCOMPARE(name.endsWith(QLatin1Char('m')), index >= 12);
        names.insert(name);
        QVERIFY(MusicalKey::parse(key->name()) == key);
    }
    QCOMPARE(names.size(), 24);
    QVERIFY(!MusicalKey::fromIndex(-1));
    QVERIFY(!MusicalKey::fromIndex(24));
    QCOMPARE(QString::fromStdString(MusicalKey{6, false}.name()), QStringLiteral("F#"));
    QCOMPARE(QString::fromStdString(MusicalKey{1, false}.name()), QStringLiteral("Db"));
    QCOMPARE(QString::fromStdString(MusicalKey{1, true}.name()), QStringLiteral("C#m"));
    QCOMPARE(QString::fromStdString(MusicalKey{3, true}.name()), QStringLiteral("Ebm"));
    QCOMPARE(QString::fromStdString(MusicalKey{8, true}.name()), QStringLiteral("G#m"));
    QCOMPARE(QString::fromStdString(MusicalKey{10, false}.name()), QStringLiteral("Bb"));

    // Enharmonic twins and other ways of writing a key.
    const auto parsed = [](const char* text) {
        const auto key = MusicalKey::parse(text);
        return key ? QString::fromStdString(key->name()) : QStringLiteral("-");
    };
    QCOMPARE(parsed("A#"), QStringLiteral("Bb"));
    QCOMPARE(parsed("Gbm"), QStringLiteral("F#m"));
    QCOMPARE(parsed("C#"), QStringLiteral("Db"));
    QCOMPARE(parsed("D#m"), QStringLiteral("Ebm"));
    QCOMPARE(parsed("Abm"), QStringLiteral("G#m"));
    QCOMPARE(parsed("cmin"), QStringLiteral("Cm"));
    QCOMPARE(parsed(" E minor "), QStringLiteral("Em"));
    QCOMPARE(parsed("B#"), QStringLiteral("C"));
    QCOMPARE(parsed("Fb"), QStringLiteral("E"));
    QCOMPARE(parsed("Bb major"), QStringLiteral("Bb"));
    for (const char* bad : {"", "H", "Cx", "C#x", "m", "#"})
        QVERIFY2(!MusicalKey::parse(bad), bad);
}

void TestSongKeys::transpositionKeepsTheModeAndWraps()
{
    const auto shifted = [](const char* key, int semitones) {
        return QString::fromStdString(MusicalKey::parse(key)->transposed(semitones).name());
    };
    QCOMPARE(shifted("C", 2), QStringLiteral("D"));
    QCOMPARE(shifted("F#m", 2), QStringLiteral("G#m"));
    QCOMPARE(shifted("Bb", -2), QStringLiteral("Ab"));
    QCOMPARE(shifted("B", 1), QStringLiteral("C"));
    QCOMPARE(shifted("C", -1), QStringLiteral("B"));
    QCOMPARE(shifted("Am", 3), QStringLiteral("Cm"));
    QCOMPARE(shifted("E", 1), QStringLiteral("F"));
    QCOMPARE(shifted("Ebm", 6), QStringLiteral("Am"));
    for (int index = 0; index < 24; ++index) {
        const MusicalKey key = *MusicalKey::fromIndex(index);
        for (int semitones = -24; semitones <= 24; ++semitones) {
            const MusicalKey moved = key.transposed(semitones);
            QCOMPARE(moved.minor, key.minor);
            QCOMPARE(moved.tonic, ((key.tonic + semitones) % 12 + 12) % 12);
            QVERIFY(moved.transposed(-semitones) == key);
            QCOMPARE(transposedKeyName(index, semitones), QString::fromStdString(moved.name()));
        }
    }
    QCOMPARE(songKeyName(-1), QString());
    QCOMPARE(songKeyName(24), QString());
    QCOMPARE(songKeyName(0), QStringLiteral("C"));
    QCOMPARE(transposedKeyName(0, 2), QStringLiteral("D"));
}

void TestSongKeys::detectorFindsKnownKeys_data()
{
    QTest::addColumn<int>("tonic");
    QTest::addColumn<bool>("minor");
    QTest::addColumn<bool>("withBand");
    QTest::addColumn<double>("cents");
    QTest::addColumn<QString>("expected");
    for (const Fixture& f : kFixtures) {
        QTest::addRow("%s band", f.name) << f.tonic << f.minor << true << 0.0 << QString::fromLatin1(f.name);
        QTest::addRow("%s chords", f.name) << f.tonic << f.minor << false << 0.0 << QString::fromLatin1(f.name);
    }
    // Recordings a little off A = 440 Hz.
    QTest::addRow("C +35 cents") << 60 << false << false << 35.0 << QStringLiteral("C");
    QTest::addRow("Em -30 cents") << 52 << true << false << -30.0 << QStringLiteral("Em");
    // Keys spelt with flats and sharps.
    QTest::addRow("Db chords") << 61 << false << false << 0.0 << QStringLiteral("Db");
    QTest::addRow("Ebm chords") << 51 << true << false << 0.0 << QStringLiteral("Ebm");
    QTest::addRow("G#m chords") << 56 << true << false << 0.0 << QStringLiteral("G#m");
}

void TestSongKeys::detectorFindsKnownKeys()
{
    QFETCH(int, tonic);
    QFETCH(bool, minor);
    QFETCH(bool, withBand);
    QFETCH(double, cents);
    QFETCH(QString, expected);
    keyaudio::Synth synth(kRate);
    if (withBand)
        keyaudio::band(synth, tonic, minor, 90);
    else
        keyaudio::progression(synth, tonic, minor, 90, 2.0, cents);
    const KeyAnalysis result = KeyDetector::analyse(synth.out);
    QVERIFY2(result.status == KeyAnalysis::Status::Confident, qPrintable(describe(result)));
    QCOMPARE(keyName(result), expected);
    QVERIFY2(std::abs(result.tuningCents - cents) < 5.0, qPrintable(describe(result)));
    QVERIFY(result.confidence > 0.0 && result.confidence <= 1.0);
}

void TestSongKeys::detectorStaysQuietWithoutAClearKey_data()
{
    QTest::addColumn<int>("kind");
    QTest::addColumn<int>("status");  // expected KeyAnalysis::Status, or -1 for "not Confident"
    QTest::addRow("silence") << 0 << int(KeyAnalysis::Status::Silent);
    QTest::addRow("white noise") << 1 << -1;
    QTest::addRow("three seconds") << 2 << int(KeyAnalysis::Status::TooShort);
    QTest::addRow("chromatic cluster") << 3 << -1;
    QTest::addRow("power chord, neither major nor minor") << 4 << -1;
    QTest::addRow("C and D alternating") << 5 << -1;
    QTest::addRow("nearly silent hum") << 6 << int(KeyAnalysis::Status::Silent);
    QTest::addRow("nothing at all") << 7 << int(KeyAnalysis::Status::TooShort);
}

void TestSongKeys::detectorStaysQuietWithoutAClearKey()
{
    QFETCH(int, kind);
    QFETCH(int, status);
    keyaudio::Synth synth(kRate);
    switch (kind) {
    case 0: synth.silence(60); break;
    case 1: synth.noise(60, 0.2); break;
    case 2: keyaudio::progression(synth, 60, false, 3); break;
    case 3:
        for (int i = 0; i < 30; ++i)
            synth.chord({60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71}, 2.0, 0.05);
        break;
    case 4:
        for (int i = 0; i < 60; ++i)
            synth.chord({48, 60, 67}, 2.0);
        break;
    case 5:
        for (int i = 0; i < 40; ++i) {
            synth.chord({60, 64, 67}, 1.0);
            synth.chord({62, 66, 69}, 1.0);
        }
        break;
    case 6:
        for (int i = 0; i < 30; ++i)
            synth.chord({60, 64, 67}, 2.0, 0.0005);  // about -60 dBFS
        break;
    case 7: break;
    }
    const KeyAnalysis result = KeyDetector::analyse(synth.out);
    if (status < 0)
        QVERIFY2(result.status != KeyAnalysis::Status::Confident, qPrintable(describe(result)));
    else
        QVERIFY2(int(result.status) == status, qPrintable(describe(result)));
}

void TestSongKeys::detectorChoosesTheMainKey()
{
    // A spoken or noisy intro and a fade do not decide it.
    {
        keyaudio::Synth synth(kRate);
        synth.noise(15, 0.05);
        keyaudio::band(synth, 55, false, 90);
        for (int i = 0; i < 10; ++i)
            synth.chord(keyaudio::triad(55, false), 1.0, 0.12 * (1.0 - i / 10.0));
        synth.silence(5);
        const KeyAnalysis result = KeyDetector::analyse(synth.out);
        QVERIFY2(result.status == KeyAnalysis::Status::Confident, qPrintable(describe(result)));
        QCOMPARE(keyName(result), QStringLiteral("G"));
    }
    // A last-chorus key change up a tone: the key sung longest is the main one.
    {
        keyaudio::Synth synth(kRate);
        keyaudio::progression(synth, 60, false, 150);
        keyaudio::progression(synth, 62, false, 45);
        const KeyAnalysis result = KeyDetector::analyse(synth.out);
        QCOMPARE(keyName(result), QStringLiteral("C"));
        QVERIFY2(result.agreement < 1.0, qPrintable(describe(result)));
    }
    // A song that is half in one key and half in a distant one is not
    // confidently either.
    {
        keyaudio::Synth synth(kRate);
        keyaudio::progression(synth, 60, false, 90);
        keyaudio::progression(synth, 54, false, 90);
        const KeyAnalysis result = KeyDetector::analyse(synth.out);
        QVERIFY2(result.status != KeyAnalysis::Status::Confident, qPrintable(describe(result)));
    }
}

void TestSongKeys::detectorIsStable()
{
    const std::vector<float> samples = bandIn(54, true, 60);
    const KeyAnalysis first = KeyDetector::analyse(samples);
    const KeyAnalysis again = KeyDetector::analyse(samples);
    QCOMPARE(keyName(again), keyName(first));
    QCOMPARE(again.correlation, first.correlation);
    // The result does not depend on how the audio arrives.
    KeyDetector detector;
    for (std::size_t offset = 0; offset < samples.size(); offset += 1103)
        detector.addSamples(samples.data() + offset, std::min<std::size_t>(1103, samples.size() - offset));
    const KeyAnalysis pieces = detector.finish();
    QCOMPARE(keyName(pieces), keyName(first));
    QCOMPARE(pieces.correlation, first.correlation);
    // A stop request is honoured.
    const KeyAnalysis stopped = KeyDetector::analyse(samples, [] { return true; });
    QVERIFY(!stopped.key);
}

void TestSongKeys::engineReadsKeysFromMp3s()
{
    GstSongKeyEngine engine;
    qint64 totalMs = 0;
    for (const Fixture& fixture : kFixtures) {
        const QString path = QDir(m_fixtures).filePath(QString::fromLatin1(fixture.name) + QStringLiteral(".mp3"));
        KeyAnalysis result;
        QString detail;
        QElapsedTimer timer;
        timer.start();
        const auto outcome = engine.analyse(path, [] { return false; }, &result, &detail);
        totalMs += timer.elapsed();
        QVERIFY2(outcome == SongKeyEngine::Outcome::Analysed, qPrintable(detail));
        QVERIFY2(result.status == KeyAnalysis::Status::Confident,
                 qPrintable(QString::fromLatin1(fixture.name) + QLatin1Char(' ') + describe(result)));
        QCOMPARE(keyName(result), QString::fromLatin1(fixture.name));
        // 45 s asked for; the chords are whole 2-second steps.
        QVERIFY2(result.seconds > 44.0 && result.seconds < 47.0, qPrintable(QString::number(result.seconds)));
    }
    qInfo("Decoded and analysed %d 45-second MP3s in %lld ms (%lld ms each)",
          int(std::size(kFixtures)), totalMs, totalMs / qint64(std::size(kFixtures)));
}

void TestSongKeys::engineReportsUnreadableAndDamagedFiles()
{
    GstSongKeyEngine engine;
    const auto run = [&engine](const QString& path, KeyAnalysis* result = nullptr) {
        KeyAnalysis scratch;
        QString detail;
        return engine.analyse(path, [] { return false; }, result ? result : &scratch, &detail);
    };
    const QDir dir(m_dir.filePath(QStringLiteral("damaged")));
    QVERIFY(QDir().mkpath(dir.path()));
    QCOMPARE(run(dir.filePath(QStringLiteral("missing.mp3"))), SongKeyEngine::Outcome::Unreadable);
    QVERIFY(testmedia::writeFile(dir.filePath(QStringLiteral("empty.mp3")), {}));
    QCOMPARE(run(dir.filePath(QStringLiteral("empty.mp3"))), SongKeyEngine::Outcome::Unreadable);
    QByteArray noise(200000, '\0');
    std::mt19937 rng(3);
    for (char& c : noise)
        c = char(rng() & 0xff);
    // Random bytes that happen to look like MP3 frames may decode to noise:
    // either way no key can come of them.
    QVERIFY(testmedia::writeFile(dir.filePath(QStringLiteral("noise.mp3")), noise));
    KeyAnalysis result;
    const auto noiseOutcome = run(dir.filePath(QStringLiteral("noise.mp3")), &result);
    QVERIFY(noiseOutcome == SongKeyEngine::Outcome::NotAudio
            || (noiseOutcome == SongKeyEngine::Outcome::Analysed
                && result.status != KeyAnalysis::Status::Confident));
    QVERIFY(testmedia::writeFile(dir.filePath(QStringLiteral("text.mp3")),
                                 QByteArray("These are not the songs you are looking for.\n").repeated(200)));
    QCOMPARE(run(dir.filePath(QStringLiteral("text.mp3"))), SongKeyEngine::Outcome::NotAudio);
    // A cut-off download: what is there is analysed, without a crash.
    QFile whole(QDir(m_fixtures).filePath(QStringLiteral("C.mp3")));
    QVERIFY(whole.open(QIODevice::ReadOnly));
    const QByteArray bytes = whole.readAll();
    QVERIFY(testmedia::writeFile(dir.filePath(QStringLiteral("cut.mp3")), bytes.left(bytes.size() / 3)));
    QCOMPARE(run(dir.filePath(QStringLiteral("cut.mp3")), &result), SongKeyEngine::Outcome::Analysed);
    QVERIFY(result.seconds < 20.0);
    QVERIFY(result.status != KeyAnalysis::Status::Confident);
    // Names in any script (the file is opened by URI, as the player does).
    const QString accented = dir.filePath(QStringLiteral("Café Ñandú – 歌.mp3"));
    QVERIFY(QFile::copy(whole.fileName(), accented));
    QCOMPARE(run(accented, &result), SongKeyEngine::Outcome::Analysed);
    QCOMPARE(keyName(result), QStringLiteral("C"));
}

void TestSongKeys::engineStopsPromptly()
{
    const QString path = m_dir.filePath(QStringLiteral("long.mp3"));
    QVERIFY(testmedia::writeMp3(path, 240000));
    GstSongKeyEngine engine;
    QElapsedTimer timer;
    timer.start();
    KeyAnalysis result;
    QString detail;
    const auto outcome = engine.analyse(path, [&timer] { return timer.elapsed() > 30; }, &result, &detail);
    const qint64 elapsed = timer.elapsed();
    QCOMPARE(outcome, SongKeyEngine::Outcome::Interrupted);
    QVERIFY2(elapsed < 30 + kPromptMs, qPrintable(QString::number(elapsed)));
    // Stopped before it began: nothing is read beyond the first byte.
    timer.restart();
    QCOMPARE(engine.analyse(path, [] { return true; }, &result, &detail), SongKeyEngine::Outcome::Interrupted);
    QVERIFY(timer.elapsed() < kPromptMs);
}

void TestSongKeys::backgroundAnalysisIsCachedByContentAndNeverWritesToTheMusic()
{
    QTemporaryDir temporary;
    const QString root = temporary.filePath(QStringLiteral("music"));
    QVERIFY(QDir().mkpath(root + QStringLiteral("/Disc 1")));
    const QStringList keys = {QStringLiteral("C"), QStringLiteral("Am"), QStringLiteral("F#m")};
    for (int i = 0; i < keys.size(); ++i) {
        const QString base = QStringLiteral("%1/Disc 1/KT001-0%2 - Synth Band - Song In %3")
                                 .arg(root).arg(i + 1).arg(QString(keys.at(i)).replace(QLatin1Char('#'), QLatin1String("sharp")));
        QVERIFY(QFile::copy(QDir(m_fixtures).filePath(keys.at(i) + QStringLiteral(".mp3")), base + QStringLiteral(".mp3")));
        QVERIFY(testmedia::writeCdg(base + QStringLiteral(".cdg"), testmedia::markerCdg(45000, 1000)));
    }
    const QString appDir = temporary.filePath(QStringLiteral("app"));
    const QString catalogue = appDir + QStringLiteral("/library.sqlite");
    auto engine = std::make_shared<CountingEngine>();

    const auto songIdFor = [&root](LibraryController& controller, const QString& relPath) {
        return controller.findSongByMp3Path(root, relPath);
    };
    QMap<QString, QPair<qint64, qint64>> before;
    {
        LibraryController controller(catalogue);
        controller.setSongKeyEngineFactory([engine] { return engine; });
        controller.setSongKeyTimings(quickTimings());
        QSignalSpy finished(&controller, &LibraryController::scanFinished);
        QVERIFY(controller.chooseRoot(root));
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 20000);
        before = snapshot(root);

        QSignalSpy changed(&controller, &LibraryController::songKeysChanged);
        controller.setSongKeyAnalysisEnabled(true);
        QTRY_VERIFY_WITH_TIMEOUT(analysedAll(controller, 3), 60000);
        QVERIFY(changed.count() >= 1);
        QCOMPARE(engine->calls.load(), 3);
        QCOMPARE(controller.songKeySummary()->confident, 3);
        QVERIFY(controller.songKeyStatusText().contains(QStringLiteral("3 of 3")));
        for (int i = 0; i < keys.size(); ++i) {
            const QString rel = QStringLiteral("Disc 1/KT001-0%1 - Synth Band - Song In %2.mp3")
                                    .arg(i + 1).arg(QString(keys.at(i)).replace(QLatin1Char('#'), QLatin1String("sharp")));
            const auto key = controller.songKey(songIdFor(controller, rel));
            QVERIFY2(key, qPrintable(rel));
            QCOMPARE(songKeyName(key->keyIndex), keys.at(i));
        }
        // Nothing was added to or changed in the music folder.
        QCOMPARE(snapshot(root), before);
        QCOMPARE(rowCount(appDir + QStringLiteral("/enrichment-cache.sqlite"),
                          QStringLiteral("SELECT count(*) FROM song_keys WHERE status='confident'")), 3);

        // A moved and renamed song keeps its key without being read again.
        QVERIFY(QDir().mkpath(root + QStringLiteral("/Moved")));
        for (const char* ext : {".mp3", ".cdg"}) {
            QVERIFY(QFile::rename(root + QStringLiteral("/Disc 1/KT001-02 - Synth Band - Song In Am") + QLatin1String(ext),
                                  root + QStringLiteral("/Moved/KT009-05 - Synth Band - Another Name") + QLatin1String(ext)));
        }
        before = snapshot(root);
        const qsizetype scans = finished.count();
        controller.requestRefreshScan();
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() > scans, 20000);
        QTRY_VERIFY_WITH_TIMEOUT(analysedAll(controller, 3), 60000);
        QCOMPARE(engine->calls.load(), 3);
        const auto moved = controller.songKey(
            songIdFor(controller, QStringLiteral("Moved/KT009-05 - Synth Band - Another Name.mp3")));
        QVERIFY(moved);
        QCOMPARE(songKeyName(moved->keyIndex), QStringLiteral("Am"));
        QCOMPARE(snapshot(root), before);
    }

    // The catalogue rebuilt from nothing: the keys come back from the cache.
    for (const QString& suffix : {QString(), QStringLiteral("-wal"), QStringLiteral("-shm")})
        QFile::remove(catalogue + suffix);
    {
        LibraryController controller(catalogue);
        controller.setSongKeyEngineFactory([engine] { return engine; });
        controller.setSongKeyTimings(quickTimings());
        QSignalSpy finished(&controller, &LibraryController::scanFinished);
        QVERIFY(controller.chooseRoot(root));
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 20000);
        controller.setSongKeyAnalysisEnabled(true);
        QTRY_VERIFY_WITH_TIMEOUT(analysedAll(controller, 3), 60000);
        QCOMPARE(engine->calls.load(), 3);
        const auto key = controller.songKey(
            songIdFor(controller, QStringLiteral("Disc 1/KT001-03 - Synth Band - Song In Fsharpm.mp3")));
        QVERIFY(key);
        QCOMPARE(songKeyName(key->keyIndex), QStringLiteral("F#m"));
    }
    QCOMPARE(snapshot(root), before);
}

void TestSongKeys::damagedSongsAreRecordedNotRetried()
{
    QTemporaryDir temporary;
    const QString root = temporary.filePath(QStringLiteral("music"));
    QVERIFY(QDir().mkpath(root));
    QVERIFY(testmedia::writeFile(root + QStringLiteral("/KT002-01 - Broken - Not Music.mp3"),
                                 QByteArray("not an mp3 at all\n").repeated(500)));
    QVERIFY(testmedia::writeCdg(root + QStringLiteral("/KT002-01 - Broken - Not Music.cdg"),
                                testmedia::markerCdg(3000, 500)));
    auto engine = std::make_shared<CountingEngine>();
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")));
    controller.setSongKeyEngineFactory([engine] { return engine; });
    controller.setSongKeyTimings(quickTimings());
    QSignalSpy finished(&controller, &LibraryController::scanFinished);
    QVERIFY(controller.chooseRoot(root));
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 20000);
    controller.setSongKeyAnalysisEnabled(true);
    QTRY_VERIFY_WITH_TIMEOUT(analysedAll(controller, 1), 30000);
    QCOMPARE(controller.songKeySummary()->confident, 0);
    QTest::qWait(1000);  // the chain has ended: nothing is tried again
    QCOMPARE(engine->calls.load(), 1);
    QCOMPARE(rowCount(temporary.filePath(QStringLiteral("app/enrichment-cache.sqlite")),
                      QStringLiteral("SELECT count(*) FROM song_keys WHERE status='not_audio'")), 1);
    QVERIFY(!controller.songKey(controller.findSongByMp3Path(root, QStringLiteral("KT002-01 - Broken - Not Music.mp3"))));
}

void TestSongKeys::playbackPausesAnalysisAndItResumesLater()
{
    QTemporaryDir temporary;
    const QString root = temporary.filePath(QStringLiteral("music"));
    QVERIFY(QDir().mkpath(root));
    QVERIFY(QFile::copy(QDir(m_fixtures).filePath(QStringLiteral("G.mp3")),
                        root + QStringLiteral("/KT003-01 - Synth Band - G Song.mp3")));
    QVERIFY(testmedia::writeCdg(root + QStringLiteral("/KT003-01 - Synth Band - G Song.cdg"),
                                testmedia::markerCdg(3000, 500)));
    auto endless = std::make_shared<EndlessEngine>();
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")));
    controller.setSongKeyEngineFactory([endless] { return endless; });
    controller.setSongKeyTimings(quickTimings());
    QSignalSpy finished(&controller, &LibraryController::scanFinished);
    QVERIFY(controller.chooseRoot(root));
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 20000);
    controller.setSongKeyAnalysisEnabled(true);
    QTRY_VERIFY_WITH_TIMEOUT(endless->working.load(), 10000);

    // A song starts: the analysis stops at once and stays stopped.
    QElapsedTimer timer;
    timer.start();
    controller.setPlaybackActive(true);
    QTRY_VERIFY_WITH_TIMEOUT(!endless->working.load(), 2000);
    QVERIFY2(timer.elapsed() < kPromptMs, qPrintable(QString::number(timer.elapsed())));
    QTRY_VERIFY_WITH_TIMEOUT(!controller.isAnalysingSongKeys(), 2000);
    QVERIFY(controller.songKeyStatusText().contains(QStringLiteral("Paused while a song plays")));
    const int calls = endless->calls.load();
    QTest::qWait(1000);
    QCOMPARE(endless->calls.load(), calls);
    QVERIFY(!controller.isAnalysingSongKeys());

    // Some time after the song ends it carries on where it was.
    controller.setPlaybackActive(false);
    QTRY_VERIFY_WITH_TIMEOUT(endless->working.load(), 5000);
    QCOMPARE(endless->calls.load(), calls + 1);

    // Turned off: it stops too.
    controller.setSongKeyAnalysisEnabled(false);
    QTRY_VERIFY_WITH_TIMEOUT(!endless->working.load(), 2000);
}

void TestSongKeys::closingDuringAnalysisStopsPromptly()
{
    // With a fake engine that never finishes, then with the real one in the
    // middle of decoding a four-minute song.
    QTemporaryDir temporary;
    for (const bool real : {false, true}) {
        const QString root = temporary.filePath(QStringLiteral("music%1").arg(int(real)));
        QVERIFY(QDir().mkpath(root));
        const QString base = root + QStringLiteral("/KT004-01 - Synth Band - Long Song");
        QVERIFY(testmedia::writeMp3(base + QStringLiteral(".mp3"), real ? 240000 : 2000));
        QVERIFY(testmedia::writeCdg(base + QStringLiteral(".cdg"), testmedia::markerCdg(3000, 500)));
        auto endless = std::make_shared<EndlessEngine>();
        auto counting = std::make_shared<CountingEngine>();
        LibraryController controller(temporary.filePath(QStringLiteral("app%1/library.sqlite").arg(int(real))));
        if (real)
            controller.setSongKeyEngineFactory([counting] { return counting; });
        else
            controller.setSongKeyEngineFactory([endless] { return endless; });
        controller.setSongKeyTimings(quickTimings());
        QSignalSpy finished(&controller, &LibraryController::scanFinished);
        QVERIFY(controller.chooseRoot(root));
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 20000);
        controller.setSongKeyAnalysisEnabled(true);
        if (real) {
            QTRY_VERIFY_WITH_TIMEOUT(counting->calls.load() >= 1, 20000);
            QTest::qWait(40);  // well into the decode (which takes several hundred ms)
            QVERIFY(controller.isAnalysingSongKeys());
        } else {
            QTRY_VERIFY_WITH_TIMEOUT(endless->working.load(), 10000);
        }
        QElapsedTimer timer;
        timer.start();
        QVERIFY(controller.stopScanner(5000));
        QVERIFY2(timer.elapsed() < kPromptMs, qPrintable(QString::number(timer.elapsed())));
        QVERIFY(!controller.scannerRunning());
        // Nothing half-done was recorded.
        if (real) {
            QCOMPARE(rowCount(temporary.filePath(QStringLiteral("app1/enrichment-cache.sqlite")),
                              QStringLiteral("SELECT count(*) FROM song_keys")), 0);
        }
    }
}

void TestSongKeys::analysisIsOffUntilTurnedOn()
{
    QTemporaryDir temporary;
    const QString root = temporary.filePath(QStringLiteral("music"));
    QVERIFY(QDir().mkpath(root));
    QVERIFY(QFile::copy(QDir(m_fixtures).filePath(QStringLiteral("Em.mp3")),
                        root + QStringLiteral("/KT005-01 - Synth Band - Em Song.mp3")));
    QVERIFY(testmedia::writeCdg(root + QStringLiteral("/KT005-01 - Synth Band - Em Song.cdg"),
                                testmedia::markerCdg(3000, 500)));
    auto engine = std::make_shared<CountingEngine>();
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")));
    controller.setSongKeyEngineFactory([engine] { return engine; });
    controller.setSongKeyTimings(quickTimings());
    QSignalSpy finished(&controller, &LibraryController::scanFinished);
    QVERIFY(controller.chooseRoot(root));
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 20000);
    QTest::qWait(500);
    QVERIFY(!controller.songKeyAnalysisEnabled());
    QCOMPARE(engine->calls.load(), 0);
    // Progress can be counted without analysing anything.
    controller.requestSongKeySummary();
    QTRY_VERIFY_WITH_TIMEOUT(controller.songKeySummary().has_value(), 5000);
    QCOMPARE(controller.songKeySummary()->total, 1);
    QCOMPARE(controller.songKeySummary()->analysed, 0);
    QCOMPARE(engine->calls.load(), 0);
    QVERIFY(controller.songKeyStatusText().contains(QStringLiteral("0 of 1")));
}

void TestSongKeys::libraryAndPlayerBarShowTheKeys()
{
    // The table's Key column: compact, blank when not known.
    LibraryResultsModel model;
    QCOMPARE(model.headerData(LibraryResultsModel::KeyColumn, Qt::Horizontal).toString(), QStringLiteral("KEY"));
    CatalogueSearchRow known;
    known.songId = 1;
    CatalogueSearchRow unknown;
    unknown.songId = 2;
    int lookups = 0;
    model.setKeyProvider([&lookups](qint64 songId) {
        ++lookups;
        return songId == 1 ? QStringLiteral("F#m") : QString();
    });
    model.setRows({known, unknown});
    QCOMPARE(model.index(0, LibraryResultsModel::KeyColumn).data().toString(), QStringLiteral("F#m"));
    QCOMPARE(model.index(1, LibraryResultsModel::KeyColumn).data().toString(), QString());
    QCOMPARE(model.index(0, 0).data(LibraryResultsModel::KeyRole).toString(), QStringLiteral("F#m"));
    QCOMPARE(lookups, 2);  // remembered until keys change
    QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
    model.forgetKeys();
    QCOMPARE(changed.count(), 1);
    QCOMPARE(model.index(0, LibraryResultsModel::KeyColumn).data().toString(), QStringLiteral("F#m"));
    QCOMPARE(lookups, 3);

    // The player bar: original key, and the key heard with Key applied.
    QTemporaryDir temporary;
    const QString root = temporary.filePath(QStringLiteral("music"));
    QVERIFY(QDir().mkpath(root));
    const QString mp3 = root + QStringLiteral("/KT006-01 - Synth Band - C Song.mp3");
    QVERIFY(QFile::copy(QDir(m_fixtures).filePath(QStringLiteral("C.mp3")), mp3));
    QVERIFY(testmedia::writeCdg(root + QStringLiteral("/KT006-01 - Synth Band - C Song.cdg"),
                                testmedia::markerCdg(3000, 500)));
    const QString other = root + QStringLiteral("/KT006-02 - Synth Band - Unknown Key.mp3");
    QVERIFY(testmedia::writeMp3(other, 2000));
    QVERIFY(testmedia::writeCdg(root + QStringLiteral("/KT006-02 - Synth Band - Unknown Key.cdg"),
                                testmedia::markerCdg(2000, 500)));
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")));
    controller.setSongKeyTimings(quickTimings());
    QSignalSpy finished(&controller, &LibraryController::scanFinished);
    QVERIFY(controller.chooseRoot(root));
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 20000);

    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings, &controller);
    window.setShowErrorDialogs(false);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.openSong(mp3));
    QVERIFY(!window.songKeyLabel()->isVisible());  // not analysed yet

    controller.setSongKeyAnalysisEnabled(true);  // the real GStreamer engine
    QTRY_VERIFY_WITH_TIMEOUT(analysedAll(controller, 2), 60000);
    QTRY_VERIFY_WITH_TIMEOUT(window.songKeyLabel()->isVisible(), 5000);
    QCOMPARE(window.songKeyLabel()->text(), QStringLiteral("(C)"));
    QCOMPARE(window.songKeyLabel()->toolTip(), QStringLiteral("Original key: C"));
    QCOMPARE(window.keyValueLabel()->text(), QStringLiteral("0"));  // Key is still the transpose
    QTest::mouseClick(window.keyUpButton(), Qt::LeftButton);
    QTest::mouseClick(window.keyUpButton(), Qt::LeftButton);
    QCOMPARE(window.keyValueLabel()->text(), QStringLiteral("+2"));
    QCOMPARE(window.songKeyLabel()->text(), QStringLiteral("(C → D)"));
    QCOMPARE(window.songKeyLabel()->toolTip(), QStringLiteral("Original key: C\nCurrent key: D (+2)"));
    QTest::mouseClick(window.keyDownButton(), Qt::LeftButton);
    QTest::mouseClick(window.keyDownButton(), Qt::LeftButton);
    QTest::mouseClick(window.keyDownButton(), Qt::LeftButton);
    QTest::mouseClick(window.keyDownButton(), Qt::LeftButton);
    QCOMPARE(window.songKeyLabel()->text(), QStringLiteral("(C → Bb)"));
    QCOMPARE(window.songKeyLabel()->toolTip(), QStringLiteral("Original key: C\nCurrent key: Bb (-2)"));

    // A song with no clear key (two seconds of one tone) shows none.
    QVERIFY(window.openSong(other));
    QVERIFY(!window.songKeyLabel()->isVisible());

    // The library table shows the key in its own column.
    LibraryView* view = window.libraryView();
    view->searchBox()->setText(QStringLiteral("C Song"));
    QTRY_COMPARE_WITH_TIMEOUT(view->songResultCount(), 1, 3000);
    QCOMPARE(view->resultsList()->model()->index(0, LibraryResultsModel::KeyColumn).data().toString(),
             QStringLiteral("C"));
}

QTEST_MAIN(TestSongKeys)
#include "tst_songkeys.moc"
