#pragma once

#include <optional>
#include <string>
#include <string_view>

// A song's musical key: a tonic pitch class and a mode (major or minor).
// Plain C++ with no Qt or GStreamer dependency.
//
// This is the key of the music itself, not the Key control in the player bar:
// that one is a transpose in semitones (-2, +1, ...). The key heard while
// singing is the original key transposed by it (see transposed()).
namespace music {

struct MusicalKey {
    int tonic = 0;       // pitch class: 0 = C, 1 = C#/Db, ..., 11 = B
    bool minor = false;

    // 0-11: C..B major; 12-23: C..B minor (the stored form).
    int index() const { return tonic + (minor ? 12 : 0); }
    static std::optional<MusicalKey> fromIndex(int index);

    // The same mode, moved by the given number of semitones (any sign).
    MusicalKey transposed(int semitones) const;

    // One fixed, familiar spelling per key, e.g. "C", "F#m", "Bb", "Ebm".
    // Never B#, E#, Cb or Fb.
    std::string name() const;
    // Accepts the names above and their enharmonic twins ("A#", "Gbm",
    // "C#" ...), with "m", "min" or " minor" for minor keys.
    static std::optional<MusicalKey> parse(std::string_view text);

    friend bool operator==(const MusicalKey&, const MusicalKey&) = default;
};

} // namespace music
