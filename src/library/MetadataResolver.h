#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QtTypes>
#include <QVariant>
#include <QVariantMap>

#include <functional>

class Catalogue;

class MetadataResolver {
public:
    static constexpr int Version = 5;

    struct Options {
        // Consulted between write batches; may block (e.g. while playback
        // pauses background work).
        std::function<bool()> shouldStop;
        // Consulted for every item at every stage; must return at once. When
        // it says stop, resolve() returns Cancelled promptly, having written
        // no partial batch and leaving the reprocess pending.
        std::function<bool()> cancelled;
        // Told each stage's name as it begins ("candidates", "sidecars",
        // "names", "evidence", "base", "duplicates", "title_screens", "write").
        std::function<void(const QString& stage)> stageStarted;
        std::function<void(qint64 done, qint64 total)> progress;
    };

    enum class Status { Completed, Cancelled, Failed };

    // Rebuilds resolved metadata exclusively from parsed_json and raw_tags_json
    // already stored in the catalogue. It never opens a library file.
    static bool resolve(Catalogue& catalogue, qint64 rootId = -1, QString* error = nullptr);
    static Status resolve(Catalogue& catalogue, qint64 rootId,
                          const Options& options, QString* error = nullptr);
    static QVariantMap evaluateTags(Catalogue& catalogue, int mismatchLimit,
                                    QString* error = nullptr);

    // Presentation form of a cleaned artist field ("Reeves, Jim" -> "Jim Reeves"
    // only when the conservative personal-name rule is satisfied).
    static QString displayArtistName(const QString& artist);

    // Everything a song must stay findable by: effective, automatic and manual
    // names plus the raw filename, folders, ID3 values and disc/track forms.
    struct SearchInputs {
        QString effectiveTitle;
        QString effectiveArtist;
        QString autoTitle;
        QString autoArtist;
        QVariant manualTitle;
        QVariant manualArtist;
        QString discId;
        int track = 0;
        QString relDir;
        QJsonObject parsed;
        QJsonObject tags;
        QStringList extra;  // e.g. an unconfirmed title-screen reading
    };
    static QString buildSearchText(const SearchInputs& inputs);

    // SQL condition (on the songs table, optionally aliased) that is true when
    // a song has any trusted manual or imported value.
    static QString hasTrustedSql(const QString& alias = {});
    // SQL condition true when the trusted values name the song itself
    // (a title or an artist), which makes it fully trusted.
    static QString hasTrustedNameSql(const QString& alias = {});
};
