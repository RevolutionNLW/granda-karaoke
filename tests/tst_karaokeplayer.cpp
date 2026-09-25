// Exercises KaraokePlayer with real GStreamer playback of synthetic MP3s.
// Audio goes to a synchronised fakesink, so the position advances in real
// time without needing a sound device.

#include "KaraokePlayer.h"
#include "BusTestPlayer.h"
#include "SongPair.h"
#include "TestMedia.h"

#include <QDir>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using State = KaraokePlayer::State;
using cdg::CdgDecoder;

namespace {

class PositionPlayer : public KaraokePlayer {
public:
    PositionPlayer() : KaraokePlayer(nullptr, "fakesink") {}
    qint64 reportedPosition = 0;
protected:
    bool queryAudioPosition(qint64& ms) const override
    {
        ms = reportedPosition;
        return true;
    }
};

constexpr int kSongMs = 3000;
constexpr int kMarkerMs = 1000;
constexpr int kShortSongMs = 1500;

// The core synchronisation rule: the lyrics are always decoded up to exactly
// the audio position, never further.
void verifyLyricsMatchAudio(const KaraokePlayer& player)
{
    const std::size_t expected = std::min(CdgDecoder::packetsDueAt(player.positionMs()),
                                          player.decoder().packetCount());
    QCOMPARE(player.decoder().packetsApplied(), expected);
}

} // namespace

class TestKaraokePlayer : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void loadEntersReady();
    void lyricsFollowAudioClock();
    void backwardPositionRewindsLyrics();
    void queuedEosDoesNotFinishRestart();
    void deletedMp3IsReportedOncePerAttempt();
    void rejectedLyricsPreservePlayback_data();
    void rejectedLyricsPreservePlayback();
    void explicitSinkDoesNotReportUnavailableOutput();
    void defaultSinkReportsUnavailableOutput();
    void skippedInstructionsAreSummarised_data();
    void skippedInstructionsAreSummarised();
    void pauseHoldsPositionAndLyrics();
    void stopResetsAndPlayRestarts();
    void endOfTrackFinishesAndResets();
    void loadingAnotherSongStopsAndResets();
    void damagedMp3IsReported();
    void rejectedLoadDoesNotSuppressPlaybackFailure();
    void immediatePlayReportsQueuedPrerollError();
    void queuedReadyErrorPreventsPlaying();
    void damagedCdgIsRejected();
    void externalMediaSmokeTest();

private:
    SongPair makePair(const QString& name, int durationMs, int markerMs);

    QTemporaryDir m_dir;
    SongPair m_song;
    SongPair m_shortSong;
};

SongPair TestKaraokePlayer::makePair(const QString& name, int durationMs, int markerMs)
{
    SongPair pair{m_dir.filePath(name + ".mp3"), m_dir.filePath(name + ".cdg")};
    if (!testmedia::writeMp3(pair.mp3Path, durationMs))
        qFatal("Could not encode test MP3 (is lamemp3enc available?)");
    if (!testmedia::writeCdg(pair.cdgPath, testmedia::markerCdg(durationMs, markerMs)))
        qFatal("Could not write test CDG");
    return pair;
}

void TestKaraokePlayer::initTestCase()
{
    // Apply before the first gst_init_check, including when run outside ctest.
    QVERIFY(!gst_is_initialized());
    qputenv("GST_PLUGIN_FEATURE_RANK",
            "osxaudiosink:NONE,wasapi2sink:NONE,wasapisink:NONE,directsoundsink:NONE,"
            "pulsesink:NONE,alsasink:NONE,pipewiresink:NONE,jackaudiosink:NONE,openalsink:NONE");
#ifdef Q_OS_MACOS
    // Set these before the process's first gst_init_check, not after discovery.
    for (const char* name : {"GST_PLUGIN_PATH", "GST_PLUGIN_PATH_1_0",
                             "GST_PLUGIN_SYSTEM_PATH", "GST_PLUGIN_SYSTEM_PATH_1_0"})
        qputenv(name, "/opt/homebrew/lib/gstreamer-1.0");
#endif
    QString error;
    QVERIFY2(KaraokePlayer::initializeGStreamer(&error), qPrintable(error));
#ifdef Q_OS_MACOS
    for (const char* name : {"GST_PLUGIN_PATH", "GST_PLUGIN_PATH_1_0",
                             "GST_PLUGIN_SYSTEM_PATH", "GST_PLUGIN_SYSTEM_PATH_1_0"})
        QVERIFY(!qEnvironmentVariableIsSet(name));
    GstPlugin* plugin = gst_plugin_load_by_name("coreelements");
    QVERIFY(plugin);
    const QString filename = QString::fromUtf8(gst_plugin_get_filename(plugin));
    gst_object_unref(plugin);
    QVERIFY(!filename.isEmpty());
    QVERIFY2(!filename.contains("/opt/homebrew"), qPrintable(filename));
#endif
    QVERIFY(m_dir.isValid());
    m_song = makePair("Song A", kSongMs, kMarkerMs);
    m_shortSong = makePair("Song B", kShortSongMs, 500);
}

void TestKaraokePlayer::loadEntersReady()
{
    KaraokePlayer player(nullptr, "fakesink");
    QCOMPARE(player.state(), State::Empty);
    QVERIFY(player.load(m_song));
    QCOMPARE(player.state(), State::Ready);
    QCOMPARE(player.positionMs(), 0);
    QCOMPARE(player.decoder().packetsApplied(), 0u);
    QCOMPARE(player.decoder().packetCount(), std::size_t(kSongMs * 300 / 1000));
}

void TestKaraokePlayer::lyricsFollowAudioClock()
{
    KaraokePlayer player(nullptr, "fakesink");
    QVERIFY(player.load(m_song));
    player.play();
    QCOMPARE(player.state(), State::Playing);

    qint64 last = 0;
    bool sawBeforeMarker = false;
    bool sawAfterMarker = false;
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < 2000) {
        QTest::qWait(50);
        player.tick();
        QCOMPARE(player.state(), State::Playing);
        QVERIFY(player.positionMs() >= last);
        last = player.positionMs();
        verifyLyricsMatchAudio(player);

        const auto markerPixel = player.decoder().pixel(6, 12);
        if (player.positionMs() < kMarkerMs) {
            QCOMPARE(markerPixel, 0);
            sawBeforeMarker = true;
        } else if (player.positionMs() >= kMarkerMs + 4) {
            QCOMPARE(markerPixel, 1);
            sawAfterMarker = true;
        }
    }
    QVERIFY(sawBeforeMarker);
    QVERIFY(sawAfterMarker);
    // Position tracks real time (allowing for start-up and scheduling).
    QVERIFY2(last > 1200 && last < 2300, qPrintable(QString::number(last)));
    player.stop();
}

void TestKaraokePlayer::pauseHoldsPositionAndLyrics()
{
    KaraokePlayer player(nullptr, "fakesink");
    QVERIFY(player.load(m_song));
    player.play();
    QTest::qWait(600);
    player.pause();
    QCOMPARE(player.state(), State::Paused);
    const qint64 pausedAt = player.positionMs();
    const std::size_t applied = player.decoder().packetsApplied();
    QVERIFY(pausedAt > 300);
    verifyLyricsMatchAudio(player);

    QTest::qWait(700);
    player.tick();
    QCOMPARE(player.positionMs(), pausedAt);
    QCOMPARE(player.decoder().packetsApplied(), applied);

    player.play();
    QCOMPARE(player.state(), State::Playing);
    QTest::qWait(300);
    player.tick();
    // Resumed from the same point: not restarted, and not jumped ahead by
    // the time spent paused.
    QVERIFY2(player.positionMs() >= pausedAt, qPrintable(QString::number(player.positionMs())));
    QVERIFY2(player.positionMs() < pausedAt + 600, qPrintable(QString::number(player.positionMs())));
    verifyLyricsMatchAudio(player);
    player.stop();
}

void TestKaraokePlayer::stopResetsAndPlayRestarts()
{
    KaraokePlayer player(nullptr, "fakesink");
    QVERIFY(player.load(m_song));
    player.play();
    QTRY_VERIFY_WITH_TIMEOUT(player.positionMs() > kMarkerMs + 100, 3000);
    QCOMPARE(player.decoder().pixel(6, 12), 1);

    player.stop();
    QCOMPARE(player.state(), State::Stopped);
    QCOMPARE(player.positionMs(), 0);
    QCOMPARE(player.decoder().packetsApplied(), 0u);
    QCOMPARE(player.decoder().pixel(6, 12), 0);
    QCOMPARE(player.currentFrame().pixel(6, 12), 0xFF000000u);

    player.play();
    QCOMPARE(player.state(), State::Playing);
    QTest::qWait(300);
    player.tick();
    QVERIFY(player.positionMs() > 100);
    QVERIFY2(player.positionMs() < 700, qPrintable(QString::number(player.positionMs())));
    QCOMPARE(player.decoder().pixel(6, 12), 0);
    verifyLyricsMatchAudio(player);
    player.stop();
}

void TestKaraokePlayer::endOfTrackFinishesAndResets()
{
    KaraokePlayer player(nullptr, "fakesink");
    QSignalSpy states(&player, &KaraokePlayer::stateChanged);
    QVERIFY(player.load(m_shortSong));
    player.play();
    QTRY_COMPARE_WITH_TIMEOUT(player.state(), State::Finished, kShortSongMs + 4000);
    QCOMPARE(player.positionMs(), 0);
    QCOMPARE(player.decoder().packetsApplied(), 0u);
    QCOMPARE(states.last().first().value<State>(), State::Finished);

    // Can be played again from the start.
    player.play();
    QCOMPARE(player.state(), State::Playing);
    QTest::qWait(200);
    player.tick();
    QVERIFY(player.positionMs() > 100);
    QVERIFY(player.positionMs() < 600);
    player.stop();
}

void TestKaraokePlayer::loadingAnotherSongStopsAndResets()
{
    KaraokePlayer player(nullptr, "fakesink");
    QVERIFY(player.load(m_song));
    player.play();
    QTRY_VERIFY_WITH_TIMEOUT(player.decoder().pixel(6, 12) == 1, 3000);

    QVERIFY(player.load(m_shortSong));
    QCOMPARE(player.state(), State::Ready);
    QCOMPARE(player.song().mp3Path, m_shortSong.mp3Path);
    QCOMPARE(player.positionMs(), 0);
    QCOMPARE(player.decoder().packetsApplied(), 0u);
    QCOMPARE(player.decoder().pixel(6, 12), 0);
    QCOMPARE(player.decoder().packetCount(), std::size_t(kShortSongMs * 300 / 1000));

    // Nothing from the previous song keeps running.
    QTest::qWait(300);
    player.tick();
    QCOMPARE(player.state(), State::Ready);
    QCOMPARE(player.positionMs(), 0);
}

void TestKaraokePlayer::damagedMp3IsReported()
{
    SongPair pair{m_dir.filePath("Broken.mp3"), m_dir.filePath("Broken.cdg")};
    QVERIFY(testmedia::writeFile(pair.mp3Path, QByteArray(20000, '\x5A')));
    QVERIFY(testmedia::writeCdg(pair.cdgPath, testmedia::markerCdg(1000, 500)));

    KaraokePlayer player(nullptr, "fakesink");
    QSignalSpy errors(&player, &KaraokePlayer::errorOccurred);
    player.load(pair);
    QTRY_COMPARE_WITH_TIMEOUT(player.state(), State::Error, 3000);
    QCOMPARE(errors.count(), 1);
    const QString firstError = player.lastError();
    QTest::qWait(1000);
    player.tick();
    QCOMPARE(errors.count(), 1);
    QCOMPARE(player.lastError(), firstError);
    QVERIFY2(player.lastError().contains("could not be played"), qPrintable(player.lastError()));

    // Pressing Play again reports the problem again rather than hanging.
    player.play();
    QTRY_COMPARE_WITH_TIMEOUT(errors.count(), 2, 3000);
    QTest::qWait(1000);
    player.tick();
    QCOMPARE(errors.count(), 2);
    QCOMPARE(player.state(), State::Error);
}

void TestKaraokePlayer::rejectedLoadDoesNotSuppressPlaybackFailure()
{
    BusTestPlayer player;
    QVERIFY(player.load(m_song));
    player.play();
    QTRY_VERIFY_WITH_TIMEOUT(player.positionMs() > 100, 3000);
    SongPair bad{m_shortSong.mp3Path, m_dir.filePath("RejectedBeforeFailure.cdg")};
    QVERIFY(testmedia::writeFile(bad.cdgPath, QByteArray(24000, '\x5a')));
    QSignalSpy errors(&player, &KaraokePlayer::errorOccurred);
    QVERIFY(!player.load(bad));
    QCOMPARE(errors.count(), 1);
    QCOMPARE(player.state(), State::Playing);
    QCOMPARE(player.song().mp3Path, m_song.mp3Path);
    errors.clear();
    QVERIFY(player.postError());
    player.tick();
    QCOMPARE(player.state(), State::Error);
    QCOMPARE(errors.count(), 1);
    QVERIFY(player.lastError().contains(m_song.displayName()));
    // Rejected validation must not reset an already reported failure either.
    QVERIFY(!player.load(bad));
    QCOMPARE(errors.count(), 2);
    QVERIFY(player.postError());
    player.tick();
    QCOMPARE(errors.count(), 2);
    QCOMPARE(player.state(), State::Error);
}

void TestKaraokePlayer::immediatePlayReportsQueuedPrerollError()
{
    SongPair bad{m_dir.filePath("Preroll.mp3"), m_dir.filePath("Preroll.cdg")};
    QVERIFY(testmedia::writeFile(bad.mp3Path, QByteArray(20000, '\x5a')));
    QVERIFY(testmedia::writeCdg(bad.cdgPath, testmedia::markerCdg(1000, 500)));
    BusTestPlayer player;
    QSignalSpy errors(&player, &KaraokePlayer::errorOccurred);
    QVERIFY(player.load(bad));
    QCOMPARE(player.state(), State::Ready);
    QVERIFY(player.waitForFailedPreroll());
    QCOMPARE(errors.count(), 0);
    QSignalSpy states(&player, &KaraokePlayer::stateChanged);
    player.play(); // No tick or Qt event processing between load and play.
    QCOMPARE(player.state(), State::Error);
    QCOMPARE(errors.count(), 1);
    QVERIFY(player.lastError().contains("damaged or in an unsupported format"));
    for (const auto& args : states)
        QVERIFY(args.first().value<State>() != State::Playing);
    QTest::qWait(200);
    player.tick();
    QCOMPARE(errors.count(), 1);
    QCOMPARE(player.state(), State::Error);
}

void TestKaraokePlayer::queuedReadyErrorPreventsPlaying()
{
    // A real corrupt MP3 can also make set_state(PLAYING) fail, masking an
    // incorrectly discarded bus error. Queue an error on an otherwise playable
    // pre-rolled pipeline to verify that play() consumes it before starting.
    BusTestPlayer player;
    QVERIFY(player.load(m_song));
    QSignalSpy errors(&player, &KaraokePlayer::errorOccurred);
    QSignalSpy states(&player, &KaraokePlayer::stateChanged);
    QVERIFY(player.postError());
    player.play();
    QCOMPARE(player.state(), State::Error);
    QCOMPARE(errors.count(), 1);
    for (const auto& args : states)
        QVERIFY(args.first().value<State>() != State::Playing);
    player.tick();
    QCOMPARE(errors.count(), 1);
}

void TestKaraokePlayer::damagedCdgIsRejected()
{
    SongPair pair{m_dir.filePath("Tiny.mp3"), m_dir.filePath("Tiny.cdg")};
    QVERIFY(QFile::copy(m_shortSong.mp3Path, pair.mp3Path));
    QVERIFY(testmedia::writeFile(pair.cdgPath, QByteArray(10, '\x09')));

    KaraokePlayer player(nullptr, "fakesink");
    QSignalSpy errors(&player, &KaraokePlayer::errorOccurred);
    QVERIFY(!player.load(pair));
    QCOMPARE(player.state(), State::Empty);
    QCOMPARE(errors.count(), 1);
    QVERIFY(player.lastError().contains("lyrics file"));
}

void TestKaraokePlayer::backwardPositionRewindsLyrics()
{
    PositionPlayer player;
    QVERIFY(player.load(m_song));
    player.play();
    player.reportedPosition = kMarkerMs + 100;
    player.tick();
    QCOMPARE(player.decoder().pixel(6, 12), 1);
    player.reportedPosition = kMarkerMs - 1;  // backwards by less than the old 250 ms hold
    player.tick();
    QCOMPARE(player.positionMs(), kMarkerMs - 1);
    verifyLyricsMatchAudio(player);
    QCOMPARE(player.decoder().pixel(6, 12), 0);
}

void TestKaraokePlayer::queuedEosDoesNotFinishRestart()
{
    KaraokePlayer player(nullptr, "fakesink");
    QVERIFY(player.load(m_shortSong));
    player.play();
    QTRY_VERIFY_WITH_TIMEOUT(player.positionMs() > 100, 3000);
    // Audio reaches EOS on its streaming thread while the GUI timer cannot run.
    QThread::msleep(kShortSongMs + 500);
    QCOMPARE(player.state(), State::Playing);
    player.stop();
    player.play();
    player.tick();
    QCOMPARE(player.state(), State::Playing);
    QTRY_VERIFY_WITH_TIMEOUT(player.positionMs() > 100, 1000);
    QCOMPARE(player.state(), State::Playing);
}

void TestKaraokePlayer::deletedMp3IsReportedOncePerAttempt()
{
    const SongPair pair = makePair("Deleted", 1000, 500);
    const auto resolved = resolveSongPair(pair.mp3Path);
    QVERIFY(resolved.pair.isValid());
    QVERIFY(QFile::remove(pair.mp3Path));
    KaraokePlayer player(nullptr, "fakesink");
    QSignalSpy errors(&player, &KaraokePlayer::errorOccurred);
    QVERIFY(player.load(resolved.pair));
    QTRY_COMPARE_WITH_TIMEOUT(player.state(), State::Error, 3000);
    QCOMPARE(errors.count(), 1);
    const QString firstError = player.lastError();
    QVERIFY2(firstError.contains("could not be read"), qPrintable(firstError));
    QTest::qWait(1000);
    player.tick();
    QCOMPARE(errors.count(), 1);
    QCOMPARE(player.lastError(), firstError);
    player.play();
    QTRY_COMPARE_WITH_TIMEOUT(errors.count(), 2, 3000);
    const QString retryError = player.lastError();
    QTest::qWait(1000);
    player.tick();
    QCOMPARE(errors.count(), 2);
    QCOMPARE(player.state(), State::Error);
    QCOMPARE(player.lastError(), retryError);
}

void TestKaraokePlayer::rejectedLyricsPreservePlayback_data()
{
    QTest::addColumn<QString>("kind");
    for (const QString& kind : {"missing", "tiny", "garbage", "oversized"})
        QTest::newRow(qPrintable(kind)) << kind;
}

void TestKaraokePlayer::rejectedLyricsPreservePlayback()
{
    QFETCH(QString, kind);
    SongPair bad{m_shortSong.mp3Path, m_dir.filePath(kind + ".cdg")};
    if (kind == "oversized") {
        QFile file(bad.cdgPath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.resize(64 * 1024 * 1024 + 1));  // sparse; no large allocation
    } else if (kind != "missing") {
        QVERIFY(testmedia::writeFile(bad.cdgPath, QByteArray(kind == "tiny" ? 10 : 24000, '\x5a')));
    }
    KaraokePlayer player(nullptr, "fakesink");
    QVERIFY(player.load(m_song));
    player.play();
    QTRY_VERIFY_WITH_TIMEOUT(player.positionMs() > 100, 3000);
    QSignalSpy errors(&player, &KaraokePlayer::errorOccurred);
    QSignalSpy states(&player, &KaraokePlayer::stateChanged);
    const qint64 position = player.positionMs();
    const auto revision = player.decoder().revision();
    const auto packets = player.decoder().packetsApplied();
    QVERIFY(!player.load(bad));
    QCOMPARE(errors.count(), 1);
    QCOMPARE(states.count(), 0);
    QCOMPARE(player.state(), State::Playing);
    QCOMPARE(player.song().mp3Path, m_song.mp3Path);
    QCOMPARE(player.positionMs(), position);
    QCOMPARE(player.decoder().revision(), revision);
    QCOMPARE(player.decoder().packetsApplied(), packets);
    if (kind == "oversized")
        QVERIFY(player.lastError().contains("64 MB"));
    if (kind == "garbage")
        QVERIFY(player.lastError().contains("empty or damaged"));
    QTRY_VERIFY_WITH_TIMEOUT(player.positionMs() > position + 100, 1500);
    QCOMPARE(player.state(), State::Playing);
}

void TestKaraokePlayer::explicitSinkDoesNotReportUnavailableOutput()
{
    class ExplicitSinkPlayer : public BusTestPlayer {
    public:
        using KaraokePlayer::pipeline;
    } player;
    QSignalSpy errors(&player, &KaraokePlayer::errorOccurred);
    QVERIFY(player.load(m_song));
    GstState current = GST_STATE_NULL;
    QCOMPARE(gst_element_get_state(player.pipeline(), &current, nullptr, 3 * GST_SECOND),
             GST_STATE_CHANGE_SUCCESS);
    QCOMPARE(current, GST_STATE_PAUSED);
    player.tick(); // Consume PAUSED bus messages through checkAudioSink().
    QCOMPARE(player.state(), State::Ready);
    QCOMPARE(errors.count(), 0);

    player.play();
    QCOMPARE(gst_element_get_state(player.pipeline(), &current, nullptr, 3 * GST_SECOND),
             GST_STATE_CHANGE_SUCCESS);
    QCOMPARE(current, GST_STATE_PLAYING);
    player.tick(); // Consume PLAYING bus messages through checkAudioSink().
    QTRY_VERIFY_WITH_TIMEOUT(player.positionMs() > 100, 3000);
    QCOMPARE(player.state(), State::Playing);
    QCOMPARE(errors.count(), 0);
}

void TestKaraokePlayer::defaultSinkReportsUnavailableOutput()
{
    KaraokePlayer player;
    QSignalSpy errors(&player, &KaraokePlayer::errorOccurred);
    QVERIFY(player.load(m_song));
    player.play();
    QTRY_COMPARE_WITH_TIMEOUT(player.state(), State::Error, 3000);
    const QString expected = QStringLiteral("The computer's sound output could not be used. "
                                           "Please check that speakers or headphones are connected.");
    QCOMPARE(player.lastError(), expected);
    QCOMPARE(errors.count(), 1);
    QCOMPARE(errors.at(0).at(0).toString(), expected);
    QTest::qWait(1000);
    QCOMPARE(errors.count(), 1);
    player.play();
    QTRY_COMPARE_WITH_TIMEOUT(errors.count(), 2, 3000);
    QCOMPARE(player.state(), State::Error);
    QCOMPARE(player.lastError(), expected);
    QCOMPARE(errors.at(1).at(0).toString(), expected);
    QTest::qWait(1000);
    QCOMPARE(errors.count(), 2);
    QCOMPARE(player.state(), State::Error);
}

void TestKaraokePlayer::skippedInstructionsAreSummarised_data()
{
    QTest::addColumn<QString>("ending");
    QTest::newRow("stop") << QStringLiteral("stop");
    QTest::newRow("finish") << QStringLiteral("finish");
    QTest::newRow("replace") << QStringLiteral("replace");
    QTest::newRow("destroy") << QStringLiteral("destroy");
}

void TestKaraokePlayer::skippedInstructionsAreSummarised()
{
    QFETCH(QString, ending);
    SongPair pair{m_shortSong.mp3Path, m_dir.filePath("Skipped.cdg")};
    QVERIFY(testmedia::writeCdg(pair.cdgPath, testcdg::stream(450, {
        {50, testcdg::instruction(63, {})},
        {150, testcdg::instruction(63, {})},
        {250, testcdg::instruction(63, {})},
    })));
    PositionPlayer player;
    QVERIFY(player.load(pair));
    player.play();
    QTest::failOnWarning(QRegularExpression("Ignoring malformed or unsupported CDG instructions"));
    QTest::ignoreMessage(QtWarningMsg, "Ignoring malformed or unsupported CDG instructions; total will be logged when the song ends");
    for (qint64 ms : {200, 600, 900, 400, 950}) {
        player.reportedPosition = ms;
        player.tick();
    }
    QCOMPARE(player.decoder().skippedInstructions(), 3u);
    QTest::ignoreMessage(QtInfoMsg, "Total ignored CDG instructions: 3");
    if (ending == "stop") {
        player.stop();
        player.stop();
    } else if (ending == "finish") {
        QTRY_COMPARE_WITH_TIMEOUT(player.state(), State::Finished, 4000);
    } else if (ending == "replace") {
        QVERIFY(player.load(m_song));
    }
    // The destroy row flushes the total in the player's destructor.
}

void TestKaraokePlayer::externalMediaSmokeTest()
{
    // Optional: set FKS_TEST_MEDIA_DIR to a folder containing a real pair.
    const QString dir = qEnvironmentVariable("FKS_TEST_MEDIA_DIR");
    if (dir.isEmpty())
        QSKIP("FKS_TEST_MEDIA_DIR not set");
    if (!QDir(dir).exists())
        QSKIP("FKS_TEST_MEDIA_DIR does not exist");
    const QStringList mp3s = QDir(dir).entryList({"*.mp3", "*.MP3"}, QDir::Files);
    if (mp3s.isEmpty())
        QSKIP("No MP3 in FKS_TEST_MEDIA_DIR");

    const SongPairResult resolved = resolveSongPair(QDir(dir).filePath(mp3s.first()));
    QVERIFY2(resolved.pair.isValid(), qPrintable(resolved.error));

    KaraokePlayer player(nullptr, "fakesink");
    QVERIFY(player.load(resolved.pair));
    player.play();
    const auto startRevision = player.decoder().revision();
    for (int i = 0; i < 30; ++i) {
        QTest::qWait(100);
        player.tick();
        QCOMPARE(player.state(), State::Playing);
        verifyLyricsMatchAudio(player);
    }
    QVERIFY(player.positionMs() > 2000);
    QVERIFY(player.durationMs() > 10'000);
    QVERIFY(player.decoder().revision() != startRevision);  // real CDG drew something
    qInfo() << "External pair played to" << player.positionMs() << "ms of" << player.durationMs()
            << "ms; CDG packets applied" << player.decoder().packetsApplied();

    player.pause();
    const qint64 pausedAt = player.positionMs();
    QTest::qWait(500);
    player.tick();
    QCOMPARE(player.positionMs(), pausedAt);
    player.stop();
    QCOMPARE(player.decoder().packetsApplied(), 0u);
}

QTEST_GUILESS_MAIN(TestKaraokePlayer)
#include "tst_karaokeplayer.moc"
