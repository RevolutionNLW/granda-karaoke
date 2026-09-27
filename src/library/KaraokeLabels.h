#pragma once

#include <QString>

// The karaoke label (producer) and series a song comes from, e.g. Sunfly /
// Most Wanted. Kept apart from artist, title, disc and track so that versions
// of one song from different labels can be told apart.
struct KaraokeLabel {
    QString label;
    QString series;
    QString source;  // "disc_prefix", "folder" or empty when unknown
};

// Conservative: a label comes from a disc prefix known to belong to exactly
// one label, or, for songs without a real disc prefix, from a top-level
// folder named after one label. When the two disagree, or the prefix is
// unknown, no label is given.
KaraokeLabel identifyKaraokeLabel(const QString& discPrefix, const QString& relDir);
