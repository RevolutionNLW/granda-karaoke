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
#include "SongKeyPicker.h"
#include "library/Catalogue.h"
#include "library/MetadataOverrideStore.h"
#include "library/SongKeys.h"
#include "music/KeyDetector.h"
#include "music/MusicalKey.h"
#include "ui/Theme.h"

#include "TestMedia.h"

#include <QApplication>
#include <QDateTime>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QLabel>
#include <QPointer>
#include <QHeaderView>
#include <QLineEdit>
#include <QMutexLocker>
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
#include <limits>
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

// An installation whose audio decoder does not work.
class BrokenEngine final : public SongKeyEngine {
public:
    std::atomic_int calls = 0;
    Outcome analyse(const QString&, const std::function<bool()>&, KeyAnalysis*, QString* detail) override
    {
        ++calls;
        *detail = QStringLiteral("no decoder");
        return Outcome::EngineUnavailable;
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

// The key the library shows for a song ("" for none).
QString shownKey(const LibraryController& controller, qint64 songId)
{
    const std::optional<SongKeyInfo> key = controller.songKey(songId);
    return key ? songKeyName(key->shownKeyIndex()) : QString();
}

// A music folder with a G major song and a song with no clear key, as used by
// the manual-key tests. Returns the two MP3 file names.
QPair<QString, QString> writeManualKeyLibrary(const QString& fixtures, const QString& root)
{
    const QString known = QStringLiteral("KT010-01 - Synth Band - G Song.mp3");
    const QString unknown = QStringLiteral("KT010-02 - Synth Band - Unknown Key.mp3");
    if (!QDir().mkpath(root) || !QFile::copy(QDir(fixtures).filePath(QStringLiteral("G.mp3")), root + QLatin1Char('/') + known)
        || !testmedia::writeCdg(root + QStringLiteral("/KT010-01 - Synth Band - G Song.cdg"), testmedia::markerCdg(3000, 500))
        || !testmedia::writeMp3(root + QLatin1Char('/') + unknown, 2000)
        || !testmedia::writeCdg(root + QStringLiteral("/KT010-02 - Synth Band - Unknown Key.cdg"),
                                testmedia::markerCdg(2000, 500)))
        return {};
    return {known, unknown};
}

// The song in the library's results with the given title, or -1.
int resultRow(LibraryView* view, const QString& title)
{
    QAbstractItemModel* model = view->resultsList()->model();
    for (int row = 0; row < view->songResultCount(); ++row) {
        if (model->index(row, 0).data().toString() == title)
            return row;
    }
    return -1;
}

QList<SongKeyPicker*> openPickers()
{
    QList<SongKeyPicker*> pickers;
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (auto* picker = qobject_cast<SongKeyPicker*>(widget); picker && picker->isVisible())
            pickers.append(picker);
    }
    return pickers;
}

// Every row of a metadata override store, read on a connection of its own.
QList<MetadataOverride> storedOverrides(const QString& path)
{
    MetadataOverrideStore store(path);
    QString error;
    if (!store.open(&error))
        return {};
    return store.all(&error);
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
    void aBrokenDecoderStopsAnalysisWithoutBlamingSongs();
    void libraryAndPlayerBarShowTheKeys();
    void manualOriginalKeyWinsAndSurvives();
    void manualOriginalKeyIsSeparateFromTranspose();
    void manualKeyAloneShowsTheKeyColumn();
    void correctionsFollowAMovedMusicFolder();
    void anOldFolderCopyNeverUndoesAClear();
    void aCopyLeftByAFailedTidyUpNeverUndoesAClear();
    void anEditBeforeTheSyncKeepsTheOtherValues();
    void setSongKeyPopupChoosesAndClears();
    void keyCellClickIsLikeTheRestOfTheRow();
    void setSongKeyFitsTheLibraryFooter();

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
        QVERIFY2(keyaudio::writeMp3(path, bandIn(fixture.tonic, fixture.minor, 70), kRate), fixture.name);
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
    QCOMPARE(parsed("CM"), QStringLiteral("C"));  // capital M is major, as on chord charts
    QCOMPARE(parsed("Cm"), QStringLiteral("Cm"));
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
    QTest::addRow("a DC offset and nothing else") << 8 << int(KeyAnalysis::Status::Silent);
    QTest::addRow("a sub-bass hum below the band") << 9 << -1;
    QTest::addRow("drums alone") << 10 << -1;
    QTest::addRow("a quarter-tone off A 440") << 11 << -1;
    QTest::addRow("Am-G-F-G loop") << 12 << -1;
    QTest::addRow("damaged samples (NaN)") << 13 << -1;
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
    case 8: synth.out.assign(std::size_t(60 * kRate), 0.01F); break;
    case 9:
        // A pure 30 Hz hum, all below the band analysed.
        for (std::size_t i = 0, n = std::size_t(60 * kRate); i < n; ++i)
            synth.out.push_back(float(0.5 * std::sin(2.0 * std::numbers::pi * 30.0 * double(i) / kRate)));
        break;
    case 10: {
        synth.out.assign(std::size_t(60 * kRate), 0.0F);
        keyaudio::Random rng(7);
        const std::size_t beat = std::size_t(0.5 * kRate);
        for (std::size_t b = 0; b * beat < synth.out.size(); ++b) {
            for (std::size_t i = 0; i < std::size_t(0.15 * kRate) && b * beat + i < synth.out.size(); ++i) {
                const double envelope = std::exp(-double(i) / (0.03 * kRate));
                synth.out[b * beat + i] += float((b % 2 ? 0.25 : 0.12) * envelope * rng.gaussian());
                if (b % 2 == 0)
                    synth.out[b * beat + i] += float(0.4 * envelope
                        * std::sin(2.0 * std::numbers::pi * (60.0 + 40.0 * envelope) * double(i) / kRate));
            }
        }
        break;
    }
    case 11: keyaudio::progression(synth, 60, false, 90, 2.0, 50.0); break;
    case 12:
        for (int i = 0; i < 40; ++i) {
            synth.chord(keyaudio::triad(57, true), 2.0);
            synth.chord(keyaudio::triad(55, false), 2.0);
            synth.chord(keyaudio::triad(53, false), 2.0);
            synth.chord(keyaudio::triad(55, false), 2.0);
        }
        break;
    case 13:
        keyaudio::progression(synth, 60, false, 60);
        for (std::size_t i = 0; i < synth.out.size(); i += 1000)
            synth.out[i] = std::numeric_limits<float>::quiet_NaN();
        break;
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
    // A long drums-only break or a sub-bass hum does not pull the key
    // elsewhere: the answer is the music's key or none.
    {
        keyaudio::Synth synth(kRate);
        keyaudio::band(synth, 55, false, 60);
        for (std::size_t i = 0, n = std::size_t(40 * kRate); i < n; ++i)  // a 45 Hz hum
            synth.out.push_back(float(0.5 * std::sin(2.0 * std::numbers::pi * 45.0 * double(i) / kRate)));
        const KeyAnalysis result = KeyDetector::analyse(synth.out);
        QVERIFY2(result.status != KeyAnalysis::Status::Confident || keyName(result) == QStringLiteral("G"),
                 qPrintable(describe(result)));
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
        // 70 s asked for; the chords are whole 2-second steps.
        QVERIFY2(result.seconds > 69.0 && result.seconds < 72.0, qPrintable(QString::number(result.seconds)));
    }
    qInfo("Decoded and analysed %d 70-second MP3s in %lld ms (%lld ms each)",
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
    keyaudio::Random rng(3);
    for (char& c : noise)
        c = char(rng.next() & 0xff);
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
    QVERIFY(result.seconds < 30.0);
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

    // Opening a song gets it off the drive at once too, and it waits.
    timer.restart();
    controller.holdSongKeysForSong();
    QTRY_VERIFY_WITH_TIMEOUT(!endless->working.load(), 2000);
    QVERIFY2(timer.elapsed() < kPromptMs, qPrintable(QString::number(timer.elapsed())));
    QTRY_VERIFY_WITH_TIMEOUT(endless->working.load(), 5000);

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

void TestSongKeys::aBrokenDecoderStopsAnalysisWithoutBlamingSongs()
{
    QTemporaryDir temporary;
    const QString root = temporary.filePath(QStringLiteral("music"));
    QVERIFY(QDir().mkpath(root));
    for (const QString& name : {QStringLiteral("KT007-01 - Synth Band - One"), QStringLiteral("KT007-02 - Synth Band - Two")}) {
        QVERIFY(QFile::copy(QDir(m_fixtures).filePath(QStringLiteral("C.mp3")), root + QLatin1Char('/') + name + QStringLiteral(".mp3")));
        QVERIFY(testmedia::writeCdg(root + QLatin1Char('/') + name + QStringLiteral(".cdg"), testmedia::markerCdg(3000, 500)));
    }
    auto broken = std::make_shared<BrokenEngine>();
    std::atomic_int made = 0;
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")));
    controller.setSongKeyEngineFactory([broken, &made] { ++made; return broken; });
    controller.setSongKeyTimings(quickTimings());
    QSignalSpy finished(&controller, &LibraryController::scanFinished);
    QVERIFY(controller.chooseRoot(root));
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 20000);
    QSignalSpy summary(&controller, &LibraryController::songKeySummaryChanged);
    controller.setSongKeyAnalysisEnabled(true);
    QTRY_VERIFY_WITH_TIMEOUT(broken->calls.load() == 1 && !controller.isAnalysingSongKeys(), 10000);
    QTest::qWait(1000);
    QCOMPARE(broken->calls.load(), 1);  // stopped at the first song, not tried on every one
    QTRY_VERIFY_WITH_TIMEOUT(controller.songKeySummary().has_value(), 5000);
    QVERIFY2(controller.songKeyStatusText().contains(QStringLiteral("cannot be worked out")),
             qPrintable(controller.songKeyStatusText()));
    // Nothing was recorded against the songs.
    QCOMPARE(rowCount(temporary.filePath(QStringLiteral("app/enrichment-cache.sqlite")),
                      QStringLiteral("SELECT count(*) FROM song_keys")), 0);
    // After the next scan the decoder is made afresh and tried again.
    const qsizetype scans = finished.count();
    controller.requestRefreshScan();
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() > scans, 20000);
    QTRY_VERIFY_WITH_TIMEOUT(broken->calls.load() == 2, 10000);
    QCOMPARE(made.load(), 2);
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
    window.resize(1280, 800);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.openSong(mp3));
    QVERIFY(!window.songKeyLabel()->isVisible());  // not analysed yet
    // No empty Key column while there is nothing to show in it.
    QTreeView* results = window.libraryView()->resultsList();
    QTRY_VERIFY_WITH_TIMEOUT(controller.songKeySummary().has_value(), 5000);
    QVERIFY(results->isColumnHidden(LibraryResultsModel::KeyColumn));

    controller.setSongKeyAnalysisEnabled(true);  // the real GStreamer engine
    QVERIFY(!results->isColumnHidden(LibraryResultsModel::KeyColumn));
    QTRY_VERIFY_WITH_TIMEOUT(analysedAll(controller, 2), 60000);
    QTRY_VERIFY_WITH_TIMEOUT(window.songKeyLabel()->isVisible(), 5000);
    QCOMPARE(window.songKeyLabel()->text(), QStringLiteral("Key C"));
    QCOMPARE(window.songKeyLabel()->toolTip(), QStringLiteral("Original key: C"));
    QCOMPARE(window.keyValueLabel()->text(), QStringLiteral("0"));  // Key is still the transpose
    QTest::mouseClick(window.keyUpButton(), Qt::LeftButton);
    QTest::mouseClick(window.keyUpButton(), Qt::LeftButton);
    QCOMPARE(window.keyValueLabel()->text(), QStringLiteral("+2"));
    QCOMPARE(window.songKeyLabel()->text(), QStringLiteral("Key C → D"));
    QCOMPARE(window.songKeyLabel()->toolTip(), QStringLiteral("Original key: C\nCurrent key: D (+2)"));
    QTest::mouseClick(window.keyDownButton(), Qt::LeftButton);
    QTest::mouseClick(window.keyDownButton(), Qt::LeftButton);
    QTest::mouseClick(window.keyDownButton(), Qt::LeftButton);
    QTest::mouseClick(window.keyDownButton(), Qt::LeftButton);
    QCOMPARE(window.songKeyLabel()->text(), QStringLiteral("Key C → Bb"));
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

void TestSongKeys::manualOriginalKeyWinsAndSurvives()
{
    QTemporaryDir temporary;
    const QString root = temporary.filePath(QStringLiteral("music"));
    const auto [known, unknown] = writeManualKeyLibrary(m_fixtures, root);
    QVERIFY(!known.isEmpty());
    const QString appDir = temporary.filePath(QStringLiteral("app"));
    const QString catalogue = appDir + QStringLiteral("/library.sqlite");
    const QString overrides = appDir + QStringLiteral("/metadata-overrides.sqlite");
    const QString cache = appDir + QStringLiteral("/enrichment-cache.sqlite");
    const QString storedKeys = QStringLiteral("SELECT count(*) FROM metadata_overrides WHERE original_key=");
    const QString detectedG = QStringLiteral("SELECT count(*) FROM song_keys WHERE status='confident' AND key_index=7");
    const QMap<QString, QPair<qint64, qint64>> before = snapshot(root);
    QString error;
    {
        LibraryController controller(catalogue, {}, overrides);
        controller.setSongKeyTimings(quickTimings());
        QSignalSpy finished(&controller, &LibraryController::scanFinished);
        QVERIFY(controller.chooseRoot(root));
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 20000);
        controller.setSongKeyAnalysisEnabled(true);
        QTRY_VERIFY_WITH_TIMEOUT(analysedAll(controller, 2), 60000);
        const qint64 song = controller.findSongByMp3Path(root, known);
        const qint64 other = controller.findSongByMp3Path(root, unknown);
        QVERIFY(song > 0 && other > 0);
        QCOMPARE(shownKey(controller, song), QStringLiteral("G"));  // detected
        QCOMPARE(shownKey(controller, other), QString());

        // A: the user's original key is shown instead; the detected one is kept.
        QSignalSpy keysChanged(&controller, &LibraryController::songKeysChanged);
        QSignalSpy catalogueChanged(&controller, &LibraryController::catalogueChanged);
        QVERIFY2(controller.setManualOriginalKey(song, 0, &error), qPrintable(error));
        QCOMPARE(shownKey(controller, song), QStringLiteral("C"));
        const auto details = controller.songKeyDetails(song);
        QVERIFY(details && details->isManual());
        QCOMPARE(details->manualKeyIndex, 0);
        QCOMPARE(details->detectedKeyIndex(), 7);
        QCOMPARE(rowCount(cache, detectedG), 1);
        QCOMPARE(rowCount(overrides, storedKeys + QStringLiteral("0")), 1);
        QCOMPARE(keysChanged.count(), 1);
        QCOMPARE(catalogueChanged.count(), 0);  // the library keeps its place
        // A key is not a name correction, and only real keys are accepted.
        QCOMPARE(rowCount(catalogue, QStringLiteral("SELECT count(*) FROM songs WHERE manual_title IS NOT NULL "
                                                    "OR manual_artist IS NOT NULL")), 0);
        QVERIFY(!controller.setManualOriginalKey(song, 24, &error));
        QVERIFY(!controller.setManualOriginalKey(song, -1, &error));
        QCOMPARE(shownKey(controller, song), QStringLiteral("C"));

        // E: analysing the song again finds G again, and C still wins.
        QCOMPARE(rowCount(cache, QStringLiteral("SELECT count(*) FROM song_keys")), 2);
        {
            const QString name = QUuid::createUuid().toString(QUuid::WithoutBraces);
            {
                QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
                database.setDatabaseName(cache);
                QVERIFY(database.open());
                QSqlQuery remove(database);
                QVERIFY(remove.exec(QStringLiteral("DELETE FROM song_keys")));
                database.close();
            }
            QSqlDatabase::removeDatabase(name);
        }
        controller.setSongKeyAnalysisEnabled(false);
        controller.setSongKeyAnalysisEnabled(true);
        QTRY_COMPARE_WITH_TIMEOUT(rowCount(cache, QStringLiteral("SELECT count(*) FROM song_keys")), 2LL, 60000);
        QCOMPARE(rowCount(cache, detectedG), 1);
        QCOMPARE(shownKey(controller, song), QStringLiteral("C"));
        controller.setSongKeyAnalysisEnabled(false);

        // Name corrections and "Use Automatic Name" leave the key alone.
        QVERIFY2(controller.setManualOverride(song, QStringLiteral("Someone"), QStringLiteral("Something"), &error),
                 qPrintable(error));
        QCOMPARE(shownKey(controller, song), QStringLiteral("C"));
        QVERIFY2(controller.clearManualOverride(song, &error), qPrintable(error));
        QCOMPARE(shownKey(controller, song), QStringLiteral("C"));
        QCOMPARE(rowCount(overrides, QStringLiteral("SELECT count(*) FROM metadata_overrides WHERE title IS NOT NULL")), 0);

        // A song with no detected key can be given one too.
        QVERIFY2(controller.setManualOriginalKey(other, 21, &error), qPrintable(error));
        QCOMPARE(shownKey(controller, other), QStringLiteral("Am"));

        // A library check (rescan) re-applies the corrections it keeps.
        finished.clear();
        controller.requestRefreshScan();
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 20000);
        QCOMPARE(shownKey(controller, song), QStringLiteral("C"));
        QCOMPARE(shownKey(controller, other), QStringLiteral("Am"));
    }
    // D: after restarting.
    {
        LibraryController controller(catalogue, {}, overrides);
        QCOMPARE(shownKey(controller, controller.findSongByMp3Path(root, known)), QStringLiteral("C"));
        QCOMPARE(shownKey(controller, controller.findSongByMp3Path(root, unknown)), QStringLiteral("Am"));
    }
    // H: the catalogue is deleted and rebuilt from the music folder.
    for (const QString& suffix : {QString(), QStringLiteral("-wal"), QStringLiteral("-shm")})
        QFile::remove(catalogue + suffix);
    {
        LibraryController controller(catalogue, {}, overrides);
        QSignalSpy finished(&controller, &LibraryController::scanFinished);
        QVERIFY(controller.chooseRoot(root));
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 20000);
        const qint64 song = controller.findSongByMp3Path(root, known);
        QCOMPARE(shownKey(controller, song), QStringLiteral("C"));
        QCOMPARE(shownKey(controller, controller.findSongByMp3Path(root, unknown)), QStringLiteral("Am"));
        // Analysis finds the detected key again in its cache, without decoding,
        // and the user's key still wins.
        auto engine = std::make_shared<CountingEngine>();
        controller.setSongKeyEngineFactory([engine] { return engine; });
        controller.setSongKeyTimings(quickTimings());
        controller.setSongKeyAnalysisEnabled(true);
        QTRY_COMPARE_WITH_TIMEOUT(controller.songKeyDetails(song)->detectedKeyIndex(), 7, 30000);
        QCOMPARE(engine->calls.load(), 0);
        controller.setSongKeyAnalysisEnabled(false);
        QCOMPARE(shownKey(controller, song), QStringLiteral("C"));
    }
    // A lost store is rebuilt from the catalogue's copy, keys included.
    for (const QString& suffix : {QString(), QStringLiteral("-wal"), QStringLiteral("-shm")})
        QFile::remove(overrides + suffix);
    {
        LibraryController controller(catalogue, {}, overrides);
        QCOMPARE(rowCount(overrides, storedKeys + QStringLiteral("0")), 1);
        QCOMPARE(rowCount(overrides, storedKeys + QStringLiteral("21")), 1);
        const qint64 song = controller.findSongByMp3Path(root, known);
        const qint64 other = controller.findSongByMp3Path(root, unknown);
        // F: clearing goes back to the detected key; the detected key is untouched.
        QVERIFY2(controller.setManualOriginalKey(song, std::nullopt, &error), qPrintable(error));
        QCOMPARE(shownKey(controller, song), QStringLiteral("G"));
        QVERIFY(!controller.songKeyDetails(song)->isManual());
        QCOMPARE(rowCount(cache, detectedG), 1);
        // G: with nothing detected, clearing leaves the key blank.
        QVERIFY2(controller.setManualOriginalKey(other, std::nullopt, &error), qPrintable(error));
        QCOMPARE(shownKey(controller, other), QString());
        QCOMPARE(rowCount(overrides, QStringLiteral("SELECT count(*) FROM metadata_overrides")), 0);
        QCOMPARE(rowCount(catalogue, QStringLiteral("SELECT count(*) FROM songs WHERE manual_original_key IS NOT NULL")), 0);
    }
    QCOMPARE(snapshot(root), before);  // nothing written to the music
}

void TestSongKeys::manualOriginalKeyIsSeparateFromTranspose()
{
    QTemporaryDir temporary;
    const QString root = temporary.filePath(QStringLiteral("music"));
    const auto [known, unknown] = writeManualKeyLibrary(m_fixtures, root);
    QVERIFY(!known.isEmpty());
    const QString overrides = temporary.filePath(QStringLiteral("app/metadata-overrides.sqlite"));
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")), {}, overrides);
    controller.setSongKeyTimings(quickTimings());
    QSignalSpy finished(&controller, &LibraryController::scanFinished);
    QVERIFY(controller.chooseRoot(root));
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 20000);
    controller.setSongKeyAnalysisEnabled(true);
    QTRY_VERIFY_WITH_TIMEOUT(analysedAll(controller, 2), 60000);
    controller.setSongKeyAnalysisEnabled(false);
    const qint64 song = controller.findSongByMp3Path(root, known);

    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings, &controller);
    window.setShowErrorDialogs(false);
    window.resize(1280, 800);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.openSong(root + QLatin1Char('/') + known));
    QCOMPARE(window.songKeyLabel()->text(), QStringLiteral("Key G"));  // detected

    // The loaded song's Now Playing key follows a key set by hand at once.
    QString error;
    QVERIFY2(controller.setManualOriginalKey(song, 0, &error), qPrintable(error));
    QCOMPARE(window.songKeyLabel()->text(), QStringLiteral("Key C"));
    const QString storedC = QStringLiteral("SELECT count(*) FROM metadata_overrides WHERE original_key=0");
    // B: Key +/- changes the key heard, never the original.
    const auto check = [&](int clicks, QPushButton* button, const QString& expected) {
        for (int i = 0; i < clicks; ++i)
            QTest::mouseClick(button, Qt::LeftButton);
        QCOMPARE(window.songKeyLabel()->text(), expected);
        QCOMPARE(controller.songKeyDetails(song)->manualKeyIndex, 0);
        QCOMPARE(rowCount(overrides, storedC), 1);
    };
    check(0, window.keyUpButton(), QStringLiteral("Key C"));
    check(1, window.keyUpButton(), QStringLiteral("Key C → Db"));
    check(1, window.keyUpButton(), QStringLiteral("Key C → D"));
    QCOMPARE(window.keyValueLabel()->text(), QStringLiteral("+2"));
    check(4, window.keyDownButton(), QStringLiteral("Key C → Bb"));
    QCOMPARE(window.keyValueLabel()->text(), QStringLiteral("-2"));
    check(1, window.keyUpButton(), QStringLiteral("Key C → B"));
    // The original chosen while transposed is still the original.
    QTest::mouseClick(window.keyUpButton(), Qt::LeftButton);
    QTest::mouseClick(window.keyUpButton(), Qt::LeftButton);
    QTest::mouseClick(window.keyUpButton(), Qt::LeftButton);
    QCOMPARE(window.keyValueLabel()->text(), QStringLiteral("+2"));
    QVERIFY2(controller.setManualOriginalKey(song, 9, &error), qPrintable(error));
    QCOMPARE(window.songKeyLabel()->text(), QStringLiteral("Key A → B"));
    QCOMPARE(player.keySemitones(), 2);  // the transpose is untouched
    // C: a minor key.
    QVERIFY2(controller.setManualOriginalKey(song, 21, &error), qPrintable(error));
    QCOMPARE(window.songKeyLabel()->text(), QStringLiteral("Key Am → Bm"));
    QTest::mouseClick(window.keyResetButton(), Qt::LeftButton);
    QCOMPARE(window.songKeyLabel()->text(), QStringLiteral("Key Am"));
    QCOMPARE(controller.songKeyDetails(song)->manualKeyIndex, 21);
    // Cleared: back to the detected key, still with the transpose applied.
    QTest::mouseClick(window.keyUpButton(), Qt::LeftButton);
    QVERIFY2(controller.setManualOriginalKey(song, std::nullopt, &error), qPrintable(error));
    QCOMPARE(window.songKeyLabel()->text(), QStringLiteral("Key G → Ab"));
    QCOMPARE(player.keySemitones(), 1);
}

void TestSongKeys::manualKeyAloneShowsTheKeyColumn()
{
    // Analysis never turned on: a key chosen by hand still shows, the Key
    // column appears for it, Settings counts it, and other trusted values
    // (an imported label) are kept when it is set and cleared.
    QTemporaryDir temporary;
    const QString root = temporary.filePath(QStringLiteral("music"));
    const auto [known, unknown] = writeManualKeyLibrary(m_fixtures, root);
    QVERIFY(!known.isEmpty());
    const QString overrides = temporary.filePath(QStringLiteral("app/metadata-overrides.sqlite"));
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")), {}, overrides);
    QSignalSpy finished(&controller, &LibraryController::scanFinished);
    QVERIFY(controller.chooseRoot(root));
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 20000);
    const qint64 song = controller.findSongByMp3Path(root, unknown);
    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings, &controller);
    window.setShowErrorDialogs(false);
    window.resize(1280, 800);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QTreeView* results = window.libraryView()->resultsList();
    QTRY_VERIFY_WITH_TIMEOUT(controller.songKeySummary().has_value(), 5000);
    QVERIFY(results->isColumnHidden(LibraryResultsModel::KeyColumn));

    MetadataOverride imported = controller.existingTrusted(song);
    imported.label = QStringLiteral("Sunfly");
    imported.origin = QStringLiteral("import");
    QString error;
    QVERIFY2(controller.setTrustedMetadata(song, imported, &error), qPrintable(error));
    QVERIFY2(controller.setManualOriginalKey(song, 2, &error), qPrintable(error));
    QTRY_VERIFY_WITH_TIMEOUT(!results->isColumnHidden(LibraryResultsModel::KeyColumn), 5000);
    QCOMPARE(controller.songKeySummary()->manual, 1);
    QVERIFY(controller.songKeyStatusText().contains(QStringLiteral("1 set by hand")));
    QCOMPARE(shownKey(controller, song), QStringLiteral("D"));
    QCOMPARE(controller.existingTrusted(song).origin, QStringLiteral("import"));
    QCOMPARE(*controller.existingTrusted(song).label, QStringLiteral("Sunfly"));
    window.libraryView()->searchBox()->setText(QStringLiteral("Unknown Key"));
    QTRY_COMPARE_WITH_TIMEOUT(window.libraryView()->songResultCount(), 1, 3000);
    QCOMPARE(results->model()->index(0, LibraryResultsModel::KeyColumn).data().toString(), QStringLiteral("D"));

    // Another music folder without keys, and back: the column and the count follow.
    const QString second = temporary.filePath(QStringLiteral("music2"));
    QVERIFY(QDir().mkpath(second));
    QVERIFY(testmedia::writeMp3(second + QStringLiteral("/KT011-01 - Other Band - Plain Song.mp3"), 2000));
    QVERIFY(testmedia::writeCdg(second + QStringLiteral("/KT011-01 - Other Band - Plain Song.cdg"),
                                testmedia::markerCdg(2000, 500)));
    finished.clear();
    QVERIFY(controller.chooseRoot(second));
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 20000);
    QTRY_VERIFY_WITH_TIMEOUT(controller.songKeySummary() && controller.songKeySummary()->manual == 0
                                 && controller.songKeySummary()->total == 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(results->isColumnHidden(LibraryResultsModel::KeyColumn), 5000);
    QVERIFY(!controller.songKeyStatusText().contains(QStringLiteral("set by hand")));
    finished.clear();
    QVERIFY(controller.chooseRoot(root));
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 20000);
    QTRY_VERIFY_WITH_TIMEOUT(controller.songKeySummary() && controller.songKeySummary()->manual == 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!results->isColumnHidden(LibraryResultsModel::KeyColumn), 5000);
    QCOMPARE(shownKey(controller, song), QStringLiteral("D"));
    window.libraryView()->searchBox()->setText(QStringLiteral("Unknown Key"));
    QTRY_COMPARE_WITH_TIMEOUT(window.libraryView()->songResultCount(), 1, 3000);

    // Cleared: the label stays, the key and the column go.
    QVERIFY2(controller.setManualOriginalKey(song, std::nullopt, &error), qPrintable(error));
    QCOMPARE(shownKey(controller, song), QString());
    QCOMPARE(controller.existingTrusted(song).origin, QStringLiteral("import"));
    QCOMPARE(*controller.existingTrusted(song).label, QStringLiteral("Sunfly"));
    QVERIFY(!controller.existingTrusted(song).originalKey);
    QTRY_VERIFY_WITH_TIMEOUT(results->isColumnHidden(LibraryResultsModel::KeyColumn), 5000);
    QCOMPARE(controller.songKeySummary()->manual, 0);
    QCOMPARE(results->model()->index(0, LibraryResultsModel::KeyColumn).data().toString(), QString());
}

void TestSongKeys::correctionsFollowAMovedMusicFolder()
{
    // The music folder moves (on Windows the USB drive gets another letter)
    // and is chosen again in Settings > Library > Change...; the catalogue is
    // kept, and the old folder's songs are still listed as present in it.
    // Corrections made before the move show on the songs at the new folder,
    // and editing or clearing them there sticks through rescans and restarts.
    QTemporaryDir temporary;
    const QString rootA = temporary.filePath(QStringLiteral("music-a"));
    const QString rootB = temporary.filePath(QStringLiteral("music-b"));
    const auto [known, unknown] = writeManualKeyLibrary(m_fixtures, rootA);
    QVERIFY(!known.isEmpty());
    QVERIFY(!writeManualKeyLibrary(m_fixtures, rootB).first.isEmpty());
    const QString appDir = temporary.filePath(QStringLiteral("app"));
    const QString catalogue = appDir + QStringLiteral("/library.sqlite");
    const QString overrides = appDir + QStringLiteral("/metadata-overrides.sqlite");
    const QString canonicalA = Catalogue::canonicalPath(rootA);
    const QString canonicalB = Catalogue::canonicalPath(rootB);
    const QMap<QString, QPair<qint64, qint64>> beforeA = snapshot(rootA);
    const QMap<QString, QPair<qint64, qint64>> beforeB = snapshot(rootB);
    auto storedAt = [&](const QString& root) {
        QStringList where;
        for (const MetadataOverride& value : storedOverrides(overrides)) {
            if (value.rootPath == root)
                where.append(value.mp3RelPath);
        }
        return where;
    };
    QString error;
    QString automaticTitle;
    {
        LibraryController controller(catalogue, {}, overrides);
        QSignalSpy finished(&controller, &LibraryController::scanFinished);
        QVERIFY(controller.chooseRoot(rootA));
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 20000);
        const qint64 songA = controller.findSongByMp3Path(rootA, known);
        const qint64 otherA = controller.findSongByMp3Path(rootA, unknown);
        QVERIFY(songA > 0 && otherA > 0);
        automaticTitle = controller.songRef(songA)->title;
        QVERIFY(!automaticTitle.isEmpty());

        // Under folder A: an imported label, a name correction and a key on
        // one song; just a key on the other.
        MetadataOverride imported = controller.existingTrusted(songA);
        imported.label = QStringLiteral("Sunfly");
        imported.origin = QStringLiteral("import");
        QVERIFY2(controller.setTrustedMetadata(songA, imported, &error), qPrintable(error));
        QVERIFY2(controller.setManualOverride(songA, QStringLiteral("Someone"), QStringLiteral("Something"), &error),
                 qPrintable(error));
        QVERIFY2(controller.setManualOriginalKey(songA, 0, &error), qPrintable(error));
        QVERIFY2(controller.setManualOriginalKey(otherA, 5, &error), qPrintable(error));
        QCOMPARE(controller.songRef(songA)->title, QStringLiteral("Something"));
        QCOMPARE(shownKey(controller, songA), QStringLiteral("C"));
        QCOMPARE(storedAt(canonicalA), QStringList({known, unknown}));
        const MetadataOverride beforeMove = storedOverrides(overrides).first();

        // The same collection is chosen at folder B.
        finished.clear();
        QVERIFY(controller.chooseRoot(rootB));
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 20000);
        const qint64 songB = controller.findSongByMp3Path(rootB, known);
        const qint64 otherB = controller.findSongByMp3Path(rootB, unknown);
        QVERIFY(songB > 0 && otherB > 0 && songB != songA && otherB != otherA);
        QCOMPARE(controller.songRef(songB)->title, QStringLiteral("Something"));
        QCOMPARE(controller.songRef(songB)->artist, QStringLiteral("Someone"));
        QCOMPARE(shownKey(controller, songB), QStringLiteral("C"));
        QCOMPARE(shownKey(controller, otherB), QStringLiteral("F"));
        // The store rows moved to the songs' new identities, every value kept.
        QCOMPARE(storedAt(canonicalA), QStringList());
        QCOMPARE(storedAt(canonicalB), QStringList({known, unknown}));
        const MetadataOverride afterMove = storedOverrides(overrides).first();
        QCOMPARE(afterMove.mp3RelPath, beforeMove.mp3RelPath);
        QCOMPARE(afterMove.artist, beforeMove.artist);
        QCOMPARE(afterMove.title, beforeMove.title);
        QCOMPARE(afterMove.label, std::optional<QString>(QStringLiteral("Sunfly")));
        QCOMPARE(afterMove.series, beforeMove.series);
        QCOMPARE(afterMove.trustedDiscId, beforeMove.trustedDiscId);
        QCOMPARE(afterMove.trustedTrack, beforeMove.trustedTrack);
        QCOMPARE(afterMove.originalKey, std::optional<int>(0));
        QCOMPARE(afterMove.origin, QStringLiteral("import"));
        QCOMPARE(afterMove.createdAt, beforeMove.createdAt);
        QCOMPARE(afterMove.updatedAt, beforeMove.updatedAt);
        QCOMPARE(afterMove.autoTitle, beforeMove.autoTitle);
        QCOMPARE(afterMove.discId, beforeMove.discId);
        QCOMPARE(afterMove.track, beforeMove.track);
        QCOMPARE(controller.existingTrusted(songB).title, std::optional<QString>(QStringLiteral("Something")));

        // Edited at B: the key changes, everything else stays.
        QVERIFY2(controller.setManualOriginalKey(songB, 21, &error), qPrintable(error));
        QCOMPARE(shownKey(controller, songB), QStringLiteral("Am"));
        QCOMPARE(storedOverrides(overrides).size(), 2);
        QCOMPARE(storedOverrides(overrides).first().originalKey, std::optional<int>(21));
        QCOMPARE(storedOverrides(overrides).first().title, std::optional<QString>(QStringLiteral("Something")));
        QCOMPARE(storedOverrides(overrides).first().createdAt, beforeMove.createdAt);
        finished.clear();
        controller.requestRefreshScan();
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 20000);
        QCOMPARE(shownKey(controller, songB), QStringLiteral("Am"));
        QCOMPARE(controller.songRef(songB)->title, QStringLiteral("Something"));
        QCOMPARE(storedAt(canonicalB), QStringList({known, unknown}));

        // Back to A and to B again: the corrections follow the folder in use.
        finished.clear();
        QVERIFY(controller.chooseRoot(rootA));
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 20000);
        QCOMPARE(shownKey(controller, songA), QStringLiteral("Am"));
        QCOMPARE(controller.songRef(songA)->title, QStringLiteral("Something"));
        QCOMPARE(storedAt(canonicalA), QStringList({known, unknown}));
        finished.clear();
        QVERIFY(controller.chooseRoot(rootB));
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 20000);
        QCOMPARE(storedAt(canonicalB), QStringList({known, unknown}));

        // Cleared at B: the name and both keys go back to automatic.
        QVERIFY2(controller.clearManualOverride(songB, &error), qPrintable(error));
        QVERIFY2(controller.setManualOriginalKey(songB, std::nullopt, &error), qPrintable(error));
        QVERIFY2(controller.setManualOriginalKey(otherB, std::nullopt, &error), qPrintable(error));
        QCOMPARE(controller.songRef(songB)->title, automaticTitle);
        QCOMPARE(shownKey(controller, songB), QString());
        QCOMPARE(shownKey(controller, otherB), QString());
        // Only the imported label is left in the store.
        QCOMPARE(storedAt(canonicalB), QStringList({known}));
        QVERIFY(!storedOverrides(overrides).first().title);
        QVERIFY(!storedOverrides(overrides).first().originalKey);
        QCOMPARE(storedOverrides(overrides).first().label, std::optional<QString>(QStringLiteral("Sunfly")));

        finished.clear();
        controller.requestRefreshScan();
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 20000);
        QCOMPARE(controller.songRef(songB)->title, automaticTitle);
        QCOMPARE(shownKey(controller, songB), QString());
        QCOMPARE(shownKey(controller, otherB), QString());
    }
    // After restarting (with the start-up library check), nothing comes back.
    {
        LibraryController controller(catalogue, {}, overrides);
        QSignalSpy finished(&controller, &LibraryController::scanFinished);
        controller.startConfiguredScan();
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 20000);
        const qint64 songB = controller.findSongByMp3Path(rootB, known);
        const qint64 otherB = controller.findSongByMp3Path(rootB, unknown);
        QCOMPARE(controller.songRef(songB)->title, automaticTitle);
        QCOMPARE(shownKey(controller, songB), QString());
        QCOMPARE(shownKey(controller, otherB), QString());
        QCOMPARE(storedAt(canonicalB), QStringList({known}));
        QCOMPARE(storedAt(canonicalA), QStringList());
        QCOMPARE(rowCount(catalogue, QStringLiteral("SELECT count(*) FROM songs WHERE manual_title IS NOT NULL "
                                                    "OR manual_artist IS NOT NULL OR manual_original_key IS NOT NULL")),
                 0LL);
    }
    QCOMPARE(snapshot(rootA), beforeA);  // nothing written to either folder
    QCOMPARE(snapshot(rootB), beforeB);
}

void TestSongKeys::anOldFolderCopyNeverUndoesAClear()
{
    // Before corrections followed a moved folder, a correction made at the
    // old folder (A) stayed there, and the user may have entered it again at
    // the new one (B). The song's own correction wins, the old copy is
    // dropped, and clearing at B stays cleared.
    QTemporaryDir temporary;
    const QString rootA = temporary.filePath(QStringLiteral("music-a"));
    const QString rootB = temporary.filePath(QStringLiteral("music-b"));
    const auto [known, unknown] = writeManualKeyLibrary(m_fixtures, rootA);
    QVERIFY(!known.isEmpty());
    QVERIFY(!writeManualKeyLibrary(m_fixtures, rootB).first.isEmpty());
    const QString appDir = temporary.filePath(QStringLiteral("app"));
    const QString catalogue = appDir + QStringLiteral("/library.sqlite");
    const QString overrides = appDir + QStringLiteral("/metadata-overrides.sqlite");
    QString error;
    QString automaticTitle;
    {
        LibraryController controller(catalogue, {}, overrides);
        QSignalSpy finished(&controller, &LibraryController::scanFinished);
        QVERIFY(controller.chooseRoot(rootA));
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 20000);
        const qint64 songA = controller.findSongByMp3Path(rootA, known);
        finished.clear();
        QVERIFY(controller.chooseRoot(rootB));
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 20000);
        const qint64 songB = controller.findSongByMp3Path(rootB, known);
        QVERIFY(songA > 0 && songB > 0 && songA != songB);
        automaticTitle = controller.songRef(songB)->title;

        // As builds before this fix left them: the old folder's row (saved
        // later than B's, to show that age does not matter) and the song's
        // own row at B, entered without the old one's values.
        {
            QMutexLocker lock(&MetadataOverrideStore::synchronisation());
            MetadataOverrideStore store(overrides);
            QVERIFY2(store.open(&error), qPrintable(error));
            const SongRef automatic = *controller.songRef(songA);
            MetadataOverride old;
            old.rootPath = rootA;
            old.mp3RelPath = known;
            old.discId = automatic.discId;
            old.track = automatic.track;
            old.autoTitle = automatic.title;
            old.autoArtist = automatic.artist;
            old.title = QStringLiteral("Old Name");
            old.originalKey = 0;
            old.updatedAt = QDateTime::currentMSecsSinceEpoch() + 60000;
            QVERIFY2(store.setOverride(old, &error), qPrintable(error));
            MetadataOverride own = old;
            own.rootPath = rootB;
            own.artist = QStringLiteral("Someone");
            own.title = QStringLiteral("New Name");
            own.originalKey.reset();
            own.updatedAt = QDateTime::currentMSecsSinceEpoch();
            QVERIFY2(store.setOverride(own, &error), qPrintable(error));
        }
        QCOMPARE(storedOverrides(overrides).size(), 2);

        finished.clear();
        controller.requestRefreshScan();
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 20000);
        QCOMPARE(controller.songRef(songB)->title, QStringLiteral("New Name"));
        QCOMPARE(shownKey(controller, songB), QString());
        QCOMPARE(storedOverrides(overrides).size(), 1);
        QCOMPARE(storedOverrides(overrides).first().rootPath, Catalogue::canonicalPath(rootB));

        QVERIFY2(controller.clearManualOverride(songB, &error), qPrintable(error));
        QCOMPARE(controller.songRef(songB)->title, automaticTitle);
        QVERIFY(storedOverrides(overrides).isEmpty());
        finished.clear();
        controller.requestRefreshScan();
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 20000);
        QCOMPARE(controller.songRef(songB)->title, automaticTitle);
        QCOMPARE(shownKey(controller, songB), QString());
    }
    {
        LibraryController controller(catalogue, {}, overrides);
        QSignalSpy finished(&controller, &LibraryController::scanFinished);
        controller.startConfiguredScan();
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 20000);
        const qint64 songB = controller.findSongByMp3Path(rootB, known);
        QCOMPARE(controller.songRef(songB)->title, automaticTitle);
        QCOMPARE(shownKey(controller, songB), QString());
        QVERIFY(storedOverrides(overrides).isEmpty());
    }
}

void TestSongKeys::aCopyLeftByAFailedTidyUpNeverUndoesAClear()
{
    // The correction is copied to the new folder's song, but removing the
    // old folder's row fails (forced here with a trigger in the test's own
    // store). No change or clear at the new folder may be undone by that
    // copy: not by a rescan, a restart, or choosing the old folder again.
    QTemporaryDir temporary;
    const QString rootA = temporary.filePath(QStringLiteral("music-a"));
    const QString rootB = temporary.filePath(QStringLiteral("music-b"));
    const auto [known, unknown] = writeManualKeyLibrary(m_fixtures, rootA);
    QVERIFY(!known.isEmpty());
    QVERIFY(!writeManualKeyLibrary(m_fixtures, rootB).first.isEmpty());
    const QString appDir = temporary.filePath(QStringLiteral("app"));
    const QString catalogue = appDir + QStringLiteral("/library.sqlite");
    const QString overrides = appDir + QStringLiteral("/metadata-overrides.sqlite");
    const QString canonicalA = Catalogue::canonicalPath(rootA);
    const QString canonicalB = Catalogue::canonicalPath(rootB);
    const QMap<QString, QPair<qint64, qint64>> beforeA = snapshot(rootA);
    const QMap<QString, QPair<qint64, qint64>> beforeB = snapshot(rootB);
    auto storedAt = [&](const QString& root) {
        QStringList where;
        for (const MetadataOverride& value : storedOverrides(overrides)) {
            if (value.rootPath == root)
                where.append(value.mp3RelPath);
        }
        return where;
    };
    auto onStore = [&](const QString& sql) {
        const QString name = QUuid::createUuid().toString(QUuid::WithoutBraces);
        bool ok = false;
        {
            QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
            database.setDatabaseName(overrides);
            if (database.open()) {
                QSqlQuery query(database);
                ok = query.exec(sql);
            }
            database.close();
        }
        QSqlDatabase::removeDatabase(name);
        return ok;
    };
    QString error;
    QString automaticTitle;
    {
        LibraryController controller(catalogue, {}, overrides);
        QSignalSpy finished(&controller, &LibraryController::scanFinished);
        QVERIFY(controller.chooseRoot(rootA));
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 20000);
        const qint64 songA = controller.findSongByMp3Path(rootA, known);
        const qint64 otherA = controller.findSongByMp3Path(rootA, unknown);
        automaticTitle = controller.songRef(songA)->title;
        QVERIFY2(controller.setManualOverride(songA, QStringLiteral("Someone"), QStringLiteral("Something"), &error),
                 qPrintable(error));
        QVERIFY2(controller.setManualOriginalKey(songA, 0, &error), qPrintable(error));
        QVERIFY2(controller.setManualOriginalKey(otherA, 5, &error), qPrintable(error));

        // Rows at folder A can no longer be removed.
        QVERIFY(onStore(QStringLiteral(
            "CREATE TRIGGER test_keep_old BEFORE DELETE ON metadata_overrides "
            "WHEN old.root_path='%1' BEGIN SELECT RAISE(ABORT,'forced failure'); END")
                            .arg(canonicalA)));
        finished.clear();
        QVERIFY(controller.chooseRoot(rootB));
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 20000);
        const qint64 songB = controller.findSongByMp3Path(rootB, known);
        const qint64 otherB = controller.findSongByMp3Path(rootB, unknown);
        QCOMPARE(controller.songRef(songB)->title, QStringLiteral("Something"));
        QCOMPARE(shownKey(controller, songB), QStringLiteral("C"));
        QCOMPARE(shownKey(controller, otherB), QStringLiteral("F"));
        // Copied to B, but the old rows are still there.
        QCOMPARE(storedAt(canonicalB), QStringList({known, unknown}));
        QCOMPARE(storedAt(canonicalA), QStringList({known, unknown}));

        // While the old copies cannot be removed, every change at B is
        // refused and changes nothing (the old copies would otherwise come
        // back, or win when A is chosen again).
        QVERIFY(!controller.clearManualOverride(songB, &error));
        QVERIFY(!controller.setManualOriginalKey(songB, std::nullopt, &error));
        QVERIFY(!controller.setManualOriginalKey(songB, 7, &error));
        QCOMPARE(controller.songRef(songB)->title, QStringLiteral("Something"));
        QCOMPARE(shownKey(controller, songB), QStringLiteral("C"));
        finished.clear();
        controller.requestRefreshScan();
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 20000);
        QCOMPARE(controller.songRef(songB)->title, QStringLiteral("Something"));
        QCOMPARE(shownKey(controller, songB), QStringLiteral("C"));
        // Choosing A again, the old rows are the same values: nothing is lost.
        finished.clear();
        QVERIFY(controller.chooseRoot(rootA));
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 20000);
        QCOMPARE(controller.songRef(songA)->title, QStringLiteral("Something"));
        QCOMPARE(shownKey(controller, songA), QStringLiteral("C"));
        QCOMPARE(storedAt(canonicalA), QStringList({known, unknown}));
        QCOMPARE(storedAt(canonicalB), QStringList());
        finished.clear();
        QVERIFY(controller.chooseRoot(rootB));
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 20000);
        QCOMPARE(storedAt(canonicalA), QStringList({known, unknown}));
        QCOMPARE(storedAt(canonicalB), QStringList({known, unknown}));

        // The failure passes, with the old copies still stored. A partial
        // clear at B takes the song's old copy with it, so choosing A again
        // keeps B's newer values: the name stays automatic, the key stays.
        QVERIFY(onStore(QStringLiteral("DROP TRIGGER test_keep_old")));
        QVERIFY2(controller.clearManualOverride(songB, &error), qPrintable(error));
        QCOMPARE(controller.songRef(songB)->title, automaticTitle);
        QCOMPARE(shownKey(controller, songB), QStringLiteral("C"));
        QCOMPARE(storedAt(canonicalA), QStringList({unknown}));
        finished.clear();
        QVERIFY(controller.chooseRoot(rootA));
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 20000);
        QCOMPARE(controller.songRef(songA)->title, automaticTitle);
        QCOMPARE(shownKey(controller, songA), QStringLiteral("C"));
        QCOMPARE(shownKey(controller, otherA), QStringLiteral("F"));
        finished.clear();
        QVERIFY(controller.chooseRoot(rootB));
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 20000);
        QCOMPARE(storedAt(canonicalA), QStringList());
        QCOMPARE(storedAt(canonicalB), QStringList({known, unknown}));

        // Cleared at B for good.
        QVERIFY2(controller.setManualOriginalKey(songB, std::nullopt, &error), qPrintable(error));
        QVERIFY2(controller.setManualOriginalKey(otherB, std::nullopt, &error), qPrintable(error));
        QCOMPARE(shownKey(controller, songB), QString());
        QCOMPARE(shownKey(controller, otherB), QString());
        QVERIFY(storedOverrides(overrides).isEmpty());

        finished.clear();
        controller.requestRefreshScan();
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 20000);
        QCOMPARE(controller.songRef(songB)->title, automaticTitle);
        QCOMPARE(shownKey(controller, songB), QString());
        QCOMPARE(shownKey(controller, otherB), QString());
    }
    {
        LibraryController controller(catalogue, {}, overrides);
        QSignalSpy finished(&controller, &LibraryController::scanFinished);
        controller.startConfiguredScan();
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 20000);
        const qint64 songB = controller.findSongByMp3Path(rootB, known);
        const qint64 otherB = controller.findSongByMp3Path(rootB, unknown);
        QCOMPARE(controller.songRef(songB)->title, automaticTitle);
        QCOMPARE(shownKey(controller, songB), QString());
        QCOMPARE(shownKey(controller, otherB), QString());
        QVERIFY(storedOverrides(overrides).isEmpty());
        QCOMPARE(rowCount(catalogue, QStringLiteral("SELECT count(*) FROM songs WHERE manual_title IS NOT NULL "
                                                    "OR manual_artist IS NOT NULL OR manual_original_key IS NOT NULL")),
                 0LL);
    }
    QCOMPARE(snapshot(rootA), beforeA);
    QCOMPARE(snapshot(rootB), beforeB);
}

void TestSongKeys::anEditBeforeTheSyncKeepsTheOtherValues()
{
    // A folder already in the catalogue is chosen again: its songs show at
    // once, before the library check moves their corrections over. A key set
    // in that moment keeps the name and label saved at the other folder.
    QTemporaryDir temporary;
    const QString rootA = temporary.filePath(QStringLiteral("music-a"));
    const QString rootB = temporary.filePath(QStringLiteral("music-b"));
    const auto [known, unknown] = writeManualKeyLibrary(m_fixtures, rootA);
    QVERIFY(!known.isEmpty());
    QVERIFY(!writeManualKeyLibrary(m_fixtures, rootB).first.isEmpty());
    const QString appDir = temporary.filePath(QStringLiteral("app"));
    const QString catalogue = appDir + QStringLiteral("/library.sqlite");
    const QString overrides = appDir + QStringLiteral("/metadata-overrides.sqlite");
    QString error;
    qint64 createdAt = 0;
    {
        LibraryController controller(catalogue, {}, overrides);
        QSignalSpy finished(&controller, &LibraryController::scanFinished);
        for (const QString& root : {rootA, rootB, rootA}) {
            finished.clear();
            QVERIFY(controller.chooseRoot(root));
            QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 20000);
        }
        const qint64 songA = controller.findSongByMp3Path(rootA, known);
        MetadataOverride imported = controller.existingTrusted(songA);
        imported.label = QStringLiteral("Sunfly");
        imported.origin = QStringLiteral("import");
        QVERIFY2(controller.setTrustedMetadata(songA, imported, &error), qPrintable(error));
        QVERIFY2(controller.setManualOverride(songA, QStringLiteral("Someone"), QStringLiteral("Something"), &error),
                 qPrintable(error));
        QVERIFY2(controller.setManualOriginalKey(songA, 0, &error), qPrintable(error));
        createdAt = storedOverrides(overrides).first().createdAt;

        // Playback holds the library check before it reaches the sync.
        controller.setPlaybackActive(true);
        finished.clear();
        QVERIFY(controller.chooseRoot(rootB));
        const qint64 songB = controller.findSongByMp3Path(rootB, known);
        QVERIFY(songB > 0);
        QCOMPARE(storedOverrides(overrides).first().rootPath, Catalogue::canonicalPath(rootA));
        QCOMPARE(controller.existingTrusted(songB).title, std::optional<QString>(QStringLiteral("Something")));
        QVERIFY2(controller.setManualOriginalKey(songB, 21, &error), qPrintable(error));
        QCOMPARE(finished.count(), 0);
        controller.setPlaybackActive(false);
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 20000);

        QCOMPARE(shownKey(controller, songB), QStringLiteral("Am"));
        QCOMPARE(controller.songRef(songB)->title, QStringLiteral("Something"));
        const QList<MetadataOverride> stored = storedOverrides(overrides);
        QCOMPARE(stored.size(), 1);
        QCOMPARE(stored.first().rootPath, Catalogue::canonicalPath(rootB));
        QCOMPARE(stored.first().title, std::optional<QString>(QStringLiteral("Something")));
        QCOMPARE(stored.first().artist, std::optional<QString>(QStringLiteral("Someone")));
        QCOMPARE(stored.first().label, std::optional<QString>(QStringLiteral("Sunfly")));
        QCOMPARE(stored.first().origin, QStringLiteral("import"));
        QCOMPARE(stored.first().originalKey, std::optional<int>(21));
        QCOMPARE(stored.first().createdAt, createdAt);
    }
    {
        LibraryController controller(catalogue, {}, overrides);
        QSignalSpy finished(&controller, &LibraryController::scanFinished);
        controller.startConfiguredScan();
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 20000);
        const qint64 songB = controller.findSongByMp3Path(rootB, known);
        QCOMPARE(shownKey(controller, songB), QStringLiteral("Am"));
        QCOMPARE(controller.songRef(songB)->title, QStringLiteral("Something"));
        QCOMPARE(storedOverrides(overrides).size(), 1);
        QCOMPARE(storedOverrides(overrides).first().label, std::optional<QString>(QStringLiteral("Sunfly")));
    }
}

void TestSongKeys::setSongKeyPopupChoosesAndClears()
{
    QTemporaryDir temporary;
    const QString root = temporary.filePath(QStringLiteral("music"));
    const auto [known, unknown] = writeManualKeyLibrary(m_fixtures, root);
    QVERIFY(!known.isEmpty());
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")), {},
                                 temporary.filePath(QStringLiteral("app/metadata-overrides.sqlite")));
    controller.setSongKeyTimings(quickTimings());
    QSignalSpy finished(&controller, &LibraryController::scanFinished);
    QVERIFY(controller.chooseRoot(root));
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 20000);
    controller.setSongKeyAnalysisEnabled(true);
    QTRY_VERIFY_WITH_TIMEOUT(analysedAll(controller, 2), 60000);
    controller.setSongKeyAnalysisEnabled(false);
    const qint64 song = controller.findSongByMp3Path(root, known);

    LibraryView view(&controller);
    view.resize(1100, 600);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    view.searchBox()->setText(QStringLiteral("Synth Band"));
    QTRY_COMPARE_WITH_TIMEOUT(view.songResultCount(), 2, 3000);
    // Nothing selected: nothing to set.
    view.resultsList()->setCurrentIndex(QModelIndex());
    QVERIFY(!view.setKeyButton()->isEnabled());
    QVERIFY(!view.openSongKeyPicker());
    QVERIFY(openPickers().isEmpty());

    const int row = resultRow(&view, QStringLiteral("G Song"));
    QVERIFY(row >= 0);
    QTreeView* results = view.resultsList();
    QTest::mouseClick(results->viewport(), Qt::LeftButton, {},
                      results->visualRect(results->model()->index(row, LibraryResultsModel::ArtistColumn)).center());
    QCOMPARE(view.selectedSongId(), song);
    QVERIFY(view.setKeyButton()->isEnabled());
    const auto keyCell = [&] {
        return results->model()->index(row, LibraryResultsModel::KeyColumn).data().toString();
    };
    QCOMPARE(keyCell(), QStringLiteral("G"));

    // The popup: 24 keys with the program's spellings, the detected one marked
    // quietly, none chosen yet.
    QTest::mouseClick(view.setKeyButton(), Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(openPickers().size(), 1, 2000);
    QPointer<SongKeyPicker> picker = openPickers().first();
    QVERIFY(picker->findChild<QLabel*>(QStringLiteral("keyPickerTitle")));
    QCOMPARE(picker->findChild<QLabel*>(QStringLiteral("keyPickerTitle"))->text(),
             QStringLiteral("Set Original Song Key"));
    const QStringList spellings = {
        QStringLiteral("C"), QStringLiteral("Db"), QStringLiteral("D"), QStringLiteral("Eb"),
        QStringLiteral("E"), QStringLiteral("F"), QStringLiteral("F#"), QStringLiteral("G"),
        QStringLiteral("Ab"), QStringLiteral("A"), QStringLiteral("Bb"), QStringLiteral("B"),
        QStringLiteral("Cm"), QStringLiteral("C#m"), QStringLiteral("Dm"), QStringLiteral("Ebm"),
        QStringLiteral("Em"), QStringLiteral("Fm"), QStringLiteral("F#m"), QStringLiteral("Gm"),
        QStringLiteral("G#m"), QStringLiteral("Am"), QStringLiteral("Bbm"), QStringLiteral("Bm")};
    for (int index = 0; index < 24; ++index) {
        QCOMPARE(picker->keyButton(index)->text(), spellings.at(index));
        QVERIFY(!picker->keyButton(index)->isChecked());
        QCOMPARE(picker->keyButton(index)->property("detected").toBool(), index == 7);
    }
    QVERIFY(!picker->keyButton(24));
    QVERIFY(picker->detectedLabel()->text().contains(QStringLiteral("G")));
    QVERIFY(!picker->clearButton()->isEnabled());

    // Escape closes it and changes nothing.
    QTest::keyClick(picker, Qt::Key_Escape);
    QTRY_VERIFY_WITH_TIMEOUT(!picker, 2000);
    QVERIFY(!controller.songKeyDetails(song)->isManual());

    // One click chooses, saves and closes; the Key cell shows it at once.
    picker = view.openSongKeyPicker();
    QVERIFY(picker);
    QTest::mouseClick(picker->keyButton(0), Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!picker, 2000);
    QCOMPARE(controller.songKeyDetails(song)->manualKeyIndex, 0);
    QCOMPARE(keyCell(), QStringLiteral("C"));
    QCOMPARE(view.selectedSongId(), song);  // still on the same song

    // Opened again: C is the chosen one; Clear Manual Key goes back to G.
    picker = view.openSongKeyPicker();
    QVERIFY(picker);
    QVERIFY(picker->keyButton(0)->isChecked());
    QVERIFY(!picker->keyButton(7)->isChecked());
    QVERIFY(!picker->keyButton(7)->property("detected").toBool());
    QVERIFY(picker->detectedLabel()->text().contains(QStringLiteral("G")));
    QVERIFY(picker->clearButton()->isEnabled());
    QTest::mouseClick(picker->clearButton(), Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!picker, 2000);
    QVERIFY(!controller.songKeyDetails(song)->isManual());
    QCOMPARE(keyCell(), QStringLiteral("G"));
}

void TestSongKeys::keyCellClickIsLikeTheRestOfTheRow()
{
    QTemporaryDir temporary;
    const QString root = temporary.filePath(QStringLiteral("music"));
    const auto [known, unknown] = writeManualKeyLibrary(m_fixtures, root);
    QVERIFY(!known.isEmpty());
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")), {},
                                 temporary.filePath(QStringLiteral("app/metadata-overrides.sqlite")));
    QSignalSpy finished(&controller, &LibraryController::scanFinished);
    QVERIFY(controller.chooseRoot(root));
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 20000);
    const qint64 song = controller.findSongByMp3Path(root, known);
    QString error;
    QVERIFY2(controller.setManualOriginalKey(song, 0, &error), qPrintable(error));

    LibraryView view(&controller);
    view.setKeyColumnAvailable(true);
    view.resize(1100, 600);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    view.searchBox()->setText(QStringLiteral("Synth Band"));
    QTRY_COMPARE_WITH_TIMEOUT(view.songResultCount(), 2, 3000);
    QTreeView* results = view.resultsList();
    QVERIFY(!results->isColumnHidden(LibraryResultsModel::KeyColumn));
    QSignalSpy sing(&view, &LibraryView::singRequested);
    const auto cell = [&](int row, int column) {
        return results->visualRect(results->model()->index(row, column)).center();
    };
    // The Key cell is display only.
    for (int row = 0; row < 2; ++row)
        QVERIFY(!(results->model()->flags(results->model()->index(row, LibraryResultsModel::KeyColumn))
                  & Qt::ItemIsEditable));

    // A click on any part of a row, the Key cell included, just selects it.
    for (const int column : {int(LibraryResultsModel::KeyColumn), int(LibraryResultsModel::ArtistColumn),
                             int(LibraryResultsModel::SongColumn), int(LibraryResultsModel::DiscColumn)}) {
        for (int row = 0; row < 2; ++row) {
            QTest::mouseClick(results->viewport(), Qt::LeftButton, {}, cell(row, column));
            QCOMPARE(results->currentIndex().row(), row);
            QVERIFY(results->selectionModel()->isRowSelected(row, {}));
            // Not editing: no editor opened in the table.
            QVERIFY(results->viewport()->findChildren<QLineEdit*>().isEmpty());
            QVERIFY(!results->isPersistentEditorOpen(results->model()->index(row, column)));
            QVERIFY(openPickers().isEmpty());
            QCOMPARE(sing.count(), 0);
        }
    }
    QCOMPARE(controller.songKeyDetails(song)->manualKeyIndex, 0);  // nothing changed
    QCOMPARE(results->model()->index(resultRow(&view, QStringLiteral("G Song")),
                                     LibraryResultsModel::KeyColumn).data().toString(), QStringLiteral("C"));

    // A double-click on the Key cell sings the song, as anywhere in the row.
    const int row = resultRow(&view, QStringLiteral("G Song"));
    // (A double-click arrives as a click, then the double-click.)
    const auto doubleClick = [&](int column) {
        QTest::mouseClick(results->viewport(), Qt::LeftButton, {}, cell(row, column));
        QTest::mouseDClick(results->viewport(), Qt::LeftButton, {}, cell(row, column));
    };
    doubleClick(LibraryResultsModel::KeyColumn);
    QTRY_COMPARE_WITH_TIMEOUT(sing.count(), 1, 2000);
    QCOMPARE(sing.first().first().toLongLong(), song);
    QTest::qWait(QApplication::doubleClickInterval() + 50);
    doubleClick(LibraryResultsModel::SongColumn);
    QTRY_COMPARE_WITH_TIMEOUT(sing.count(), 2, 2000);
    QCOMPARE(sing.last().first().toLongLong(), song);
    QVERIFY(openPickers().isEmpty());
    QVERIFY(results->viewport()->findChildren<QLineEdit*>().isEmpty());
}

void TestSongKeys::setSongKeyFitsTheLibraryFooter()
{
    // J: the button must not make the window larger or squeeze the song table.
    theme::apply(*qApp);
    QTemporaryDir temporary;
    const QString root = temporary.filePath(QStringLiteral("music"));
    const auto [known, unknown] = writeManualKeyLibrary(m_fixtures, root);
    QVERIFY(!known.isEmpty());
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")), {},
                                 temporary.filePath(QStringLiteral("app/metadata-overrides.sqlite")));
    QSignalSpy finished(&controller, &LibraryController::scanFinished);
    QVERIFY(controller.chooseRoot(root));
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 20000);
    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings, &controller);
    window.setShowErrorDialogs(false);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    LibraryView* view = window.libraryView();
    view->searchBox()->setText(QStringLiteral("Synth Band"));
    QTRY_COMPARE_WITH_TIMEOUT(view->songResultCount(), 2, 3000);
    view->resultsList()->setCurrentIndex(view->resultsList()->model()->index(0, 0));
    QVERIFY(view->setKeyButton()->isEnabled());
    QHeaderView* header = view->resultsList()->header();
    for (const int percent : {80, 90, 100, 125, 150}) {
        theme::setScalePercent(percent);
        QCoreApplication::processEvents();
        const QSize with = window.minimumSizeHint();
        view->setKeyButton()->hide();
        const QSize without = window.minimumSizeHint();
        view->setKeyButton()->show();
        QCOMPARE(with, without);
        // A 1366x768 screen (at most; the window's own minimum otherwise).
        window.resize(QSize(1366, 705).expandedTo(with));
        QCoreApplication::processEvents();
        const int artist = header->sectionSize(LibraryResultsModel::ArtistColumn);
        view->setKeyButton()->hide();
        QCoreApplication::processEvents();
        QCOMPARE(header->sectionSize(LibraryResultsModel::ArtistColumn), artist);
        view->setKeyButton()->show();
        QCoreApplication::processEvents();
        // Every footer button is shown whole.
        for (QPushButton* button : {view->setKeyButton(), view->addToPlaylistButton(), view->singButton()}) {
            QVERIFY2(button->isVisible() && button->width() >= button->minimumSizeHint().width(),
                     qPrintable(QStringLiteral("%1 at %2%").arg(button->text()).arg(percent)));
        }
        QPointer<SongKeyPicker> picker = view->openSongKeyPicker();
        QVERIFY(picker);
        QVERIFY(picker->width() <= 1366 && picker->height() <= 705);
        picker->close();
        QTRY_VERIFY_WITH_TIMEOUT(!picker, 2000);
    }
    theme::setScalePercent(100);
}

QTEST_MAIN(TestSongKeys)
#include "tst_songkeys.moc"
