#pragma once

#include "playlist/PlaylistTypes.h"

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
};

struct PlaybackPaths {
    QString mp3Path;
    QString graphicsPath;
    QString reason;

    bool playable() const { return !mp3Path.isEmpty() && !graphicsPath.isEmpty(); }
};

class Catalogue {
public:
    static constexpr int SchemaVersion = 4;

    explicit Catalogue(QString databasePath, QString cacheDirectory = {});
    ~Catalogue();
    Catalogue(const Catalogue&) = delete;
    Catalogue& operator=(const Catalogue&) = delete;

    bool open(QString* error = nullptr, const QStringList& libraryRoots = {});
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
    QList<CatalogueSearchRow> searchActive(const QString& text, int limit = 100,
                                           QString* error = nullptr) const;
    QList<CatalogueSearchRow> browseActive(QString* error = nullptr) const;
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
    bool execute(const QString& sql, QString* error = nullptr) const;
    bool recoverCorruptDatabase(const QString& detail, QString* error);
    QList<CatalogueSearchRow> searchImpl(const QString& text, int limit,
                                         bool includeUnplayable, bool activeOnly,
                                         QString* error) const;
    PlaybackPaths playbackPathsForImpl(qint64 songId, bool activeOnly,
                                       QString* error) const;
    void setError(const QString& message, QString* error) const;
    QSqlDatabase database() const { return m_database; }

    QString m_databasePath;
    QString m_cacheDirectory;
    QString m_connectionName;
    mutable QString m_lastError;
    QSqlDatabase m_database;
};
