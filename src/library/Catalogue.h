#pragma once

#include "playlist/PlaylistTypes.h"
#include "library/MetadataOverrideStore.h"
#include "library/UserStateStore.h"

#include <QList>
#include <QSqlDatabase>
#include <QString>
#include <QVariantMap>

#include <optional>

struct CatalogueRoot {
    qint64 id = 0;
    QString path;
    bool online = false;
    bool active = false;
};

struct CatalogueSearchRow {
    qint64 songId = 0;
    QString displayTitle;
    QString displayArtist;
    QString discId;
    int track = 0;
    bool playable = false;
    QString confidence;
    // The karaoke label and series, when known (e.g. "Sunfly", "Most Wanted").
    QString label;
    QString series;
};

// Orders of the main song library. Blank artists, titles and labels always
// come after named ones; disc, track and id keep every order stable.
enum class LibrarySort {
    ArtistAsc,
    ArtistDesc,
    TitleAsc,
    TitleDesc,
    MostPlayed,
    RecentlyPlayed,
    LabelAsc,
};

// How often and when one song (one karaoke version) was sung, as projected
// into the catalogue from the durable play history (UserStateStore).
struct SongPlayStats {
    int playCount = 0;
    qint64 lastPlayedMs = 0;
};

// One song in the maintenance review list.
struct ReviewRow {
    qint64 songId = 0;
    QString displayArtist;
    QString displayTitle;
    QString confidence;
    QString source;
    bool conflict = false;
    bool manual = false;
    QString relPath;
    QString label;
};

enum class ReviewFilter { Unresolved, Low, Medium, Conflicts, Manual, All };

struct PlaybackPaths {
    QString mp3Path;
    QString graphicsPath;
    QString reason;

    bool playable() const { return !mp3Path.isEmpty() && !graphicsPath.isEmpty(); }
};

class Catalogue {
public:
    static constexpr int SchemaVersion = 5;

    explicit Catalogue(QString databasePath, QString cacheDirectory = {});
    ~Catalogue();
    Catalogue(const Catalogue&) = delete;
    Catalogue& operator=(const Catalogue&) = delete;

    bool open(QString* error = nullptr, const QStringList& libraryRoots = {});
    // Lets open() set a damaged catalogue aside and start an empty one. Only
    // the application enables this, for its own catalogue in app data; every
    // other caller fails closed and leaves a damaged file untouched.
    void setCorruptionRecoveryAllowed(bool allowed) { m_recoveryAllowed = allowed; }
    // Expensive, reproducible enrichment (raw title-screen readings) is kept
    // in its own file beside the catalogue, attached to every connection as
    // the schema "enrich", so rebuilding the catalogue never discards it.
    QString enrichmentCachePath() const;
    bool enrichmentCacheIsDurable() const { return m_enrichmentDurable; }
    void close();
    bool isOpen() const;
    QString databasePath() const { return m_databasePath; }
    QString lastError() const { return m_lastError; }

    bool addRoot(const QString& path, qint64* id = nullptr, QString* error = nullptr);
    bool setActiveRoot(qint64 rootId, QString* error = nullptr);
    CatalogueRoot activeRoot(QString* error = nullptr) const;
    QList<CatalogueRoot> roots(QString* error = nullptr) const;
    QList<CatalogueSearchRow> search(const QString& text, int limit = 100,
                                     bool includeUnplayable = false,
                                     QString* error = nullptr) const;
    // Without a sort, matches are ranked by how well they fit the text.
    QList<CatalogueSearchRow> searchActive(const QString& text, int limit = 100,
                                           QString* error = nullptr,
                                           std::optional<LibrarySort> sort = std::nullopt) const;
    QList<CatalogueSearchRow> browseActive(QString* error = nullptr,
                                           LibrarySort sort = LibrarySort::ArtistAsc) const;
    // Play statistics for sorting: a derived copy of the durable play history
    // (which lives outside this rebuildable catalogue). Values only grow.
    bool setPlayStats(qint64 songId, const SongPlayStats& stats, QString* error = nullptr);
    SongPlayStats playStats(qint64 songId, QString* error = nullptr) const;
    // Rebuilds the catalogue's copy from the durable history, for every song
    // whose files are where the history last saw them: the same relative path
    // with the same MP3 and CDG sizes, in any root (a drive that moved or was
    // added again is the same files). No file is read. Entries not found are
    // returned in `unmatched`, for re-finding by content.
    bool rebuildPlayProjection(const QList<PlayHistoryEntry>& history,
                               QList<PlayHistoryEntry>* unmatched = nullptr,
                               QString* error = nullptr);
    // Songs in connected roots whose MP3 and CDG sizes match, for re-finding
    // a moved song by content.
    struct PlayCandidate {
        qint64 songId = 0;
        QString rootPath;
        QString mp3RelPath;
        QString cdgRelPath;
    };
    QList<PlayCandidate> playCandidates(qint64 mp3Size, qint64 cdgSize,
                                        QString* error = nullptr) const;
    PlaybackPaths playbackPathsFor(qint64 songId, QString* error = nullptr) const;
    PlaybackPaths activePlaybackPathsFor(qint64 songId, QString* error = nullptr) const;
    std::optional<SongRef> songRef(qint64 songId, QString* error = nullptr) const;
    qint64 findSongByMp3Path(const QString& rootPath, const QString& relPath,
                             QString* error = nullptr) const;
    qint64 findUniqueActiveSongByMp3Path(const QString& relPath,
                                         QString* error = nullptr) const;
    qint64 activeSongCount(QString* error = nullptr) const;
    QVariantMap stats(QString* error = nullptr) const;
    QList<QVariantMap> sample(int count, const QString& confidence = {},
                              QString* error = nullptr) const;
    QVariantMap explain(const QString& relativePath, QString* error = nullptr) const;
    QVariantMap metadataStats(QString* error = nullptr) const;
    QVariantMap compareMetadata(const QString& baselinePath, int exampleLimit = 10,
                                QString* error = nullptr) const;

    bool applyManualOverrides(const QList<MetadataOverride>& overrides,
                              QString* error = nullptr);
    bool setManualOverride(qint64 songId, const std::optional<QString>& artist,
                           const std::optional<QString>& title,
                           qint64 updatedAt = 0, QString* error = nullptr);
    // Trusted values for one song (a correction or an import); unset values
    // keep the automatic ones. Reprocessing never overwrites them.
    bool setTrustedMetadata(qint64 songId, const MetadataOverride& value,
                            qint64 updatedAt = 0, QString* error = nullptr);
    bool clearManualOverride(qint64 songId, QString* error = nullptr);
    // The trusted values mirrored in the catalogue, with their song-file keys,
    // used to re-seed a lost or damaged override store.
    QList<MetadataOverride> trustedMirror(QString* error = nullptr) const;
    bool hasTrustedMirror(QString* error = nullptr) const;
    std::optional<MetadataOverride> metadataOverrideSnapshot(
        qint64 songId, QString* error = nullptr) const;
    QString catalogueMeta(const QString& key, QString* error = nullptr) const;
    bool setCatalogueMeta(const QString& key, const QString& value, QString* error = nullptr);
    // How long a write on this connection waits for another writer (ms).
    void setBusyTimeout(int milliseconds);
    // Maintenance review of automatic metadata (playable songs in the active root).
    QList<ReviewRow> reviewList(ReviewFilter filter, const QString& text, int limit = 500,
                                QString* error = nullptr) const;
    qint64 reviewCount(ReviewFilter filter, QString* error = nullptr) const;
    // Everything known about one song: display, automatic and manual layers,
    // raw file/folder/tag data and the evidence behind the automatic result.
    QVariantMap reviewDetail(qint64 songId, QString* error = nullptr) const;
    bool hasSongs(QString* error = nullptr) const;

    QByteArray computeSha256(qint64 fileId, QString* error = nullptr);
    bool mergeLooseDuplicates(qint64 firstSourceId, qint64 secondSourceId,
                              QString* error = nullptr);

    static QString canonicalPath(const QString& path);
    // Playlist snapshots use the catalogue's exact stored spelling: cleaned
    // paths with '/' separators. Case-only differences are not relinked.
    static QString normalizedPlaylistRelativePath(const QString& path);
    static bool playlistSnapshotPathsMatch(const QString& firstRoot,
                                           const QString& firstRelativePath,
                                           const QString& secondRoot,
                                           const QString& secondRelativePath);
    static bool pathIsInsideOrEqual(const QString& candidate, const QString& root);
    static bool storageIsSafe(const QString& databasePath, const QString& cacheDirectory,
                              const QStringList& libraryRoots, QString* error = nullptr);

private:
    friend class CatalogueResolverTestAccess;
    friend class CatalogueTools;
    friend class LibraryScanner;
    friend class MetadataResolver;

    static QString playlistSongLookupSql();

    bool ensureSchema(QString* error);
    bool backupBeforeV5Migration(int currentVersion, bool existedNonEmpty,
                                 QString* error);
    bool ensureSongRows(qint64 rootId, QString* error);
    bool recomputeEffectiveSong(qint64 songId, QString* error);
    bool execute(const QString& sql, QString* error = nullptr) const;
    bool recoverCorruptDatabase(const QString& detail, QString* error);
    QList<CatalogueSearchRow> searchImpl(const QString& text, int limit,
                                         bool includeUnplayable, bool activeOnly,
                                         QString* error,
                                         std::optional<LibrarySort> sort = std::nullopt) const;
    bool ensureCurrentTables(QString* error);
    void attachEnrichmentCache(const QStringList& libraryRoots);
    QString prepareEnrichmentCache(const QString& path);
    PlaybackPaths playbackPathsForImpl(qint64 songId, bool activeOnly,
                                       QString* error) const;
    void setError(const QString& message, QString* error) const;
    QSqlDatabase database() const { return m_database; }

    QString m_databasePath;
    QString m_cacheDirectory;
    bool m_recoveryAllowed = false;
    bool m_enrichmentDurable = false;
    QString m_connectionName;
    mutable QString m_lastError;
    QSqlDatabase m_database;
};
