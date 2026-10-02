#pragma once

#include "library/Catalogue.h"
#include "library/MetadataOverrideStore.h"

#include <QObject>
#include <QPointer>
#include <QThread>

#include <functional>
#include <optional>

#include <memory>

#include <utility>

class LibraryScanner;
class QTimer;
class SongKeyEngine;

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
    // Closing: asks the background scanner to stop, takes no new work, and
    // waits up to timeoutMs for its thread to end. The scanner notices within
    // milliseconds. True once the thread has ended; false only if it is stuck
    // (e.g. in a read from a music drive that stopped answering), in which
    // case nothing it may still be using must be destroyed. Safe to repeat.
    bool stopScanner(int timeoutMs);
    bool scannerRunning() const { return m_scannerThread.isRunning(); }
    // Tests only: runs `work` on the scanner's thread, as a scan step would.
    void runOnScannerThreadForTesting(std::function<void()> work);
    // Cheap to ask often: the song count is remembered until the catalogue
    // changes (it is asked on every scan progress update).
    QString statusText() const;

    QList<CatalogueSearchRow> search(const QString& text, int limit,
                                     QString* error = nullptr) const;
    QList<CatalogueSearchRow> browse(QString* error = nullptr) const;
    // The main library's order, for browsing and search results alike. A user
    // preference, remembered in the user-state store (not the catalogue).
    LibrarySort librarySort() const { return m_sort; }
    // Application settings, kept in the user-state store beside play history
    // (never in the catalogue). False/empty when there is no user-state store.
    bool hasPreferences() const { return m_userState != nullptr; }
    QString preference(const QString& key) const;
    bool setPreference(const QString& key, const QString& value);
    bool setPreferences(const QList<QPair<QString, QString>>& values);
    QString databasePath() const;
    QString userStatePath() const;
    // When the active music folder was last fully scanned (0 if never).
    qint64 lastScanCompletedMs() const;
    // Title-screen text as a JSON file (for moving it between computers).
    // Refused while the library is being scanned, and for files inside a
    // music folder. Returns a short summary, or false with the reason.
    bool exportTitleScreens(const QString& path, QString* summary, QString* error);
    bool importTitleScreens(const QString& path, QString* summary, QString* error);
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
    // The name-quality counts, worked out on a worker thread (never the
    // interface's): reviewSummaryReady() brings them. Remembered until the
    // catalogue changes; asking again while they are being counted starts
    // nothing new, and a count made before a change is never delivered.
    void requestReviewSummary();
    std::optional<ReviewSummary> cachedReviewSummary() const { return m_reviewSummary; }
    bool isCountingReviewSummary() const { return m_summaryRunning; }
    using ReviewSummaryReader = std::function<std::optional<ReviewSummary>(const QString& databasePath)>;
    void setReviewSummaryReader(ReviewSummaryReader reader) { m_summaryReader = std::move(reader); }
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

    // Background key analysis (see library/SongKeys.h). Off unless the user
    // turns it on: it reads every song's audio from the music drive, which
    // takes hours for a large library. While on it works in short batches
    // between other library work, pauses while a song plays (and for a
    // while after), and carries on in later sessions where it left off.
    void setSongKeyAnalysisEnabled(bool enabled);
    bool songKeyAnalysisEnabled() const { return m_keysEnabled; }
    bool isAnalysingSongKeys() const { return m_keyBatchInFlight; }
    // The last progress counted (by a batch, or requestSongKeySummary()).
    std::optional<SongKeySummary> songKeySummary() const { return m_keySummary; }
    // Counts progress on a worker thread; songKeySummaryChanged() brings it.
    void requestSongKeySummary();
    // Average time to analyse one song this session (0 until measured).
    qint64 songKeyMsPerSong() const
    {
        return m_keySongsTimed > 0 ? m_keyMsTimed / m_keySongsTimed : 0;
    }
    // A song is being opened: key analysis gets off the drive now and waits
    // a while (Play usually follows; playback then keeps it stopped).
    void holdSongKeysForSong();
    // One line for Settings: how far analysis has got, and what it is doing.
    QString songKeyStatusText() const;
    // The song's key if it is known well enough to show (the user's original
    // key first, else a confident detected one).
    std::optional<SongKeyInfo> songKey(qint64 songId) const;
    // Everything known about the song's key, shown or not (for Set Song Key).
    std::optional<SongKeyInfo> songKeyDetails(qint64 songId) const;
    // The original key of the backing track, chosen by the user (0-23, see
    // music::MusicalKey::index), or none to go back to the detected key. It
    // is trusted metadata, kept with the user's other corrections; it never
    // touches the detected key or the song's Key +/- transpose.
    bool setManualOriginalKey(qint64 songId, std::optional<int> keyIndex, QString* error = nullptr);
    // Tests only: the engine used instead of the GStreamer one, and shorter waits.
    using SongKeyEngineFactory = std::function<std::shared_ptr<SongKeyEngine>()>;
    void setSongKeyEngineFactory(SongKeyEngineFactory factory) { m_keyEngineFactory = std::move(factory); }
    struct SongKeyTimings {
        int startMs = 15000;         // after being turned on or the program starting
        int restMs = 2000;           // between batches
        int afterPlaybackMs = 45000; // after a song stops (Autoplay may start the next)
        int retryMs = 60000;         // after a batch was interrupted for another reason
    };
    void setSongKeyTimings(const SongKeyTimings& timings) { m_keyTimings = timings; }

signals:
    // Keys of some songs became known: shown keys should be looked up again.
    void songKeysChanged();
    void songKeySummaryChanged();
    void stateChanged();
    void catalogueChanged();
    void progressChanged(const QString& phase, qint64 done, qint64 total);
    void libraryReady();
    // A song's play count or last-played time is now in the library.
    void playStatsChanged(qint64 songId);
    void reviewSummaryReady(const ReviewSummary& summary);
    // The counts could not be worked out (asking again tries once more).
    void reviewSummaryFailed();
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
    void startReviewSummary();
    void finishReviewSummary(const std::optional<ReviewSummary>& summary, quint64 generation);
    void startScan(const QString& rootPath);
    void startPendingWork();
    void scheduleKeyBatch(int delayMs);
    void startKeyBatch();
    bool keyBatchAllowed() const;
    void onKeyBatchFinished(const QVariantMap& summary);

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
    mutable qint64 m_songCount = -1;  // -1: not known (see statusText)
    std::optional<ReviewSummary> m_reviewSummary;
    quint64 m_summaryGeneration = 0;
    bool m_summaryRunning = false;
    bool m_summaryWanted = false;
    ReviewSummaryReader m_summaryReader;
    bool m_keysEnabled = false;
    bool m_keyBatchInFlight = false;
    bool m_keySummaryRunning = false;
    bool m_keySummaryAgain = false;
    bool m_playbackActive = false;
    QTimer* m_keyTimer = nullptr;
    std::shared_ptr<SongKeyEngine> m_keyEngine;
    SongKeyEngineFactory m_keyEngineFactory;
    SongKeyTimings m_keyTimings;
    std::optional<SongKeySummary> m_keySummary;
    bool m_keyChainDone = false;  // the last batch found nothing left to try
    bool m_keyUnavailable = false;  // the last batch could not work at all
    qint64 m_keyMsTimed = 0;
    qint64 m_keySongsTimed = 0;
};
