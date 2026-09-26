#pragma once

#include <QString>
#include <QStringList>

class QJsonObject;

struct ParsedName {
    QString stem;
    QString cleaned;
    QString discId;
    QString discPrefix;
    int track = 0;
    QStringList fields;
    enum class Kind { DiscTrackFields, TrackFields, Fields, DiscTrackOnly, TrackOnly, NumericOnly, FreeText };
    Kind kind = Kind::FreeText;
    QString discSource;
    QString fallbackFolder;
};

ParsedName parseSongName(const QString& relativeDir, const QString& fileName);
QJsonObject parsedNameJson(const ParsedName& parsed, const QString& rawPath,
                           const QString& rawFileName);
QString parsedKindName(ParsedName::Kind kind);
QString normalizeForSearch(const QString& value);
QString fallbackTitle(const ParsedName& parsed);
