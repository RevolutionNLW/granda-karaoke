#pragma once

#include "playlist/PlaylistTypes.h"
#include "library/MetadataOverrideStore.h"
#include "library/UserStateStore.h"

#include <QList>
#include <QSqlDatabase>
#include <QString>
#include <QVariantMap>

#include <functional>
#include <optional>

struct CatalogueRoot {
    qint64 id = 0;
    QString path;
    bool online = false;
    bool active = false;
    // When the last full scan of this folder finished (ms since epoch, 0 if never).
    qint64 lastScanCompleted = 0;
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

// How well the library's songs are named, in one pass.
struct ReviewSummary {
    qint64 all = 0;
    qint64 high = 0;
    qint64 medium = 0;
    qint64 low = 0;
    qint64 unresolved = 0;
    qint64 conflicts = 0;
    qint64 manual = 0;
};

// A song's original key: the one chosen by hand, if any, and the one worked
// out from its audio (see library/SongKeys.h). Both are keys of the backing
// track itself, never the Key +/- transpose.
struct SongKeyInfo {
    // From the audio (empty status: not analysed yet).
    QString status;      // "confident", "uncertain", "silent", "too_short", "not_audio"
    int keyIndex = -1;   // 0-23 (music::MusicalKey::index), -1 for none
    double confidence = 0.0;
    // Chosen by the user (trusted metadata), -1 for none. Always wins.
    int manualKeyIndex = -1;

    bool isManual() const { return manualKeyIndex >= 0; }
    // A detected key is shown only when confident.
    int detectedKeyIndex() const
    {
        return status == QLatin1String("confident") && keyIndex >= 0 ? keyIndex : -1;
    }
    // The key to show: the user's, else a confident detected one, else none.
    int shownKeyIndex() const { return isManual() ? manualKeyIndex : detectedKeyIndex(); }
    bool shown() const { return shownKeyIndex() >= 0; }
};

// How far key analysis has got in the active music folder: MP3s of playable
// songs, those analysed by the current version, and those with a shown key.
struct SongKeySummary {
    qint64 total = 0;
    qint64 analysed = 0;
    qint64 confident = 0;
    qint64 manual = 0;  // songs with an original key chosen by hand

    qint64 remaining() const { return total > analysed ? total - analysed : 0; }
};

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

    // Mirrors the trusted values into the catalogue. A value stored for a
    // song in another folder than the active one goes to the same song in the
    // active folder when it is found there safely: `copyToSong` first copies
    // its store row to that song's identity (if it returns false the value
    // stays with the song at its old path). One value per song: the song's
    // own wins, else the newest moved one. After a successful sync,
    // `replaced` lists the store rows that have been copied or superseded,
    // for removal.
    using CopyOverride = std::function<bool(const MovedMetadataOverride&)>;
    bool applyManualOverrides(const QList<MetadataOverride>& overrides,
                              QString* error = nullptr,
                              const CopyOverride& copyToSong = {},
                              QList<MetadataOverride>* replaced = nullptr);
    // Stored values saved at another folder's path that would follow this
    // song (in the active folder) on the next sync: older copies of its
    // correction, to be removed with it when it is cleared.
    QList<MetadataOverride> movedCopiesOf(qint64 songId, const QList<MetadataOverride>& stored,
                                          QString* error = nullptr) const;
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
    // The review counts of a catalogue file in one query, on a read-only
    // connection of its own: safe to call on a worker thread.
    static std::optional<ReviewSummary> readReviewSummary(const QString& databasePath,
                                                          QString* error = nullptr);
    // Everything known about one song: display, automatic and manual layers,
    // raw file/folder/tag data and the evidence behind the automatic result.
    QVariantMap reviewDetail(qint64 songId, QString* error = nullptr) const;
    bool hasSongs(QString* error = nullptr) const;
    // The key of the MP3 the song would play (its preferred source first),
    // if one has been worked out.
    std::optional<SongKeyInfo> songKey(qint64 songId, QString* error = nullptr) const;
    // Key-analysis progress of a catalogue file, on a read-only connection
    // of its own: safe to call on a worker thread.
    static std::optional<SongKeySummary> readSongKeySummary(const QString& databasePath,
                                                            QString* error = nullptr);

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
    // The same, for deciding where the program may write: on Windows, a path
    // that exists but that Windows will not identify is taken to be inside.
    static bool mayBeInsideOrEqual(const QString& candidate, const QString& root);
    static bool storageIsSafe(const QString& databasePath, const QString& cacheDirectory,
                              const QStringList& libraryRoots, QString* error = nullptr);

private:
    friend class CatalogueResolverTestAccess;
    friend class CatalogueTools;
    friend class LibraryScanner;
    friend class MetadataResolver;

    static QString playlistSongLookupSql();
    // The one song in the active folder at this value's path, if its
    // automatic disc/track (or title) shows it is the same song; else 0.
    qint64 movedActiveSongFor(const MetadataOverride& value, QString* error) const;
    // The MP3s key analysis covers (see SongKeySummary), counted on any
    // connection that has the enrichment cache attached as "enrich".
    static std::optional<SongKeySummary> songKeySummaryOn(const QSqlDatabase& database,
                                                          QString* error);

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
