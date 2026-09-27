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
    // The two fields came from treating underscores as separators.
    bool underscoreSplit = false;
    // The first underscore directly follows the disc/track digits
    // ("FIK015_06_Westlife_Uptown Girl"): underscores are this name's separator.
    bool underscoreAfterCode = false;
    QString unsplit;
};

// Bumped whenever parseSongName() output changes, so stored parses of the
// raw file names are refreshed by the next metadata reprocess.
inline constexpr int kFilenameParserVersion = 7;

ParsedName parseSongName(const QString& relativeDir, const QString& fileName);
QJsonObject parsedNameJson(const ParsedName& parsed, const QString& rawPath,
                           const QString& rawFileName);
QString parsedKindName(ParsedName::Kind kind);
QString normalizeForSearch(const QString& value);
QString fallbackTitle(const ParsedName& parsed);
