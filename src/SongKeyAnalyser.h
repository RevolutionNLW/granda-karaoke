#pragma once

#include "library/SongKeys.h"

#include <memory>

// Works out a song's key with GStreamer: the MP3 is decoded (read-only) to
// mono at the detector's rate, fed to music::KeyDetector as it arrives and
// never written anywhere. Uses only elements the player already needs, so
// no extra audio component has to be installed or packaged.
//
// GStreamer must be initialised first (KaraokePlayer::initializeGStreamer).
class GstSongKeyEngine final : public SongKeyEngine {
public:
    // At most this much audio is analysed (the start of anything longer).
    static constexpr int kMaxSeconds = 15 * 60;

    Outcome analyse(const QString& path, const std::function<bool()>& stop,
                    music::KeyAnalysis* result, QString* detail) override;
};

std::shared_ptr<SongKeyEngine> createSongKeyEngine();
