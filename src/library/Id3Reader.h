#pragma once

#include <QByteArray>
#include <QString>

struct Id3Tags {
    QString title;
    QString artist;
    QString album;
    QString albumArtist;
    QString track;
    QString version;
    bool hasV1 = false;
    bool hasV2 = false;

    bool isEmpty() const;
};

Id3Tags readId3Tags(const QString& mp3Path);
Id3Tags parseId3v2(const QByteArray& head);
Id3Tags parseId3v1(const QByteArray& tail128);
bool isPlaceholderTagValue(const QString& value);
