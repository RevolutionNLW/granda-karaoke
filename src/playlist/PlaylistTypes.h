#pragma once

#include <QMetaType>
#include <QString>

struct SongRef {
    qint64 songId = 0;
    QString title;
    QString artist;
    QString discId;
    int track = 0;
    QString rootPath;
    QString mp3RelPath;
};

struct PlaylistInfo {
    qint64 id = 0;
    QString name;
    qint64 createdAt = 0;
    qint64 updatedAt = 0;
    int itemCount = 0;
};

struct PlaylistEntry {
    qint64 itemId = 0;
    qint64 playlistId = 0;
    int position = 0;
    qint64 songId = 0;
    QString title;
    QString artist;
    QString discId;
    int track = 0;
    QString rootPath;
    QString mp3RelPath;
};

Q_DECLARE_METATYPE(PlaylistEntry)
