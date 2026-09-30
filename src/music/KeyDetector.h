#pragma once

#include "music/MusicalKey.h"

#include <array>
#include <functional>
#include <optional>
#include <vector>

// Works out the main key of a piece of music from its sound. Plain C++ with
// no Qt or GStreamer dependency: it is given mono samples, already decoded.
//
// Method (the classic one behind most key finders): a pitch-class profile
// ("chroma") is built from the spectral peaks of the whole song, corrected
// for the song's overall tuning, and compared with a template of each of the
// 24 major and minor keys. Every second of music counts the same, however
// loud, so the key heard for longest wins: a spoken or silent intro, a fade
// and a last-chorus key change do not decide it. The song is also judged in
// windows of about 20 seconds to see how consistently it stays in that key.
//
// Automatic key finding is imperfect: the usual mistakes are the relative
// major/minor (C for Am), the key a fifth away, and songs that change key.
// So a key is only reported as Confident when the evidence is clear;
// otherwise the result is Uncertain and should not be shown.
namespace music {

inline constexpr int kKeyAnalysisSampleRate = 11025;

struct KeyAnalysis {
    enum class Status {
        Confident,  // key is the song's main key
        Uncertain,  // a best guess exists (key is set) but is not reliable
        Silent,     // too little sound to judge
        TooShort,   // too little audio to judge
    };
    Status status = Status::TooShort;
    std::optional<MusicalKey> key;        // best match (Confident or Uncertain)
    std::optional<MusicalKey> runnerUp;   // next best, for diagnosis
    double correlation = 0.0;       // best key's match, -1..1
    double runnerUpCorrelation = 0.0;
    double margin = 0.0;            // (best - runner-up) / (1 - runner-up)
    double parallelMargin = 0.0;    // the same, over the best key's other mode (C / Cm)
    double confidence = 0.0;        // 0..1, from match, margin and consistency
    double agreement = 0.0;         // share of windows whose own key is the main key
    int windows = 0;                // windows judged
    double tuningCents = 0.0;       // the song's offset from A = 440 Hz
    double tuningConsistency = 0.0; // 0..1: how closely the notes agree on it
    // The song's pitch-class profile (C..B, summing to 1), kept with the
    // result so the decision can be re-tuned later without decoding again.
    std::array<double, 12> chroma{};
    double seconds = 0.0;           // audio analysed
    double voicedSeconds = 0.0;     // audio loud enough to count
};

const char* statusName(KeyAnalysis::Status status);

// Accumulates audio a block at a time, so a whole song never has to be held.
class KeyDetector {
public:
    KeyDetector();

    // Mono samples at kKeyAnalysisSampleRate, nominally -1..1.
    void addSamples(const float* samples, std::size_t count);
    // Seconds of audio added so far.
    double seconds() const;
    KeyAnalysis finish();

    // The analysis of samples already in memory.
    static KeyAnalysis analyse(const std::vector<float>& samples,
                               const std::function<bool()>& cancelled = {});

private:
    struct Peak {
        float pitch;   // MIDI note number, fractional (A4 = 69 at 440 Hz)
        float weight;
    };
    struct Frame {
        std::vector<Peak> peaks;
        float rms = 0.0F;  // level of the band analysed (0: not counted)
    };

    void processFrame();

    std::vector<float> m_pending;
    std::vector<float> m_window;
    std::vector<Frame> m_frames;
    std::size_t m_totalSamples = 0;
};

} // namespace music
