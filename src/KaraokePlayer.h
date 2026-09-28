#pragma once

#include "SongPair.h"
#include "SongSettings.h"
#include "cdg/CdgDecoder.h"

#include <QElapsedTimer>
#include <QImage>
#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>

typedef struct _GstElement GstElement;
typedef struct _GstMessage GstMessage;

// Plays one MP3+CDG pair.
//
// Audio is played by a GStreamer playbin. The audio playback position reported
// by GStreamer is the only clock: a timer periodically asks GStreamer for the
// position and the CDG decoder is advanced to exactly that point. The timer
// never moves the song timeline forward on its own, so pausing the audio
// automatically holds the lyrics.
class KaraokePlayer : public QObject {
    Q_OBJECT

public:
    enum class State {
        Empty,     // No song loaded.
        Ready,     // Song loaded, at the beginning.
        Playing,
        Paused,
        Stopped,   // Stopped by the user; at the beginning.
        Finished,  // Reached the end; at the beginning.
        Error,     // Playback failed; Play will try again from the beginning.
    };
    Q_ENUM(State)

    // audioSinkName selects the GStreamer audio sink element. Leave empty for
    // the platform default; tests use "fakesink".
    explicit KaraokePlayer(QObject* parent = nullptr, const QString& audioSinkName = {});
    ~KaraokePlayer() override;

    // Initialises GStreamer and checks that the required elements exist.
    // Must be called once before creating a player.
    static bool initializeGStreamer(QString* errorMessage);
    // The GStreamer elements a karaoke song needs, all checked at start-up.
    static QList<const char*> requiredElements();
    // What the user is told when any of them is missing (no plugin names).
    static QString missingAudioComponentsMessage();

    // Validates the lyrics before replacing the current song. Rejected lyrics
    // emit errorOccurred and return false without disturbing playback.
    bool load(const SongPair& pair);

    void play();   // Starts from the beginning, or resumes when paused.
    void pause();
    void stop();   // Stops and resets audio and lyrics to the beginning.
    void setKeySemitones(int semitones);
    void setTempoPercent(int percent);
    // The sound output to use, by the name the system shows for it (see
    // AudioOutputs.h); empty = the system default. A song already loaded
    // keeps its output: the change applies from the next song.
    void setAudioOutput(const QString& name);
    QString audioOutput() const { return m_outputName; }
    // Loudness of the music, 0-100% (100% = the song as recorded).
    void setVolumePercent(int percent);
    int volumePercent() const { return m_volumePercent; }

    State state() const { return m_state; }
    bool hasSong() const { return m_song.isValid(); }
    const SongPair& song() const { return m_song; }
    qint64 positionMs() const { return m_positionMs; }
    qint64 durationMs() const { return m_durationMs; }
    QString lastError() const { return m_lastError; }
    int keySemitones() const { return m_keySemitones; }
    int tempoPercent() const { return m_tempoPercent; }

    // The current lyrics image (300x216).
    QImage currentFrame() const;
    const cdg::CdgDecoder& decoder() const { return m_decoder; }

    // Processes pending GStreamer messages and syncs the lyrics to the audio
    // position. Called by the internal timer; public so tests can drive it.
    void tick();

signals:
    void stateChanged(KaraokePlayer::State state);
    void positionChanged(qint64 positionMs, qint64 durationMs);
    void frameChanged(const QImage& frame);
    // A user-facing description of a failure. Details are in the log.
    void errorOccurred(const QString& message);
    void settingsChanged(int keySemitones, int tempoPercent);

protected:
    // Borrowed pipeline for deterministic bus regression tests; never unref it.
    GstElement* pipeline() const { return m_pipeline; }
    virtual bool queryAudioPosition(qint64& positionMs) const;
    virtual bool performTempoSeek(qint64 positionMs, int tempoPercent);
    virtual bool acceptTempoSeekCompletion();
    static bool hasAutoAudioFakeSink(GstElement* pipeline);

private:
    bool ensurePipeline();
    bool installAudioFilter();
    void destroyPipeline();
    bool setPipelineState(int gstState);
    bool prepareForStart();
    bool beginTempoSeek(qint64 positionMs, bool waitForCompletion);
    bool waitForTempoSeek();
    void requestLiveTempoSeek();
    void serviceLiveTempoSeek();
    void completeTempoSeek();
    void resetActiveTempo();
    void applyKey();
    void handleMessage(GstMessage* message);
    void handleError(GstMessage* message, bool report);
    void discardPendingMessages();
    void checkAudioSink();
    void reportSkippedTotal();
    void syncToAudioPosition();
    void resetToBeginning();
    void setState(State state);
    void fail(const QString& userMessage);
    void emitFrameIfChanged();

    QString m_audioSinkName;
    QString m_outputName;
    bool m_outputChanged = false;
    int m_volumePercent = 100;
    GstElement* m_pipeline = nullptr;
    GstElement* m_pitchElement = nullptr; // Borrowed from the audio-filter bin.
    QTimer m_timer;
    SongPair m_song;
    cdg::CdgDecoder m_decoder;
    State m_state = State::Empty;
    qint64 m_positionMs = 0;
    qint64 m_durationMs = 0;
    std::uint64_t m_shownRevision = 0;
    std::size_t m_reportedSkipped = 0;
    bool m_skippedWarningLogged = false;
    bool m_errorReported = false;
    QString m_lastError;
    int m_keySemitones = 0;
    int m_tempoPercent = 100;
    int m_appliedTempoPercent = 100;
    int m_seekingTempoPercent = 100;
    qint64 m_lastGoodPositionMs = 0;
    bool m_hasLastGoodPosition = false;
    bool m_tempoSeekInFlight = false;
    bool m_tempoRetryPending = false;
    bool m_tempoRetryWarningLogged = false;
    QElapsedTimer m_tempoRetryTimer;
    QElapsedTimer m_tempoSeekTimer;
};
