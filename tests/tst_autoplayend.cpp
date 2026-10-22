// Smart Autoplay End ("Skip trailing silence during Autoplay"): where the
// music of a recording ends (synthetic sound of every awkward kind), the
// GStreamer decoding of synthetic MP3s, the enrichment cache across restarts,
// and the playlist lifecycle: only Autoplay ever moves on early, only after
// the music, and exactly as at a natural end. No real karaoke media is used.

#include "AppPreferences.h"
#include "AudibleEnds.h"
#include "LibraryController.h"
#include "LibraryView.h"
#include "MainWindow.h"
#include "PlaylistView.h"
#include "SettingsDialog.h"
#include "SmartAutoplayEnd.h"
#include "SongPair.h"
#include "SongSettings.h"
#include "library/ContentIdentity.h"
#include "music/AudibleEnd.h"
#include "playlist/PlaylistPlayback.h"
#include "playlist/PlaylistStore.h"

#include "TestMedia.h"

#include <QApplication>
#include <QCheckBox>
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTreeView>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

#include <gst/gst.h>

#include <atomic>
#include <cmath>
#include <random>
#include <vector>

using music::AudibleEnd;
using music::AudibleEndDetector;

namespace {

constexpr int kRate = music::kAudibleEndSampleRate;
constexpr double kPi = 3.14159265358979323846;

double fromDb(double db)
{
    return std::pow(10.0, db / 20.0);
}

// Stereo sound built a section at a time.
class Sound {
public:
    // A chord with a slow swell, at about the given RMS level (dBFS).
    Sound& music(double seconds, double levelDb = -14.0)
    {
        const double amplitude = fromDb(levelDb) * std::sqrt(2.0) / 1.7;
        for (std::int64_t i = 0, n = frames(seconds); i < n; ++i, ++m_t) {
            const double t = double(m_t) / kRate;
            const double swell = 0.85 + 0.15 * std::sin(2 * kPi * 0.5 * t);
            const double v = amplitude * swell
                * (std::sin(2 * kPi * 220.0 * t) + 0.5 * std::sin(2 * kPi * 277.2 * t)
                   + 0.4 * std::sin(2 * kPi * 329.6 * t));
            push(v, v * 0.9);
        }
        return *this;
    }
    Sound& silence(double seconds)
    {
        for (std::int64_t i = 0, n = frames(seconds); i < n; ++i, ++m_t)
            push(0.0, 0.0);
        return *this;
    }
    // A steady tone (one channel, or both) at an RMS level.
    Sound& tone(double seconds, double levelDb, bool leftToo = true)
    {
        const double amplitude = fromDb(levelDb) * std::sqrt(2.0);
        for (std::int64_t i = 0, n = frames(seconds); i < n; ++i, ++m_t) {
            const double v = amplitude * std::sin(2 * kPi * 440.0 * double(m_t) / kRate);
            push(leftToo ? v : 0.0, v);
        }
        return *this;
    }
    // Music fading out, in dB per second, until it is below -100 dBFS.
    Sound& fade(double seconds, double fromDbLevel = -14.0)
    {
        const double perSecond = (fromDbLevel + 100.0) / seconds;
        const double amplitude = std::sqrt(2.0) / 1.7;
        for (std::int64_t i = 0, n = frames(seconds); i < n; ++i, ++m_t) {
            const double t = double(m_t) / kRate;
            const double level = fromDb(fromDbLevel - perSecond * (double(i) / kRate));
            const double v = amplitude * level
                * (std::sin(2 * kPi * 220.0 * t) + 0.5 * std::sin(2 * kPi * 277.2 * t));
            push(v, v);
        }
        return *this;
    }
    // Words: bursts of noise with short pauses between them.
    Sound& speech(double seconds, double levelDb)
    {
        std::mt19937 random(7);
        std::normal_distribution<double> noise(0.0, fromDb(levelDb));
        for (std::int64_t i = 0, n = frames(seconds); i < n; ++i, ++m_t) {
            const bool word = (i % frames(0.7)) < frames(0.3);
            const double v = word ? noise(random) : 0.0;
            push(v, v);
        }
        return *this;
    }
    // Steady hiss (the noise floor of an old recording).
    Sound& hiss(double seconds, double levelDb)
    {
        std::mt19937 random(11);
        std::normal_distribution<double> noise(0.0, fromDb(levelDb));
        for (std::int64_t i = 0, n = frames(seconds); i < n; ++i, ++m_t)
            push(noise(random), noise(random));
        return *this;
    }
    // One loud click (a few samples).
    Sound& click()
    {
        for (int i = 0; i < 40; ++i, ++m_t)
            push(i < 20 ? 0.3 : -0.3, 0.0);
        return *this;
    }

    const std::vector<float>& samples() const { return m_samples; }
    AudibleEnd analyse() const
    {
        AudibleEndDetector detector;
        // In uneven pieces, as a decoder hands them over.
        std::size_t done = 0;
        const std::size_t total = m_samples.size() / 2;
        std::size_t piece = 1000;
        while (done < total) {
            const std::size_t count = std::min(piece, total - done);
            detector.addSamples(m_samples.data() + done * 2, count);
            done += count;
            piece = piece * 7 % 4093 + 100;
        }
        return detector.finish();
    }

private:
    static std::int64_t frames(double seconds) { return std::int64_t(std::llround(seconds * kRate)); }
    void push(double left, double right)
    {
        m_samples.push_back(float(left));
        m_samples.push_back(float(right));
    }

    std::vector<float> m_samples;
    std::int64_t m_t = 0;
};

QString describe(const AudibleEnd& end)
{
    return QStringLiteral("%1 length %2 audible end %3 end at %4 ref %5 silence %6 (%7)")
        .arg(end.skips() ? QStringLiteral("SKIP") : QStringLiteral("PLAY"))
        .arg(end.durationMs).arg(end.audibleEndMs).arg(end.endAtMs)
        .arg(end.referenceDb, 0, 'f', 1).arg(end.silenceDb, 0, 'f', 1)
        .arg(QString::fromStdString(end.reason));
}

// A 16-bit stereo WAV of the sound.
bool writeWav(const QString& path, const std::vector<float>& samples)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    QDataStream out(&file);
    out.setByteOrder(QDataStream::LittleEndian);
    const quint32 dataBytes = quint32(samples.size() * 2);
    out.writeRawData("RIFF", 4);
    out << quint32(36 + dataBytes);
    out.writeRawData("WAVEfmt ", 8);
    out << quint32(16) << quint16(1) << quint16(2) << quint32(kRate) << quint32(kRate * 4)
        << quint16(4) << quint16(16);
    out.writeRawData("data", 4);
    out << dataBytes;
    for (const float sample : samples)
        out << qint16(std::lround(std::clamp(sample, -1.0f, 1.0f) * 32767.0f));
    return out.status() == QDataStream::Ok;
}

// Encodes the sound as an MP3 (constant bitrate, or variable).
bool writeMp3(const QString& path, const Sound& sound, bool variableBitrate = false)
{
    const QString wav = path + QStringLiteral(".wav");
    if (!writeWav(wav, sound.samples()))
        return false;
    GError* error = nullptr;
    const QByteArray description = QStringLiteral(
        "filesrc name=in ! wavparse ! audioconvert ! lamemp3enc %1 ! filesink name=out")
        .arg(variableBitrate ? QStringLiteral("target=quality quality=4")
                             : QStringLiteral("target=bitrate bitrate=128 cbr=true"))
        .toUtf8();
    GstElement* pipeline = gst_parse_launch(description.constData(), &error);
    if (!pipeline) {
        g_clear_error(&error);
        return false;
    }
    GstElement* in = gst_bin_get_by_name(GST_BIN(pipeline), "in");
    GstElement* out = gst_bin_get_by_name(GST_BIN(pipeline), "out");
#ifdef Q_OS_WIN
    const auto location = [](const QString& file) { return file.toUtf8(); };  // GLib file names are UTF-8 on Windows
#else
    const auto location = [](const QString& file) { return QFile::encodeName(file); };
#endif
    g_object_set(in, "location", location(wav).constData(), nullptr);
    g_object_set(out, "location", location(path).constData(), nullptr);
    gst_object_unref(in);
    gst_object_unref(out);
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    GstBus* bus = gst_element_get_bus(pipeline);
    GstMessage* message = gst_bus_timed_pop_filtered(
        bus, 60 * GST_SECOND, GstMessageType(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    const bool ok = message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS;
    if (message)
        gst_message_unref(message);
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    QFile::remove(wav);
    return ok;
}

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

// Counts the songs it is asked to analyse; the work is done by another engine.
class CountingEngine final : public AudibleEndEngine {
public:
    explicit CountingEngine(std::shared_ptr<AudibleEndEngine> inner) : m_inner(std::move(inner)) {}
    std::atomic_int calls = 0;
    Outcome analyse(const QString& path, const std::function<bool()>& stop,
                    AudibleEnd* result, QString* detail) override
    {
        ++calls;
        return m_inner->analyse(path, stop, result, detail);
    }

private:
    std::shared_ptr<AudibleEndEngine> m_inner;
};

// Gives a fixed answer.
class FixedEngine final : public AudibleEndEngine {
public:
    Outcome outcome = Outcome::Analysed;
    AudibleEnd end;
    std::atomic_int calls = 0;
    Outcome analyse(const QString&, const std::function<bool()>&, AudibleEnd* result,
                    QString* detail) override
    {
        ++calls;
        *result = end;
        *detail = QStringLiteral("fixed");
        return outcome;
    }
};

// Plays for real (silently), but reports the position `ahead` further on:
// the end of a long song is reached in a moment.
class AheadPlayer : public KaraokePlayer {
public:
    AheadPlayer() : KaraokePlayer(nullptr, QStringLiteral("fakesink")) {}
    std::atomic<qint64> ahead = 0;
    // Where the song really is.
    qint64 realPositionMs() const
    {
        qint64 ms = 0;
        return KaraokePlayer::queryAudioPosition(ms) ? ms : 0;
    }

protected:
    bool queryAudioPosition(qint64& positionMs) const override
    {
        if (!KaraokePlayer::queryAudioPosition(positionMs))
            return false;
        positionMs += ahead.load();
        return true;
    }
};

struct Library {
    QString root;
    QString endsInSilence;   // 40 s: music to 30 s, then silence
    QString short1;          // 3 s of music
    QString noSilence;       // 40 s of music
    qint64 silenceId = 0;
    qint64 shortId = 0;
    qint64 noSilenceId = 0;
};

bool writeSongPair(const QString& base, const Sound& sound, int lengthMs)
{
    return writeMp3(base + QStringLiteral(".mp3"), sound)
        && testmedia::writeCdg(base + QStringLiteral(".cdg"),
                               testmedia::markerCdg(lengthMs, qMax(50, lengthMs / 3)));
}

Library prepareLibrary(const QString& root, LibraryController& controller)
{
    Library library;
    library.root = root;
    const QString silence = root + QStringLiteral("/AE001-01 - Test Singer - Ends In Silence");
    const QString shortSong = root + QStringLiteral("/AE001-02 - Test Singer - Short Song");
    const QString noSilence = root + QStringLiteral("/AE001-03 - Test Singer - No Silence");
    if (!QDir().mkpath(root) || !writeSongPair(silence, Sound().music(30).silence(10), 40000)
        || !writeSongPair(shortSong, Sound().music(3), 3000)
        || !writeSongPair(noSilence, Sound().music(40), 40000))
        qFatal("Could not make the synthetic songs");
    library.endsInSilence = silence + QStringLiteral(".mp3");
    library.short1 = shortSong + QStringLiteral(".mp3");
    library.noSilence = noSilence + QStringLiteral(".mp3");
    QSignalSpy finished(&controller, &LibraryController::scanFinished);
    if (!controller.chooseRoot(root) || (finished.count() == 0 && !finished.wait(20000)))
        qFatal("The synthetic library was not scanned");
    const auto id = [&controller](const char* title) {
        const auto rows = controller.search(QString::fromLatin1(title), 10);
        if (rows.size() != 1)
            qFatal("A synthetic song was not found");
        return rows.first().songId;
    };
    library.silenceId = id("Ends In Silence");
    library.shortId = id("Short Song");
    library.noSilenceId = id("No Silence");
    return library;
}

qint64 addSong(PlaylistStore& store, LibraryController& controller, qint64 playlistId, qint64 songId)
{
    const auto ref = controller.songRef(songId);
    qint64 itemId = 0;
    if (!ref || !store.addItem(playlistId, *ref, &itemId))
        qFatal("Could not add a playlist song");
    return itemId;
}

int rowOf(const QListWidget* list, qint64 itemId)
{
    for (int row = 0; row < list->count(); ++row) {
        if (list->item(row)->data(Qt::UserRole + 1).toLongLong() == itemId)
            return row;
    }
    return -1;
}

} // namespace

class TestAutoplayEnd : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();

    // Where the music ends.
    void hardSilenceAfterTheMusicIsSkipped();
    void shortSilenceIsLeftAlone();
    void noSilencePlaysToTheEnd();
    void aLongFadeIsHeardToTheEnd();
    void aVeryQuietLastNoteIsKept();
    void aQuietBreakInTheMiddleIsNeverTheEnd();
    void aSpokenEndingIsKept();
    void soundInOneChannelOnlyIsKept();
    void aLateClickIsKeptAndFaintNoiseIsSilence();
    void hissThroughoutPlaysToTheEnd();
    void quietButAudibleEndingsAreKept();
    void unclearRecordingsPlayToTheEnd();
    void damagedSamplesNeverCount();

    // Decoding and the cache.
    void decodingFindsTheEndOfSyntheticMp3s();
    void damagedOrMissingFilesPlayToTheEnd();
    void resultsAreKeptAcrossRestarts();
    void recordingsAlikeOnlyAtTheirEndsAreNeverConfused();

    // The playlist lifecycle.
    void autoplayMovesOnAfterTheMusic();
    void onlyAutoplayEverEndsASongEarly();
    void pauseAndResumeNearTheEnd();
    void anUnknownOrDifferentRecordingPlaysToTheEnd();
    void onlyAFreshLookAtTheFileLetsASongEnd();
    void theStoredSuccessorAndItsSettingsFollow();
    void settingsShowTheChoice();
};

void TestAutoplayEnd::initTestCase()
{
    qputenv("GST_PLUGIN_FEATURE_RANK",
            "osxaudiosink:NONE,wasapi2sink:NONE,wasapisink:NONE,directsoundsink:NONE,"
            "pulsesink:NONE,alsasink:NONE,pipewiresink:NONE,jackaudiosink:NONE,openalsink:NONE");
    QString error;
    QVERIFY2(KaraokePlayer::initializeGStreamer(&error), qPrintable(error));
}

// ---- Where the music ends ---------------------------------------------------

void TestAutoplayEnd::hardSilenceAfterTheMusicIsSkipped()
{
    const AudibleEnd end = Sound().music(50).silence(10).analyse();
    QVERIFY2(end.skips(), qPrintable(describe(end)));
    QCOMPARE(end.durationMs, qint64(60000));
    QCOMPARE(end.audibleEndMs, qint64(50000));
    QCOMPARE(end.endAtMs, qint64(51000));  // a second's pause after the music
    QCOMPARE(end.savedMs(), qint64(9000));
}

void TestAutoplayEnd::shortSilenceIsLeftAlone()
{
    // Three seconds: a second's pause is kept, two are skipped.
    const AudibleEnd three = Sound().music(57).silence(3).analyse();
    QVERIFY2(three.skips(), qPrintable(describe(three)));
    QCOMPARE(three.audibleEndMs, qint64(57000));
    QCOMPARE(three.endAtMs, qint64(58000));
    // Less than that is not worth it.
    for (const double seconds : {2.9, 2.0, 1.0, 0.3}) {
        const AudibleEnd end = Sound().music(60 - seconds).silence(seconds).analyse();
        QVERIFY2(!end.skips(), qPrintable(describe(end)));
        QCOMPARE(end.endAtMs, end.durationMs);
    }
}

void TestAutoplayEnd::noSilencePlaysToTheEnd()
{
    const AudibleEnd end = Sound().music(60).analyse();
    QVERIFY2(!end.skips(), qPrintable(describe(end)));
    QCOMPARE(end.audibleEndMs, qint64(60000));
    QCOMPARE(end.savedMs(), qint64(0));
}

void TestAutoplayEnd::aLongFadeIsHeardToTheEnd()
{
    // 12 s fading from -14 to -100 dBFS (7.2 dB a second), then 3 s of silence.
    const AudibleEnd end = Sound().music(45).fade(12).silence(3).analyse();
    QVERIFY2(end.skips(), qPrintable(describe(end)));
    // The fade is heard until it is below the silence level (at most -90
    // dBFS, lower for a quieter song) and a second more; nothing louder is
    // ever cut. The fade's level is -17.6 dB RMS at its start.
    QVERIFY2(end.silenceDb <= -90.0, qPrintable(describe(end)));
    const qint64 belowSilence = 45000 + qint64((-17.64 - end.silenceDb) / (86.0 / 12.0) * 1000.0);
    QVERIFY2(end.audibleEndMs >= belowSilence - 50, qPrintable(describe(end)));
    QVERIFY2(end.audibleEndMs <= belowSilence + 100, qPrintable(describe(end)));
    QCOMPARE(end.endAtMs, end.audibleEndMs + 1000);
}

void TestAutoplayEnd::aVeryQuietLastNoteIsKept()
{
    // A last note held at -50 dBFS (36 dB below the song) for 6 s.
    const AudibleEnd end = Sound().music(50).tone(6, -50.0).silence(4).analyse();
    QVERIFY2(end.skips(), qPrintable(describe(end)));
    QCOMPARE(end.audibleEndMs, qint64(56000));
    QCOMPARE(end.endAtMs, qint64(57000));
    // Held to the end of the file: nothing to skip.
    const AudibleEnd held = Sound().music(50).tone(10, -50.0).analyse();
    QVERIFY2(!held.skips(), qPrintable(describe(held)));
}

void TestAutoplayEnd::aQuietBreakInTheMiddleIsNeverTheEnd()
{
    const AudibleEnd breakThenMusic = Sound().music(25).silence(10).music(25).analyse();
    QVERIFY2(!breakThenMusic.skips(), qPrintable(describe(breakThenMusic)));
    QCOMPARE(breakThenMusic.audibleEndMs, qint64(60000));
    // A break, more music, then silence: the end is after the second part.
    const AudibleEnd both = Sound().music(25).silence(10).music(15).silence(10).analyse();
    QVERIFY2(both.skips(), qPrintable(describe(both)));
    QCOMPARE(both.audibleEndMs, qint64(50000));
    QCOMPARE(both.endAtMs, qint64(51000));
}

void TestAutoplayEnd::aSpokenEndingIsKept()
{
    // Soft words (-40 dBFS, with pauses) after the music, then silence.
    const AudibleEnd end = Sound().music(45).speech(8, -40.0).silence(7).analyse();
    QVERIFY2(end.skips(), qPrintable(describe(end)));
    // The last word starts at 45 + 7 + 0.7 = 52.7 s... (bursts of 0.3 s every 0.7 s
    // for 8 s: the last one is 52.0-52.3 s, then 0.7 s of pause, then 52.7-53.0 s).
    QVERIFY2(end.audibleEndMs >= 53000, qPrintable(describe(end)));
    QVERIFY2(end.audibleEndMs <= 53050, qPrintable(describe(end)));
    // Words to the very end: nothing to skip.
    const AudibleEnd talking = Sound().music(45).speech(15, -40.0).analyse();
    QVERIFY2(!talking.skips(), qPrintable(describe(talking)));
}

void TestAutoplayEnd::soundInOneChannelOnlyIsKept()
{
    const AudibleEnd end = Sound().music(45).tone(6, -45.0, false).silence(9).analyse();
    QVERIFY2(end.skips(), qPrintable(describe(end)));
    QCOMPARE(end.audibleEndMs, qint64(51000));
}

void TestAutoplayEnd::aLateClickIsKeptAndFaintNoiseIsSilence()
{
    // A click 5 s into the silence: it is heard, so the end is after it.
    const AudibleEnd click = Sound().music(50).silence(5).click().silence(5).analyse();
    QVERIFY2(click.skips(), qPrintable(describe(click)));
    QVERIFY2(click.audibleEndMs >= 55000 && click.audibleEndMs <= 55100, qPrintable(describe(click)));
    // Noise at an encoder's own level (-100 dBFS) is silence...
    const AudibleEnd faint = Sound().music(50).hiss(10, -100.0).analyse();
    QVERIFY2(faint.skips(), qPrintable(describe(faint)));
    QCOMPARE(faint.audibleEndMs, qint64(50000));
    // ...but not hiss above it (-80 dBFS), however faint.
    const AudibleEnd hiss = Sound().music(50).hiss(10, -80.0).analyse();
    QVERIFY2(!hiss.skips(), qPrintable(describe(hiss)));
}

void TestAutoplayEnd::hissThroughoutPlaysToTheEnd()
{
    // Tape hiss at -55 dBFS after the music is not silence.
    const AudibleEnd end = Sound().music(50).hiss(10, -55.0).analyse();
    QVERIFY2(!end.skips(), qPrintable(describe(end)));
    QCOMPARE(end.audibleEndMs, qint64(60000));
}

void TestAutoplayEnd::quietButAudibleEndingsAreKept()
{
    // Sustained, structured sound far below the music but above the level of
    // digital silence and encoder noise (-90 dBFS): a held tone, words, a
    // tone in one channel. Kept, down to just above the silence level.
    for (const double level : {-61.0, -71.0, -80.0, -87.0}) {
        AudibleEnd end = Sound().music(50).tone(4, level).silence(6).analyse();
        QVERIFY2(end.skips() && end.audibleEndMs == 54000, qPrintable(describe(end)));
        end = Sound().music(50).speech(4.2, level).silence(6).analyse();
        QVERIFY2(end.skips() && end.audibleEndMs >= 53800, qPrintable(describe(end)));
        end = Sound().music(50).tone(4, level, false).silence(6).analyse();
        QVERIFY2(end.skips() && end.audibleEndMs == 54000, qPrintable(describe(end)));
        // Held to the end of the file: nothing is skipped.
        end = Sound().music(50).tone(10, level).analyse();
        QVERIFY2(!end.skips(), qPrintable(describe(end)));
    }
    // At an encoder's noise level (-100 dBFS) it is silence.
    const AudibleEnd faint = Sound().music(50).tone(4, -100.0).silence(6).analyse();
    QVERIFY2(faint.skips() && faint.audibleEndMs == 50000, qPrintable(describe(faint)));
    // A quieter song is judged against itself too: 78 dB below it.
    const AudibleEnd quietSong = Sound().music(50, -30.0).tone(4, -100.0).silence(6).analyse();
    QVERIFY2(quietSong.skips() && quietSong.audibleEndMs == 54000, qPrintable(describe(quietSong)));
}

void TestAutoplayEnd::unclearRecordingsPlayToTheEnd()
{
    // A short recording.
    AudibleEnd end = Sound().music(15).silence(10).analyse();
    QVERIFY2(!end.skips(), qPrintable(describe(end)));
    QCOMPARE(QString::fromStdString(end.reason), QStringLiteral("a short recording"));
    // Silence for most of the track: the music is not half of it.
    end = Sound().music(25).silence(35).analyse();
    QVERIFY2(!end.skips(), qPrintable(describe(end)));
    QCOMPARE(QString::fromStdString(end.reason), QStringLiteral("silence makes up much of the recording"));
    // A whole recording far quieter than any karaoke track.
    end = Sound().music(50, -52.0).silence(10).analyse();
    QVERIFY2(!end.skips(), qPrintable(describe(end)));
    QCOMPARE(QString::fromStdString(end.reason), QStringLiteral("a very quiet recording"));
    // Nothing at all.
    end = AudibleEndDetector().finish();
    QVERIFY(!end.skips());
    QCOMPARE(end.durationMs, qint64(0));
    end = Sound().silence(60).analyse();
    QVERIFY(!end.skips());
}

void TestAutoplayEnd::damagedSamplesNeverCount()
{
    // A value the decoder could not make sense of counts as loud.
    Sound sound;
    sound.music(50).silence(10);
    std::vector<float> samples = sound.samples();
    samples[std::size_t(58 * kRate) * 2] = std::nanf("");
    AudibleEndDetector detector;
    detector.addSamples(samples.data(), samples.size() / 2);
    const AudibleEnd end = detector.finish();
    QVERIFY2(!end.skips() || end.audibleEndMs >= 58000, qPrintable(describe(end)));
}

// ---- Decoding and the cache -------------------------------------------------

void TestAutoplayEnd::decodingFindsTheEndOfSyntheticMp3s()
{
    QTemporaryDir temporary;
    struct Case {
        const char* name;
        Sound sound;
        bool variable;
        bool skips;
        qint64 audibleEnd;
    };
    const QList<Case> cases = {
        {"ten-seconds-cbr", Sound().music(35).silence(10), false, true, 35000},
        {"ten-seconds-vbr", Sound().music(35).silence(10), true, true, 35000},
        // The encoder's delay moves the music ~50 ms later: just under the
        // three seconds of silence needed, so it plays to the end.
        {"three-seconds", Sound().music(40).silence(3), false, false, 40050},
        {"four-seconds", Sound().music(40).silence(4), false, true, 40050},
        {"no-silence", Sound().music(40), false, false, 40000},
        // Fading by 8.6 dB a second from -17.6 dB RMS: below the silence level
        // (-95 dBFS for this song) after 9.0 s; the encoder's noise in the
        // tail can keep a little more, never less.
        {"fade", Sound().music(30).fade(10).silence(5), true, true, 39030},
        {"quiet-last-note", Sound().music(30).tone(6, -50.0).silence(6), false, true, 36000},
        {"middle-break", Sound().music(20).silence(8).music(15), false, false, 43000},
        {"spoken-end", Sound().music(30).speech(8, -40.0).silence(6), true, true, 38000},
    };
    auto engine = createAudibleEndEngine();
    QVERIFY(engine);
    for (const Case& test : cases) {
        const QString path = temporary.filePath(QString::fromLatin1(test.name) + QStringLiteral(".mp3"));
        QVERIFY2(writeMp3(path, test.sound, test.variable), test.name);
        AudibleEnd end;
        QString detail;
        QElapsedTimer timer;
        timer.start();
        QCOMPARE(engine->analyse(path, {}, &end, &detail), AudibleEndEngine::Outcome::Analysed);
        qInfo().noquote() << test.name << describe(end) << "in" << timer.elapsed() << "ms";
        QCOMPARE(end.skips(), test.skips);
        // The encoder adds a few milliseconds at the start; the decoded
        // length and end match the sound to within a window or two.
        const qint64 length = qint64(test.sound.samples().size() / 2) * 1000 / kRate;
        QVERIFY2(std::llabs(end.durationMs - length) <= 100, qPrintable(describe(end)));
        if (QByteArray(test.name) == "fade") {
            QVERIFY2(end.audibleEndMs >= test.audibleEnd - 50 && end.audibleEndMs <= test.audibleEnd + 600,
                     qPrintable(describe(end)));
        } else {
            QVERIFY2(std::llabs(end.audibleEndMs - test.audibleEnd) <= 150, qPrintable(describe(end)));
        }
        // The player's figure for the length agrees for a constant bitrate.
        // For a variable bitrate without a seek table it is only an estimate
        // (here about 39.5 s for 45 s), which is why Smart Autoplay End never
        // relies on it: the player's position, like the analysis, counts
        // from the start.
        KaraokePlayer player(nullptr, QStringLiteral("fakesink"));
        const QString cdg = path.chopped(4) + QStringLiteral(".cdg");
        QVERIFY(testmedia::writeCdg(cdg, testmedia::markerCdg(int(length), 500)));
        QVERIFY(player.load(resolveSongPair(path).pair));
        player.play();
        QTRY_VERIFY_WITH_TIMEOUT(player.durationMs() > 0, 5000);
        qInfo().noquote() << test.name << "player length" << player.durationMs() << "ms";
        if (!test.variable) {
            QVERIFY2(std::llabs(player.durationMs() - end.durationMs) <= 100,
                     qPrintable(QStringLiteral("player %1 ms, analysed %2 ms")
                                    .arg(player.durationMs()).arg(end.durationMs)));
        }
        player.stop();
    }
}

void TestAutoplayEnd::damagedOrMissingFilesPlayToTheEnd()
{
    QTemporaryDir temporary;
    auto engine = createAudibleEndEngine();
    AudibleEnd end;
    QString detail;
    // Not audio at all.
    const QString garbage = temporary.filePath(QStringLiteral("garbage.mp3"));
    QVERIFY(testmedia::writeFile(garbage, QByteArray(200000, '\x5a')));
    const AudibleEndEngine::Outcome outcome = engine->analyse(garbage, {}, &end, &detail);
    QVERIFY2(outcome == AudibleEndEngine::Outcome::NotAudio
                 || (outcome == AudibleEndEngine::Outcome::Analysed && !end.skips()),
             qPrintable(detail));
    // Gone.
    QCOMPARE(engine->analyse(temporary.filePath(QStringLiteral("missing.mp3")), {}, &end, &detail),
             AudibleEndEngine::Outcome::Unreadable);
    // Cut off half way through a frame: what can be read is judged as it is.
    const QString whole = temporary.filePath(QStringLiteral("whole.mp3"));
    QVERIFY(writeMp3(whole, Sound().music(40).silence(10)));
    QFile file(whole);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray bytes = file.readAll();
    const QString cut = temporary.filePath(QStringLiteral("cut.mp3"));
    QVERIFY(testmedia::writeFile(cut, bytes.left(bytes.size() * 7 / 10 + 13)));
    const AudibleEndEngine::Outcome cutOutcome = engine->analyse(cut, {}, &end, &detail);
    QVERIFY(cutOutcome != AudibleEndEngine::Outcome::Interrupted);
    if (cutOutcome == AudibleEndEngine::Outcome::Analysed)
        QVERIFY2(!end.skips(), qPrintable(describe(end)));  // it stops mid-music
    // Asked to stop: nothing is known.
    QCOMPARE(engine->analyse(whole, [] { return true; }, &end, &detail),
             AudibleEndEngine::Outcome::Interrupted);
}

void TestAutoplayEnd::resultsAreKeptAcrossRestarts()
{
    QTemporaryDir temporary;
    const QString catalogue = temporary.filePath(QStringLiteral("app/library.sqlite"));
    const QString root = temporary.filePath(QStringLiteral("music"));
    QString cachePath;
    Library library;
    {
        LibraryController controller(catalogue);
        library = prepareLibrary(root, controller);
        cachePath = controller.enrichmentCachePath();
    }
    QVERIFY(!cachePath.isEmpty());
    QVERIFY(QFileInfo(cachePath).absolutePath() != QFileInfo(root).absoluteFilePath());
    const QMap<QString, QPair<qint64, qint64>> before = snapshot(root);
    const auto rowCount = [&cachePath] {
        const QString name = QUuid::createUuid().toString();
        qint64 count = -1;
        {
            QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
            database.setDatabaseName(cachePath);
            QSqlQuery query(database);
            if (database.open() && query.exec(QStringLiteral("SELECT count(*) FROM audible_ends")) && query.next())
                count = query.value(0).toLongLong();
            query.finish();
            database.close();
        }
        QSqlDatabase::removeDatabase(name);
        return count;
    };
    QCOMPARE(rowCount(), qint64(0));

    // First session: analysed, and kept.
    auto counting = std::make_shared<CountingEngine>(createAudibleEndEngine());
    {
        AudibleEnds ends(cachePath);
        ends.setEngineFactory([counting] { return counting; });
        QSignalSpy ready(&ends, &AudibleEnds::ready);
        ends.request(library.endsInSilence);
        ends.request(library.noSilence);  // waits for the first
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 2, 20000);
        QTRY_VERIFY_WITH_TIMEOUT(!ends.isWorking(), 20000);  // its result kept, too
        QVERIFY(ends.known(library.endsInSilence)->skips());
        QVERIFY(!ends.known(library.noSilence)->skips());
        QCOMPARE(counting->calls.load(), 2);
        ends.request(library.endsInSilence);  // known: nothing to do
        QTest::qWait(100);
        QCOMPARE(ready.count(), 2);
    }
    QTRY_COMPARE_WITH_TIMEOUT(rowCount(), qint64(2), 10000);

    // After a restart: from the cache, without reading the songs' audio again.
    {
        AudibleEnds ends(cachePath);
        ends.setEngineFactory([counting] { return counting; });
        QSignalSpy ready(&ends, &AudibleEnds::ready);
        ends.request(library.endsInSilence);
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!ends.isWorking(), 20000);  // its result kept, too
        const auto end = ends.known(library.endsInSilence);
        QVERIFY(end && end->skips());
        QVERIFY2(std::llabs(end->audibleEndMs - 30000) <= 150, qPrintable(describe(*end)));
        QCOMPARE(counting->calls.load(), 2);
    }

    // A result from other rules (another version), or one that makes no sense
    // by these rules, is worked out again.
    {
        const QString name = QStringLiteral("tamper");
        {
            QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
            database.setDatabaseName(cachePath);
            QVERIFY(database.open());
            QSqlQuery query(database);
            QVERIFY(query.exec(QStringLiteral("UPDATE audible_ends SET end_at_ms=1000 WHERE status='skip'")));
            QVERIFY(query.exec(QStringLiteral("UPDATE audible_ends SET version=version+1 WHERE status<>'skip'")));
            database.close();
        }
        QSqlDatabase::removeDatabase(name);
        AudibleEnds ends(cachePath);
        ends.setEngineFactory([counting] { return counting; });
        QSignalSpy ready(&ends, &AudibleEnds::ready);
        ends.request(library.endsInSilence);
        ends.request(library.noSilence);
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 2, 20000);
        QTRY_VERIFY_WITH_TIMEOUT(!ends.isWorking(), 20000);  // its result kept, too
        QCOMPARE(counting->calls.load(), 4);
        QVERIFY2(ends.known(library.endsInSilence)->endAtMs > 30000,
                 qPrintable(describe(*ends.known(library.endsInSilence))));
    }

    // A file that cannot be read now is not remembered as anything.
    {
        auto unreadable = std::make_shared<FixedEngine>();
        unreadable->outcome = AudibleEndEngine::Outcome::Unreadable;
        AudibleEnds ends(cachePath);
        ends.setEngineFactory([unreadable] { return unreadable; });
        QSignalSpy ready(&ends, &AudibleEnds::ready);
        ends.request(library.short1);
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!ends.isWorking(), 20000);  // its result kept, too
        QVERIFY(!ends.known(library.short1));
        ends.request(library.short1);  // tried again
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 2, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!ends.isWorking(), 20000);  // its result kept, too
        QCOMPARE(unreadable->calls.load(), 2);
    }
    QTest::qWait(200);
    QCOMPARE(rowCount(), qint64(2));
    // The music folder is only ever read.
    QCOMPARE(snapshot(root), before);
}

void TestAutoplayEnd::recordingsAlikeOnlyAtTheirEndsAreNeverConfused()
{
    // The same size, the same first and last 64 KiB, different in between:
    // one key for Key/Tempo (sampled), two for where the music ends.
    QTemporaryDir temporary;
    std::mt19937 random(5);
    QByteArray first(600 * 1024, Qt::Uninitialized);
    for (char& byte : first)
        byte = char(random() & 0xff);
    QByteArray second = first;
    for (qsizetype i = 200 * 1024; i < 400 * 1024; ++i)
        second[i] = char(~second[i]);
    const QString a = temporary.filePath(QStringLiteral("a.mp3"));
    const QString b = temporary.filePath(QStringLiteral("b.mp3"));
    QVERIFY(testmedia::writeFile(a, first));
    QVERIFY(testmedia::writeFile(b, second));
    QByteArray sampledA, sampledB, completeA, completeB;
    QVERIFY(mp3AudioDigest(a, &sampledA) && mp3AudioDigest(b, &sampledB));
    QCOMPARE(sampledA, sampledB);
    QVERIFY(mp3CompleteAudioDigest(a, &completeA) && mp3CompleteAudioDigest(b, &completeB));
    QVERIFY(completeA != completeB);
    // Tags are not the audio: a retagged copy keeps its key.
    const QString tagged = temporary.filePath(QStringLiteral("tagged.mp3"));
    QByteArray id3("ID3\x03\x00\x00\x00\x00\x00\x0a", 10);
    QVERIFY(testmedia::writeFile(tagged, id3 + QByteArray(10, 'T') + first + QByteArray("TAG").leftJustified(128, ' ')));
    QByteArray completeTagged;
    QVERIFY(mp3CompleteAudioDigest(tagged, &completeTagged));
    QCOMPARE(completeTagged, completeA);
}

// ---- The playlist lifecycle -------------------------------------------------

namespace {

struct Session {
    QTemporaryDir temporary;
    std::unique_ptr<LibraryController> controller;
    Library library;
    std::unique_ptr<PlaylistStore> playlists;
    std::unique_ptr<SongSettingsStore> settings;
    std::unique_ptr<AheadPlayer> player;
    std::unique_ptr<MainWindow> window;
    std::shared_ptr<CountingEngine> engine;
    qint64 playlistId = 0;

    Session()
    {
        controller = std::make_unique<LibraryController>(temporary.filePath(QStringLiteral("app/library.sqlite")));
        library = prepareLibrary(temporary.filePath(QStringLiteral("music")), *controller);
        playlists = std::make_unique<PlaylistStore>(temporary.filePath(QStringLiteral("app/playlists.sqlite")));
        if (!playlists->open(nullptr, controller->libraryRoots())
            || !playlists->createPlaylist(QStringLiteral("Party"), &playlistId)
            || !playlists->setLastPlaylistId(playlistId))
            qFatal("Could not make the playlist");
        settings = std::make_unique<SongSettingsStore>(temporary.filePath(QStringLiteral("settings.json")));
        player = std::make_unique<AheadPlayer>();
        engine = std::make_shared<CountingEngine>(createAudibleEndEngine());
    }
    void open()
    {
        window = std::make_unique<MainWindow>(player.get(), settings.get(), controller.get(), playlists.get());
        window->setShowErrorDialogs(false);
        auto counting = engine;
        window->audibleEnds()->setEngineFactory([counting] { return counting; });
        window->resize(1000, 700);
        window->show();
    }
    // Plays the playlist's item as a double-click would.
    bool play(qint64 itemId)
    {
        PlaylistView* view = window->playlistView();
        view->refresh();
        const int row = rowOf(view->itemList(), itemId);
        if (row < 0)
            return false;
        view->itemList()->setCurrentRow(row);
        view->playSelected();
        return player->state() == KaraokePlayer::State::Playing;
    }
    // Moves the reported position to `ms` into the song.
    void jumpTo(qint64 ms)
    {
        player->ahead.store(std::max<qint64>(0, ms - player->realPositionMs()));
    }
    std::optional<music::AudibleEnd> known(const QString& path) const
    {
        return window->audibleEnds()->known(path);
    }
};

bool samePath(const QString& a, const QString& b)
{
    return QFileInfo(a).canonicalFilePath() == QFileInfo(b).canonicalFilePath();
}

} // namespace

void TestAutoplayEnd::autoplayMovesOnAfterTheMusic()
{
    Session session;
    QVERIFY(session.playlists->setAutoplay(true));
    const qint64 first = addSong(*session.playlists, *session.controller, session.playlistId, session.library.silenceId);
    const qint64 second = addSong(*session.playlists, *session.controller, session.playlistId, session.library.shortId);
    session.open();
    QVERIFY(session.play(first));
    QVERIFY(samePath(session.player->song().mp3Path, session.library.endsInSilence));
    QVERIFY(session.window->songText().contains(QStringLiteral("Ends In Silence")));

    // Not analysed in the first two seconds; then in the background.
    QTest::qWait(300);
    QCOMPARE(session.engine->calls.load(), 0);
    session.jumpTo(2500);
    QTRY_VERIFY_WITH_TIMEOUT(session.known(session.player->song().mp3Path).has_value(), 20000);
    const music::AudibleEnd end = *session.known(session.player->song().mp3Path);
    QVERIFY2(end.skips(), qPrintable(describe(end)));
    QCOMPARE(session.engine->calls.load(), 1);

    // Just before the end it was given: still the same song.
    session.jumpTo(end.endAtMs - 600);
    QTest::qWait(400);
    QCOMPARE(session.player->state(), KaraokePlayer::State::Playing);
    QVERIFY(samePath(session.player->song().mp3Path, session.library.endsInSilence));
    QCOMPARE(session.window->playlistPlayback()->context()->itemId, first);

    // Then the next song, at once, exactly as at the end of the file.
    QSignalSpy states(session.player.get(), &KaraokePlayer::stateChanged);
    QElapsedTimer timer;
    timer.start();
    session.jumpTo(end.endAtMs + 50);
    QTRY_COMPARE_WITH_TIMEOUT(session.window->playlistPlayback()->context()->itemId, second, 3000);
    QVERIFY(timer.elapsed() < 3000);
    QTRY_COMPARE_WITH_TIMEOUT(session.player->state(), KaraokePlayer::State::Playing, 3000);
    QVERIFY(samePath(session.player->song().mp3Path, session.library.short1));
    bool finished = false;
    for (const QList<QVariant>& state : states)
        finished = finished || state.first().value<KaraokePlayer::State>() == KaraokePlayer::State::Finished;
    QVERIFY(finished);  // the first song finished, as at its natural end
    // Now Playing and the lyrics are the new song's.
    QVERIFY2(session.window->songText().contains(QStringLiteral("Short Song")),
             qPrintable(session.window->songText()));
    QVERIFY(session.player->song().cdgPath.endsWith(QStringLiteral("Short Song.cdg")));
    session.player->stop();
}

void TestAutoplayEnd::onlyAutoplayEverEndsASongEarly()
{
    Session session;
    const qint64 first = addSong(*session.playlists, *session.controller, session.playlistId, session.library.silenceId);
    const qint64 last = addSong(*session.playlists, *session.controller, session.playlistId, session.library.silenceId);
    session.open();
    const auto stillPlaysPast = [&session](const char* what) -> bool {
        session.jumpTo(2500);
        if (!QTest::qWaitFor([&session] { return session.engine->calls.load() > 0; }, 20000)
            || !QTest::qWaitFor([&session] { return !session.window->audibleEnds()->isWorking(); }, 20000)) {
            qWarning("%s: the song was not analysed", what);
            return false;
        }
        const auto end = session.known(session.player->song().mp3Path);
        const qint64 endAt = end ? end->endAtMs : 31000;
        session.jumpTo(endAt + 500);
        QTest::qWait(600);
        if (session.player->state() != KaraokePlayer::State::Playing || session.player->positionMs() < endAt) {
            qWarning("%s: the song did not play on past %lld ms (at %lld ms)", what,
                     static_cast<long long>(endAt), static_cast<long long>(session.player->positionMs()));
            return false;
        }
        return true;
    };

    // Autoplay off.
    QVERIFY(!session.playlists->autoplay());
    QVERIFY(session.play(first));
    QVERIFY(stillPlaysPast("autoplay off"));
    session.player->stop();

    // The last song of the playlist: nothing to move on to.
    QVERIFY(session.playlists->setAutoplay(true));
    session.window->playlistView()->refresh();
    QVERIFY(session.play(last));
    QVERIFY(stillPlaysPast("last song"));
    session.player->stop();

    // Sung from the library, not the playlist.
    LibraryView* libraryView = session.window->libraryView();
    libraryView->searchBox()->setText(QStringLiteral("Ends In Silence"));
    libraryView->refreshSearch();
    libraryView->resultsList()->setCurrentIndex(libraryView->resultsList()->model()->index(0, 0));
    QTest::mouseClick(libraryView->singButton(), Qt::LeftButton);
    QCOMPARE(session.player->state(), KaraokePlayer::State::Playing);
    QVERIFY(!session.window->playlistPlayback()->context());
    QVERIFY(stillPlaysPast("library"));
    session.player->stop();

    // The setting turned off.
    QVERIFY(session.window->preferences()->setFlag(pref::SkipTrailingSilence, false));
    QVERIFY(session.play(first));
    QVERIFY(stillPlaysPast("setting off"));
    // Turned on again while the song is in its silence: it moves on.
    QVERIFY(session.window->preferences()->setFlag(pref::SkipTrailingSilence, true));
    QTRY_COMPARE_WITH_TIMEOUT(session.window->playlistPlayback()->context()->itemId, last, 3000);
    session.player->stop();
    QCOMPARE(session.engine->calls.load(), 1);  // one recording, analysed once
}

void TestAutoplayEnd::pauseAndResumeNearTheEnd()
{
    Session session;
    QVERIFY(session.playlists->setAutoplay(true));
    const qint64 first = addSong(*session.playlists, *session.controller, session.playlistId, session.library.silenceId);
    const qint64 second = addSong(*session.playlists, *session.controller, session.playlistId, session.library.noSilenceId);
    session.open();
    QVERIFY(session.play(first));
    session.jumpTo(2500);
    QTRY_VERIFY_WITH_TIMEOUT(session.known(session.player->song().mp3Path).has_value(), 20000);
    const music::AudibleEnd end = *session.known(session.player->song().mp3Path);
    session.jumpTo(end.endAtMs - 300);
    QTest::qWait(100);
    QTest::mouseClick(session.window->pauseButton(), Qt::LeftButton);
    QCOMPARE(session.player->state(), KaraokePlayer::State::Paused);
    // Paused in the silence: it waits, however long.
    session.jumpTo(end.endAtMs + 2000);
    QTest::qWait(700);
    QCOMPARE(session.player->state(), KaraokePlayer::State::Paused);
    QCOMPARE(session.window->playlistPlayback()->context()->itemId, first);
    // Play again: it moves on.
    QTest::mouseClick(session.window->playButton(), Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(session.window->playlistPlayback()->context()->itemId, second, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(session.player->state(), KaraokePlayer::State::Playing, 3000);
    // Stopped in the silence and played again: from the beginning, as ever.
    session.player->stop();
    session.player->ahead.store(0);
    QVERIFY(session.play(first));
    QVERIFY(session.player->positionMs() < 2000);
    QTest::qWait(300);
    QCOMPARE(session.window->playlistPlayback()->context()->itemId, first);
    session.player->stop();
}

void TestAutoplayEnd::anUnknownOrDifferentRecordingPlaysToTheEnd()
{
    Session session;
    QVERIFY(session.playlists->setAutoplay(true));
    const qint64 first = addSong(*session.playlists, *session.controller, session.playlistId, session.library.silenceId);
    addSong(*session.playlists, *session.controller, session.playlistId, session.library.shortId);

    // Not worked out (the file could not be read): the song plays on.
    auto unreadable = std::make_shared<FixedEngine>();
    unreadable->outcome = AudibleEndEngine::Outcome::Unreadable;
    session.open();
    session.window->audibleEnds()->setEngineFactory([unreadable] { return unreadable; });
    QVERIFY(session.play(first));
    session.jumpTo(2500);
    QTRY_COMPARE_WITH_TIMEOUT(unreadable->calls.load(), 1, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!session.window->audibleEnds()->isWorking(), 10000);
    session.jumpTo(36000);
    QTest::qWait(600);
    QCOMPARE(session.player->state(), KaraokePlayer::State::Playing);
    QCOMPARE(session.window->playlistPlayback()->context()->itemId, first);
    session.player->stop();
    session.window.reset();

    // A result for a recording of another length (not this file): plays on.
    auto other = std::make_shared<FixedEngine>();
    other->end = Sound().music(40).silence(10).analyse();  // 50 s, not 40 s
    QVERIFY(other->end.skips());
    session.open();
    session.window->audibleEnds()->setEngineFactory([other] { return other; });
    QVERIFY(session.play(first));
    session.jumpTo(2500);
    QTRY_VERIFY_WITH_TIMEOUT(session.known(session.player->song().mp3Path).has_value(), 10000);
    session.jumpTo(other->end.endAtMs - 9000 + 500);  // past 32 s
    QTest::qWait(600);
    QCOMPARE(session.player->state(), KaraokePlayer::State::Playing);
    QCOMPARE(session.window->playlistPlayback()->context()->itemId, first);
    session.player->stop();
    session.player->ahead.store(0);
    session.window.reset();

    // (The made-up result above was kept in the cache: gone again.)
    {
        const QString name = QStringLiteral("forget-results");
        {
            QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
            database.setDatabaseName(session.controller->enrichmentCachePath());
            QVERIFY(database.open());
            QSqlQuery query(database);
            QVERIFY(query.exec(QStringLiteral("DELETE FROM audible_ends")));
            database.close();
        }
        QSqlDatabase::removeDatabase(name);
    }

    // Unreadable the first time it was sung, readable the next: worked out then.
    auto flaky = std::make_shared<FixedEngine>();
    flaky->outcome = AudibleEndEngine::Outcome::Unreadable;
    session.open();
    std::shared_ptr<AudibleEndEngine> current = flaky;
    session.window->audibleEnds()->setEngineFactory([&current] { return current; });
    QVERIFY(session.play(first));
    session.jumpTo(2500);
    QTRY_COMPARE_WITH_TIMEOUT(flaky->calls.load(), 1, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!session.window->audibleEnds()->isWorking(), 10000);
    session.player->stop();
    session.player->ahead.store(0);
    current = session.engine;
    QVERIFY(session.play(first));
    session.jumpTo(2500);
    QTRY_VERIFY_WITH_TIMEOUT(session.known(session.player->song().mp3Path).has_value(), 20000);
    const music::AudibleEnd end = *session.known(session.player->song().mp3Path);
    QVERIFY(end.skips());

    // The file changed on disk after it was analysed: it plays to its end.
    QFile mp3(session.player->song().mp3Path);
    QVERIFY(mp3.open(QIODevice::ReadWrite));
    QVERIFY(mp3.setFileTime(QDateTime::currentDateTime().addSecs(-3600), QFileDevice::FileModificationTime));
    mp3.close();
    session.jumpTo(end.endAtMs + 300);
    QTest::qWait(800);
    QCOMPARE(session.player->state(), KaraokePlayer::State::Playing);
    QCOMPARE(session.window->playlistPlayback()->context()->itemId, first);
    session.player->stop();
}

void TestAutoplayEnd::onlyAFreshLookAtTheFileLetsASongEnd()
{
    // At the end point the file is looked at; that it was the one analysed
    // a while ago (before a pause, say) is not enough.
    Session session;
    QVERIFY(session.playlists->setAutoplay(true));
    const qint64 first = addSong(*session.playlists, *session.controller, session.playlistId, session.library.silenceId);
    session.open();
    QVERIFY(session.play(first));
    session.jumpTo(2500);
    QTRY_VERIFY_WITH_TIMEOUT(session.known(session.player->song().mp3Path).has_value(), 20000);
    const music::AudibleEnd end = *session.known(session.player->song().mp3Path);
    QVERIFY(end.skips());
    // At the end point with nothing to move on to: the file is looked at
    // (and found unchanged), but the song plays on.
    session.jumpTo(end.endAtMs + 200);
    QTest::qWait(500);
    QCOMPARE(session.player->state(), KaraokePlayer::State::Playing);
    // Paused; the file changes; a song is added to follow; played again.
    QTest::mouseClick(session.window->pauseButton(), Qt::LeftButton);
    QFile mp3(session.player->song().mp3Path);
    QVERIFY(mp3.open(QIODevice::ReadWrite));
    QVERIFY(mp3.setFileTime(QDateTime::currentDateTime().addSecs(-7200), QFileDevice::FileModificationTime));
    mp3.close();
    const qint64 next = addSong(*session.playlists, *session.controller, session.playlistId, session.library.shortId);
    QTest::qWait(1200);
    QTest::mouseClick(session.window->playButton(), Qt::LeftButton);
    QTest::qWait(800);
    QCOMPARE(session.player->state(), KaraokePlayer::State::Playing);
    QCOMPARE(session.window->playlistPlayback()->context()->itemId, first);
    QVERIFY(session.player->positionMs() > end.endAtMs);
    // An unchanged file: it moves on as soon as the look has answered.
    session.player->stop();
    session.player->ahead.store(0);
    session.window.reset();
    session.open();
    QVERIFY(session.play(first));
    session.jumpTo(2500);
    QTRY_VERIFY_WITH_TIMEOUT(session.known(session.player->song().mp3Path).has_value(), 20000);
    session.jumpTo(session.known(session.player->song().mp3Path)->endAtMs + 100);
    QTRY_COMPARE_WITH_TIMEOUT(session.window->playlistPlayback()->context()->itemId, next, 3000);
    session.player->stop();
}

void TestAutoplayEnd::theStoredSuccessorAndItsSettingsFollow()
{
    Session session;
    QVERIFY(session.playlists->setAutoplay(true));
    const qint64 first = addSong(*session.playlists, *session.controller, session.playlistId, session.library.silenceId);
    const qint64 second = addSong(*session.playlists, *session.controller, session.playlistId, session.library.shortId);
    const qint64 third = addSong(*session.playlists, *session.controller, session.playlistId, session.library.noSilenceId);
    // The third song is remembered at Key +2 and Tempo 90%.
    const SongPair thirdPair = resolveSongPair(session.library.noSilence).pair;
    SongSettings remembered;
    remembered.keySemitones = 2;
    remembered.tempoPercent = 90;
    QVERIFY(session.settings->store(songIdentity(thirdPair), remembered, thirdPair));
    session.open();
    QVERIFY(session.play(first));
    session.jumpTo(2500);
    QTRY_VERIFY_WITH_TIMEOUT(session.known(session.player->song().mp3Path).has_value(), 20000);
    // While the first song plays, the third is moved up: it follows now.
    QVERIFY(session.playlists->moveItem(third, 1));
    QCOMPARE(session.playlists->itemAfter(session.playlistId, first)->itemId, third);
    session.jumpTo(session.known(session.player->song().mp3Path)->endAtMs + 100);
    QTRY_COMPARE_WITH_TIMEOUT(session.window->playlistPlayback()->context()->itemId, third, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(session.player->state(), KaraokePlayer::State::Playing, 3000);
    QVERIFY(samePath(session.player->song().mp3Path, session.library.noSilence));
    QCOMPARE(session.player->keySemitones(), 2);
    QCOMPARE(session.player->tempoPercent(), 90);
    QVERIFY(session.window->songText().contains(QStringLiteral("No Silence")));
    QVERIFY(second > 0);
    session.player->stop();
}

void TestAutoplayEnd::settingsShowTheChoice()
{
    AppPreferences preferences;
    QVERIFY(preferences.flag(pref::SkipTrailingSilence, true));
    AheadPlayer player;
    SongSettingsStore settings(QDir::temp().filePath(QStringLiteral("unused-%1.json")
                                                         .arg(QUuid::createUuid().toString(QUuid::WithoutBraces))));
    MainWindow window(&player, &settings, nullptr, nullptr, &preferences);
    SettingsDialog* dialog = window.openSettings();
    QVERIFY(dialog);
    dialog->showPage(QStringLiteral("Playback"));
    auto* box = dialog->findChild<QCheckBox*>(QStringLiteral("skipTrailingSilence"));
    QVERIFY(box);
    QCOMPARE(box->text(), QStringLiteral("Skip trailing silence during Autoplay"));
    QVERIFY(box->isChecked());  // on unless turned off
    box->click();
    QVERIFY(!preferences.flag(pref::SkipTrailingSilence, true));
    box->click();
    QVERIFY(preferences.flag(pref::SkipTrailingSilence, false));
    dialog->close();
}

QTEST_MAIN(TestAutoplayEnd)
#include "tst_autoplayend.moc"
