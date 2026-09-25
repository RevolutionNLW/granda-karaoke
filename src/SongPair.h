#pragma once

#include <QString>

// A matching MP3 + CDG karaoke pair on disk.
struct SongPair {
    QString mp3Path;
    QString cdgPath;

    // File name without extension, used as the song's display name.
    QString displayName() const;
    bool isValid() const { return !mp3Path.isEmpty() && !cdgPath.isEmpty(); }
};

struct SongPairResult {
    SongPair pair;
    QString error;  // User-facing explanation when pair is not valid.
};

// Given either the .mp3 or the .cdg of a pair, finds its companion in the same
// folder (same base name, extension matched case-insensitively) and checks that
// both files can be read.
SongPairResult resolveSongPair(const QString& selectedPath);
