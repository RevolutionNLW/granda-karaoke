#pragma once

#include "library/Catalogue.h"
#include "library/MetadataOverrideStore.h"

#include <QObject>
#include <QThread>

#include <memory>

#include <utility>

class LibraryScanner;
class QTimer;

struct PlaylistSongResolution {
    qint64 songId = 0;
    bool updateStoredSongId = false;

    explicit operator bool() const { return songId != 0; }
};

// Owns the GUI-thread catalogue connection and the cooperative scanner worker.
// The database remains useful for searching when the external music drive is absent.
class LibraryController : public QObject {
    Q_OBJECT

public:
    explicit LibraryController(const QString& databasePath,
                               const QString& cacheDirectory = {},
                               const QString& overrideStorePath = {},
                               QObject* parent = nullptr,
                               QStringList knownRoots = {},
                               const QString& userStatePath = {});
    ~LibraryController() override;

    bool isAvailable() const { return m_catalogue.isOpen(); }
    QString openError() const { return m_openError; }
    bool hasActiveRoot() const;
    CatalogueRoot activeRoot() const;
    bool isRootConnected() const;
    bool isScanning() const { return m_scanning; }
    bool scannerPaused() const;
    QString statusText() const;

    QList<CatalogueSearchRow> search(const QString& text, int limit,
                                     QString* error = nullptr) const;
    QList<CatalogueSearchRow> browse(QString* error = nullptr) const;
    // The main library's order, for browsing and search results alike. A user
    // preference, remembered in the user-state store (not the catalogue).
    LibrarySort librarySort() const { return m_sort; }
    void setLibrarySort(LibrarySort sort);
    // Counts one sung play of a song version (never for previews). The play
    // is kept in the user-state store under the song's content identity (the
    // Key/Tempo identity), and projected into the catalogue for sorting.
    // mp3Path/cdgPath are the files actually playing; they locate the song
    // for re-finding it after the catalogue is rebuilt.
    void recordPlay(qint64 songId, const QString& identity, const QString& mp3Path,
                    const QString& cdgPath);
    // The catalogue's (sortable) view of a song's play history.
    SongPlayStats playStats(qint64 songId) const;
    PlayHistoryEntry playHistory(const QString& identity) const;
    // The catalogue song for a song file opened directly, or 0.
    qint64 songIdForMp3File(const QString& mp3Path) const;
    PlaybackPaths playbackPathsFor(qint64 songId, QString* error = nullptr) const;
    PlaybackPaths playbackPathsForAny(qint64 songId, QString* error = nullptr) const;
    std::optional<SongRef> songRef(qint64 songId, QString* error = nullptr) const;
    qint64 findSongByMp3Path(const QString& rootPath, const QString& relPath,
                             QString* error = nullptr) const;
    PlaylistSongResolution resolvePlaylistSong(const PlaylistEntry& entry,
                                               QString* error = nullptr) const;
    // Every library root known to be configured: the catalogue's own list
    // plus roots remembered outside it, so application files are checked
    // against them even when the catalogue itself cannot be read.
    QStringList libraryRoots() const;
    void setProtectedStoragePaths(QStringList paths)
    {
        m_protectedStoragePaths = std::move(paths);
    }

    bool chooseRoot(const QString& path, QString* error = nullptr);
    void startConfiguredScan();
    void recheckRoot();
    void requestRefreshScan();
    void requestMetadataReprocess();
    // A maintenance reprocess that also reads the title screens of songs that
    // stay unresolved (slow; needs the music drive and a local OCR engine).
    void requestMetadataReprocessWithTitleScreens();
    bool titleScreenOcrAvailable() const { return m_titleScreenOcrAvailable; }
    bool isReprocessing() const { return m_reprocessing; }
    QString progressText() const;
    QList<ReviewRow> reviewList(ReviewFilter filter, const QString& text, int limit = 500,
                                QString* error = nullptr) const;
    qint64 reviewCount(ReviewFilter filter, QString* error = nullptr) const;
    QVariantMap reviewDetail(qint64 songId, QString* error = nullptr) const;
    void setPlaybackActive(bool active);
    bool setManualOverride(qint64 songId, const std::optional<QString>& artist,
                           const std::optional<QString>& title,
                           QString* error = nullptr);
    bool clearManualOverride(qint64 songId, QString* error = nullptr);
    // Trusted metadata for a catalogued song: artist, title, label, series,
    // disc and track, with origin "manual" (a correction) or "import" (clean
    // metadata supplied when the song was added). Saved in the user-owned
    // store first, so it survives catalogue rebuilds and every reprocess.
    bool setTrustedMetadata(qint64 songId, const MetadataOverride& values,
                            QString* error = nullptr);
    // The song's current trusted values from the user-owned store (empty if none).
    MetadataOverride existingTrusted(qint64 songId) const;

signals:
    void stateChanged();
    void catalogueChanged();
    void progressChanged(const QString& phase, qint64 done, qint64 total);
    void libraryReady();
    void scanFinished(const QVariantMap& summary);
    void scanRequested(const QString& rootPath);
    void metadataReprocessRequested();

private slots:
    void onProgress(const QString& phase, qint64 done, qint64 total,
                    const QString& currentRelPath);
    void onFinished(const QVariantMap& summary);
    void onFailed(const QString& message);

private:
    void invalidateBrowseCache();
    void startScan(const QString& rootPath);
    void startPendingWork();

    Catalogue m_catalogue;
    mutable QList<CatalogueSearchRow> m_browseRows;
    mutable bool m_browseCacheValid = false;
    LibrarySort m_sort = LibrarySort::ArtistAsc;
    // Plays and the sort choice are written without waiting for a background
    // scan's write lock; anything not yet written is retried, and finally
    // written when the library closes.
    struct PendingPlay {
        qint64 songId = 0;
        SongPlayStats stats;
    };
    QList<PendingPlay> m_pendingPlays;
    QTimer* m_writeRetry = nullptr;
    std::unique_ptr<UserStateStore> m_userState;
    void flushPendingWrites(bool mayWait);
    QString m_openError;
    QStringList m_knownRoots;
    QThread m_scannerThread;
    LibraryScanner* m_scanner = nullptr;
    std::unique_ptr<MetadataOverrideStore> m_overrideStore;
    bool m_scanning = false;
    bool m_ready = false;  // The current scan has made its songs searchable.
    QString m_scanningRoot;
    QString m_pendingRoot;
    bool m_reprocessing = false;
    bool m_titleScreenOcrAvailable = false;
    bool m_pendingReprocess = false;
    QStringList m_protectedStoragePaths;
    QString m_phase;
    qint64 m_done = 0;
    qint64 m_total = -1;
    bool m_rootWasConnected = false;
};
