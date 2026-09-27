#pragma once

#include <QByteArray>
#include <QString>

// Content fingerprints shared by remembered Key/Tempo settings and duplicate
// identification. They depend only on file content, never on names or paths,
// and every file is opened read-only.

// The Key/Tempo song identity: the complete CDG plus the MP3 audio payload
// (recognised ID3v2/ID3v1/APE metadata excluded, sampled at both ends).
// Returns an empty string if either file cannot be read completely.
QString contentIdentity(const QString& mp3Path, const QString& cdgPath);

// SHA-256 of a complete CDG file, and the number of tile-drawing packets
// (a blank stream, or one made only of presets/palette loads, has none).
inline constexpr int kContentDigestVersion = 3;
bool cdgContentDigest(const QString& path, QByteArray* digest, qint64* graphicsPackets);

// The MP3 audio component of contentIdentity() on its own: equal for copies
// that differ only in their tags.
bool mp3AudioDigest(const QString& path, QByteArray* digest);

// A cheap pre-filter: SHA-256 of the size and the first and last 64 KiB.
// Equal quick digests only nominate a pair for the complete comparison.
bool quickFileDigest(const QString& path, QByteArray* digest);
