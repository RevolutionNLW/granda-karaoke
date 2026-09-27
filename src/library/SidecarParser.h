#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

// A disc track list found beside the songs, such as the "Folder Text File"
// written by MP3+G Toolz ("01. Kpp01-01 Martin, Ricky - Livin' La Vida Loca")
// or a label list ("SF 035 - 01 - Do You Want To Touch Me - Glitter, Gary").
struct SidecarEntry {
    QString discId;
    int track = 0;
    QStringList fields;  // two name fields, in the order the list wrote them
    int line = 0;
};

struct SidecarTrackList {
    bool recognised = false;
    QString reason;  // why a file was not accepted as a track list
    QString discId;
    QList<SidecarEntry> entries;
};

// Accepts a file only when nearly every content line is a disc-track entry
// with two name fields for one disc. Anything else (download notes, forum
// text, song lyrics, spreadsheets) is rejected, never partially trusted.
// Tracks listed twice with different names are dropped as ambiguous.
SidecarTrackList parseTrackListSidecar(const QByteArray& contents);
