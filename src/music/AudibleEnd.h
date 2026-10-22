#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Where a song's music really ends, from its decoded audio. Many karaoke
// tracks carry 5-15 seconds of silence after the music; Autoplay need not
// wait through it (Smart Autoplay End). Plain C++ with no Qt or GStreamer
// dependency: it is given decoded samples.
//
// Safety comes before saving time. The audio is judged in 50 ms windows by
// its loudest channel, both by level (RMS) and by its loudest sample, so a
// sound that is short or in one channel only still counts. A window is
// silent only when it is at the level of digital silence or an MP3
// encoder's own noise: below -90 dBFS, and 78 dB below the song's own
// loudness, with no sample above -78 dBFS. Any sound above that (however
// quiet: the last of a fade, a soft held note) is kept. The music ends
// after the last window that is not silent; only the silence after that
// can be skipped, so a quiet passage followed by more music, a fade, a soft
// last note or spoken words at the end are all kept. And the song is played
// to its end (PlayToEnd) whenever the result is not clear: a short or very
// quiet recording, little silence to gain, or silence making up much of the
// track. Otherwise Autoplay may move on a second after the music ends
// (endAtMs), a natural pause before the next song.
namespace music {

// Results are cached by this version: a change to the rules below that
// could change a result must raise it.
inline constexpr int kAudibleEndVersion = 1;
// Decoded at this rate, both channels kept (a downmix can cancel a sound
// that is out of phase between them).
inline constexpr int kAudibleEndSampleRate = 44100;
inline constexpr int kAudibleEndChannels = 2;

struct AudibleEnd {
    enum class Verdict {
        Skip,       // Autoplay may move on at endAtMs
        PlayToEnd,  // play the whole file (reason says why)
    };
    Verdict verdict = Verdict::PlayToEnd;
    std::int64_t durationMs = 0;    // the decoded length
    std::int64_t audibleEndMs = 0;  // the end of the last sound (the length if none)
    std::int64_t endAtMs = 0;       // Skip: where Autoplay may move on; else the length
    double referenceDb = -150.0;    // the song's loudness (90th percentile of its windows)
    double silenceDb = -150.0;      // windows quieter than this are silent
    std::string reason;

    bool skips() const { return verdict == Verdict::Skip; }
    // How much sooner Autoplay moves on.
    std::int64_t savedMs() const { return skips() ? durationMs - endAtMs : 0; }
};

class AudibleEndDetector {
public:
    static constexpr int kWindowMs = 50;
    // The pause left after the music before Autoplay moves on.
    static constexpr std::int64_t kGapMs = 1000;
    // Less to gain than this is not worth it: play to the end.
    static constexpr std::int64_t kMinSavedMs = 2000;
    // Shorter recordings always play to their end.
    static constexpr std::int64_t kMinSongMs = 30000;
    // The music must last at least this share of the track.
    static constexpr double kMinMusicShare = 0.5;
    // Silent: below both of these (dBFS, and dB below the song's loudness)...
    static constexpr double kSilenceFloorDb = -90.0;
    static constexpr double kBelowSongDb = 78.0;
    // ...and no sample louder than the silence level plus this.
    static constexpr double kPeakAllowanceDb = 12.0;
    // A recording whose loudness is below this is too quiet to judge.
    static constexpr double kQuietestSongDb = -45.0;

    explicit AudibleEndDetector(int sampleRate = kAudibleEndSampleRate,
                                int channels = kAudibleEndChannels);

    // Interleaved samples, full scale = 1.0; frames = samples per channel.
    void addSamples(const float* interleaved, std::size_t frames);
    AudibleEnd finish() const;

    std::int64_t framesSeen() const { return m_frames; }

private:
    void closeWindow();

    int m_sampleRate;
    int m_channels;
    std::size_t m_windowFrames;
    std::int64_t m_frames = 0;
    std::size_t m_inWindow = 0;
    std::vector<double> m_sumSquares;  // per channel, current window
    float m_peak = 0.0f;               // current window
    std::vector<float> m_rmsDb;        // per finished window (loudest channel)
    std::vector<float> m_peakDb;
};

// "3:05.2" (minutes:seconds.tenths), for logs.
std::string formatTime(std::int64_t ms);

} // namespace music
