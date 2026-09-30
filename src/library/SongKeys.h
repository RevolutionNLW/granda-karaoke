#pragma once

#include "music/KeyDetector.h"

#include <QString>

#include <functional>
#include <optional>

// Song keys worked out from the audio (see music/KeyDetector.h). Results are
// app-owned enrichment: they live in the enrichment cache (never in the music
// folder or the song files), keyed by the MP3's audio content, so a moved or
// renamed song keeps its key and a rebuilt catalogue finds it again.

// Raise when the detector changes: songs analysed by an older version are
// analysed again (their old key is shown until then).
inline constexpr int kSongKeyAnalysisVersion = 1;

// Reads and analyses one song file. The implementation decodes with the
// platform's audio components; the scanner only sees this interface.
class SongKeyEngine {
public:
    enum class Outcome {
        Analysed,     // result is filled in (any status)
        NotAudio,     // the file was read but holds no usable audio
        Unreadable,   // the file could not be read (perhaps the drive went away)
                      // or a decoder it needs is missing: tried again next session
        Interrupted,  // stop() asked to stop; nothing is known
        EngineUnavailable,  // the audio components themselves failed: stop trying
    };

    virtual ~SongKeyEngine() = default;
    // Opens the file read-only; writes nothing anywhere. stop() is asked
    // often (every few tens of milliseconds) and ends the work promptly.
    virtual Outcome analyse(const QString& path, const std::function<bool()>& stop,
                            music::KeyAnalysis* result, QString* detail) = 0;
};

// The key as stored (0-23, see music::MusicalKey::index) as the library shows
// it, e.g. "F#m"; empty for none.
QString songKeyName(int keyIndex);
// The key heard with the song transposed by `semitones`, e.g. "D" for C at +2.
QString transposedKeyName(int keyIndex, int semitones);
