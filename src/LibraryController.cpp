#include "LibraryController.h"

#include "BackgroundWork.h"
#include "SongKeyAnalyser.h"

#include "library/CatalogueTools.h"
#include "library/FilenameParser.h"
#include "library/LibraryScanner.h"
#include "ocr/PlatformTitleScreenOcr.h"
#include "library/MetadataResolver.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMutexLocker>
#include <QDateTime>
#include <QLoggingCategory>
#include <QPointer>
#include <QTimer>

#include <utility>

namespace {
Q_LOGGING_CATEGORY(lcLibraryController, "fks.library.controller")

const QString kSortKey = QStringLiteral("library_sort");

// Stored names of the library sorts; unknown values fall back to Artist A-Z.
const std::pair<LibrarySort, const char*> kSortNames[] = {
    {LibrarySort::ArtistAsc, "artist_asc"},
    {LibrarySort::ArtistDesc, "artist_desc"},
    {LibrarySort::TitleAsc, "title_asc"},
    {LibrarySort::TitleDesc, "title_desc"},
    {LibrarySort::MostPlayed, "most_played"},
    {LibrarySort::RecentlyPlayed, "recently_played"},
    {LibrarySort::LabelAsc, "label_asc"},
};

bool dependsOnPlays(LibrarySort sort)
{
    return sort == LibrarySort::MostPlayed || sort == LibrarySort::RecentlyPlayed;
}
}

LibraryController::LibraryController(const QString& databasePath,
                                     const QString& cacheDirectory,
                                     const QString& overrideStorePath,
                                     QObject* parent,
                                     QStringList knownRoots,
                                     const QString& userStatePath)
    : QObject(parent)
    , m_catalogue(databasePath, cacheDirectory)
    , m_knownRoots(std::move(knownRoots))
{
    // This is the application's own catalogue, so a damaged copy may be set
    // aside - but only once it is proved to lie outside every known root.
    m_catalogue.setCorruptionRecoveryAllowed(true);
    if (!m_catalogue.open(&m_openError, m_knownRoots)) {
        qCCritical(lcLibraryController).noquote()
            << "Song library catalogue is unavailable:" << m_openError;
        return;
    }

    if (!userStatePath.isEmpty()) {
        m_userState = std::make_unique<UserStateStore>(userStatePath);
        QString stateError;
        if (!m_userState->open(&stateError, libraryRoots())) {
            qCCritical(lcLibraryController).noquote()
                << "Play history and preferences are unavailable:" << stateError;
            m_userState.reset();
        }
    }
    QString storedSort = m_userState ? m_userState->preference(kSortKey) : QString();
    if (storedSort.isEmpty()) {
        // Adopt an order saved in the catalogue by an earlier test build.
        storedSort = m_catalogue.catalogueMeta(kSortKey);
        if (!storedSort.isEmpty() && m_userState)
            m_userState->setPreference(kSortKey, storedSort);
    }
    for (const auto& [sort, name] : kSortNames) {
        if (storedSort == QLatin1String(name))
            m_sort = sort;
    }
    // The catalogue's sortable copy of the play history is derived: bring it
    // up to date for every song still where it was (no song file is read;
    // moved songs are re-found by the next scan).
    if (m_userState) {
        QMutexLocker lock(&UserStateStore::synchronisation());
        QString historyError;
        const QList<PlayHistoryEntry> history = m_userState->playHistory(&historyError);
        if (!historyError.isEmpty() || !m_catalogue.rebuildPlayProjection(history, nullptr, &historyError))
            qCWarning(lcLibraryController).noquote() << "Play history was not applied:" << historyError;
    }

    if (!overrideStorePath.isEmpty()) {
        m_overrideStore = std::make_unique<MetadataOverrideStore>(overrideStorePath);
        QString overrideError;
        if (!m_overrideStore->open(&overrideError, libraryRoots())) {
            qCCritical(lcLibraryController).noquote()
                << "Metadata overrides are unavailable:" << overrideError;
        } else if (m_overrideStore->all(&overrideError).isEmpty() && overrideError.isEmpty()
                   && m_catalogue.hasTrustedMirror()) {
            // The store was damaged (and set aside) or lost, but the catalogue
            // still mirrors every correction: rebuild the store from it rather
            // than letting an empty store erase them.
            int restored = 0;
            for (const MetadataOverride& value : m_catalogue.trustedMirror()) {
                if (m_overrideStore->setOverride(value, &overrideError))
                    ++restored;
            }
            qCWarning(lcLibraryController).noquote()
                << "Metadata override store was empty or damaged"
                << (m_overrideStore->recoveredFromCorruption() ? "(damaged copy kept aside);" : ";")
                << "restored" << restored << "corrections from the catalogue";
        }
    }

    m_scanner = new LibraryScanner(databasePath, cacheDirectory, overrideStorePath);
    if (m_userState)
        m_scanner->setUserStatePath(m_userState->databasePath());
    m_scanner->setKnownRoots(libraryRoots());
    // Title screens are only read by an explicit maintenance reprocess.
    std::shared_ptr<TitleScreenOcrEngine> ocr = createPlatformTitleScreenOcr();
    m_titleScreenOcrAvailable = ocr != nullptr;
    m_scanner->setTitleScreenOcr(std::move(ocr));
    m_scanner->moveToThread(&m_scannerThread);
    connect(this, &LibraryController::scanRequested,
            m_scanner, &LibraryScanner::scan, Qt::QueuedConnection);
    connect(this, &LibraryController::metadataReprocessRequested,
            m_scanner, &LibraryScanner::reprocessMetadata, Qt::QueuedConnection);
    connect(m_scanner, &LibraryScanner::progress,
            this, &LibraryController::onProgress);
    connect(m_scanner, &LibraryScanner::libraryReady, this, [this] {
        invalidateBrowseCache();
        m_ready = true;
        emit libraryReady();
        emit stateChanged();
    });
    connect(m_scanner, &LibraryScanner::finished,
            this, &LibraryController::onFinished);
    connect(m_scanner, &LibraryScanner::failed,
            this, &LibraryController::onFailed);
    connect(&m_scannerThread, &QThread::finished,
            m_scanner, &QObject::deleteLater);
    m_scannerThread.setObjectName(QStringLiteral("LibraryScanner"));
    m_scannerThread.start();

    // Key analysis runs on the same worker, between its other jobs.
    connect(m_scanner, &LibraryScanner::songKeyBatchFinished,
            this, &LibraryController::onKeyBatchFinished);
    m_keyTimer = new QTimer(this);
    m_keyTimer->setSingleShot(true);
    connect(m_keyTimer, &QTimer::timeout, this, &LibraryController::startKeyBatch);

    // A catalogue written by older resolver rules, or an interrupted reprocess,
    // is brought up to date in the background. It only reads the database, so
    // it also runs while the music drive is disconnected.
    QString metaError;
    const bool hasSongs = m_catalogue.hasSongs(&metaError);
    const int storedVersion = m_catalogue.catalogueMeta(
        QStringLiteral("resolver_version"), &metaError).toInt();
    const bool pending = m_catalogue.catalogueMeta(
        QStringLiteral("reprocess_pending"), &metaError) == QLatin1String("1");
    const int parserVersion = m_catalogue.catalogueMeta(
        QStringLiteral("parser_version"), &metaError).toInt();
    if (metaError.isEmpty() && hasSongs
        && (storedVersion < MetadataResolver::Version || pending
            || parserVersion < kFilenameParserVersion)) {
        QTimer::singleShot(0, this, &LibraryController::requestMetadataReprocess);
    }
}

LibraryController::~LibraryController()
{
    // A count still running is not waited for: it finishes on its own and
    // its result is dropped (main() waits for it before Qt shuts down).
    //
    // The scanner's thread is never destroyed while it runs and never
    // terminated (that would abandon locks and half-done work). The program
    // stops it with stopScanner() before destroying anything; waiting here
    // covers any other owner.
    if (!stopScanner(5000)) {
        qCCritical(lcLibraryController) << "Library scanner has not stopped; still waiting for it";
        while (!m_scannerThread.wait(5000))
            qCCritical(lcLibraryController) << "Still waiting for the library scanner to stop";
    }
    flushPendingWrites(true);  // the scan has stopped: write anything still waiting
    m_catalogue.close();
}

bool LibraryController::stopScanner(int timeoutMs)
{
    if (m_keyTimer)
        m_keyTimer->stop();
    if (m_scanner) {
        // Asked while its thread still runs its event loop, so the scanner
        // cannot have been deleted yet. It is deleted on its own thread as
        // that thread ends; from here on nothing uses it or queues work.
        m_scanner->setKeyYield(true);
        m_scanner->requestCancel();
        m_scanner = nullptr;
        m_scannerThread.quit();
    }
    return m_scannerThread.wait(QDeadlineTimer(timeoutMs));
}

void LibraryController::runOnScannerThreadForTesting(std::function<void()> work)
{
    if (m_scanner)
        QMetaObject::invokeMethod(m_scanner, std::move(work), Qt::QueuedConnection);
}

bool LibraryController::hasActiveRoot() const
{
    return isAvailable() && activeRoot().id != 0;
}

CatalogueRoot LibraryController::activeRoot() const
{
    if (!isAvailable())
        return {};
    return m_catalogue.activeRoot();
}

bool LibraryController::isRootConnected() const
{
    const CatalogueRoot root = activeRoot();
    return root.id != 0 && QFileInfo(root.path).isDir();
}

bool LibraryController::scannerPaused() const
{
    return m_scanner && m_scanner->isPaused();
}

QString LibraryController::statusText() const
{
    if (!isAvailable())
        return QStringLiteral("Song library is unavailable");
    if (!hasActiveRoot())
        return QString();
    if (!isRootConnected())
        return QStringLiteral("Music drive not connected");
    QString error;
    if (m_songCount < 0) {
        m_songCount = m_catalogue.activeSongCount(&error);
        if (!error.isEmpty()) {
            m_songCount = -1;
            qCWarning(lcLibraryController).noquote() << error;
            return QStringLiteral("Song library is unavailable");
        }
    }
    const qint64 count = m_songCount;
    // Progress is only shown while there is nothing to search yet. Background
    // rescans and tag reading stay quiet once songs can be found.
    if (m_scanning && !m_ready && count == 0) {
        if (m_phase == QLatin1String("walk"))
            return QStringLiteral("Finding songs... %L1 files checked").arg(m_done);
        return QStringLiteral("Finding songs...");
    }
    return count == 1 ? QStringLiteral("1 song")
                      : QStringLiteral("%L1 songs").arg(count);
}

QList<CatalogueSearchRow> LibraryController::search(const QString& text, int limit,
                                                    QString* error) const
{
    if (!isAvailable()) {
        if (error)
            *error = m_openError;
        return {};
    }
    return m_catalogue.searchActive(text, limit, error, m_sort);
}

void LibraryController::setLibrarySort(LibrarySort sort)
{
    if (sort == m_sort)
        return;
    m_sort = sort;
    invalidateBrowseCache();
    QString error;
    for (const auto& [value, name] : kSortNames) {
        if (value == sort && m_userState && !m_userState->setPreference(kSortKey, QLatin1String(name), &error))
            qCWarning(lcLibraryController).noquote() << "Could not remember the library order:" << error;
    }
}

void LibraryController::recordPlay(qint64 songId, const QString& identity,
                                   const QString& mp3Path, const QString& cdgPath)
{
    if (!isAvailable() || songId <= 0)
        return;
    if (!m_userState || identity.isEmpty()) {
        qCWarning(lcLibraryController) << "Play not recorded: no play history or song identity";
        return;
    }
    // Where the playing files are, in the catalogue's terms. An incomplete
    // location never replaces the one already remembered.
    PlayHistoryEntry where;
    where.identity = identity;
    const QString file = Catalogue::canonicalPath(mp3Path);
    for (const CatalogueRoot& root : m_catalogue.roots()) {
        const QString rootPath = Catalogue::canonicalPath(root.path);
        if (file != rootPath && Catalogue::pathIsInsideOrEqual(file, rootPath)) {
            where.rootPath = root.path;
            where.mp3RelPath = QDir(rootPath).relativeFilePath(file);
            break;
        }
    }
    const QFileInfo mp3(mp3Path);
    const QFileInfo cdg(cdgPath);
    where.mp3Size = mp3.exists() ? mp3.size() : 0;
    where.cdgSize = cdg.exists() ? cdg.size() : 0;
    PlayHistoryEntry updated;
    QString error;
    QMutexLocker lock(&UserStateStore::synchronisation());
    if (!m_userState->recordPlay(where, QDateTime::currentMSecsSinceEpoch(), &updated, &error)) {
        qCWarning(lcLibraryController).noquote() << "Could not record the play:" << error;
        return;
    }
    lock.unlock();
    m_pendingPlays.append(PendingPlay{songId, SongPlayStats{updated.playCount, updated.lastPlayedMs}});
    flushPendingWrites(false);
}

void LibraryController::requestReviewSummary()
{
    if (m_reviewSummary) {
        emit reviewSummaryReady(*m_reviewSummary);
        return;
    }
    m_summaryWanted = true;
    if (!m_summaryRunning)
        startReviewSummary();
}

void LibraryController::startReviewSummary()
{
    if (!isAvailable())
        return;
    m_summaryRunning = true;
    m_summaryWanted = false;
    const quint64 generation = m_summaryGeneration;
    const QString path = m_catalogue.databasePath();
    const ReviewSummaryReader reader = m_summaryReader
        ? m_summaryReader
        : ReviewSummaryReader([](const QString& databasePath) {
              QString error;
              auto summary = Catalogue::readReviewSummary(databasePath, &error);
              if (!summary)
                  qCWarning(lcLibraryController).noquote() << "Could not count song names:" << error;
              return summary;
          });
    // The worker never touches the controller: its result travels through
    // the application object and is dropped if the controller has gone.
    const QPointer<LibraryController> self(this);
    background::run(QStringLiteral("ReviewSummary"), [self, reader, path, generation] {
        const std::optional<ReviewSummary> summary = reader(path);
        if (QCoreApplication* app = QCoreApplication::instance()) {
            QMetaObject::invokeMethod(app, [self, summary, generation] {
                if (self)
                    self->finishReviewSummary(summary, generation);
            }, Qt::QueuedConnection);
        }
    });
}

void LibraryController::finishReviewSummary(const std::optional<ReviewSummary>& summary,
                                            quint64 generation)
{
    m_summaryRunning = false;
    if (generation != m_summaryGeneration) {
        // The catalogue changed while counting: count again if wanted.
        if (m_summaryWanted)
            startReviewSummary();
        return;
    }
    if (summary) {
        m_reviewSummary = summary;
        emit reviewSummaryReady(*summary);
        return;
    }
    emit reviewSummaryFailed();
    if (m_summaryWanted)
        startReviewSummary();
}

QString LibraryController::preference(const QString& key) const
{
    return m_userState ? m_userState->preference(key) : QString();
}

bool LibraryController::setPreference(const QString& key, const QString& value)
{
    QString error;
    if (!m_userState || !m_userState->setPreference(key, value, &error)) {
        qCWarning(lcLibraryController).noquote() << "Could not save setting" << key << error;
        return false;
    }
    return true;
}

bool LibraryController::setPreferences(const QList<QPair<QString, QString>>& values)
{
    QString error;
    if (!m_userState || !m_userState->setPreferences(values, &error)) {
        qCWarning(lcLibraryController).noquote() << "Could not save settings" << error;
        return false;
    }
    return true;
}

QString LibraryController::databasePath() const
{
    return m_catalogue.databasePath();
}

QString LibraryController::userStatePath() const
{
    return m_userState ? m_userState->databasePath() : QString();
}

qint64 LibraryController::lastScanCompletedMs() const
{
    return hasActiveRoot() ? activeRoot().lastScanCompleted : 0;
}

namespace {

bool titleScreenFileAllowed(const QString& path, const QStringList& roots, QString* error)
{
    for (const QString& root : roots) {
        if (Catalogue::mayBeInsideOrEqual(path, root)) {
            if (error)
                *error = QStringLiteral("Files inside the music folder are never written or read "
                                        "here; choose another place.");
            return false;
        }
    }
    return true;
}

} // namespace

bool LibraryController::exportTitleScreens(const QString& path, QString* summary, QString* error)
{
    if (!isAvailable() || m_scanning) {
        if (error)
            *error = QStringLiteral("The library is busy; please try again when it has finished.");
        return false;
    }
    if (!titleScreenFileAllowed(path, libraryRoots(), error))
        return false;
    QString failure;
    const QVariantMap result = CatalogueTools::exportTitleScreens(m_catalogue, path, &failure);
    if (!failure.isEmpty()) {
        if (error)
            *error = failure;
        return false;
    }
    if (summary)
        *summary = QStringLiteral("Saved %L1 title screens.").arg(result.value(QStringLiteral("exported")).toLongLong());
    return true;
}

bool LibraryController::importTitleScreens(const QString& path, QString* summary, QString* error)
{
    if (!isAvailable() || m_scanning) {
        if (error)
            *error = QStringLiteral("The library is busy; please try again when it has finished.");
        return false;
    }
    if (!titleScreenFileAllowed(path, libraryRoots(), error))
        return false;
    QString failure;
    const QVariantMap result = CatalogueTools::importTitleScreens(m_catalogue, path, &failure);
    if (!failure.isEmpty()) {
        if (error)
            *error = failure;
        return false;
    }
    if (summary)
        *summary = QStringLiteral("Read %L1 title screens. Reprocess song names to use them.")
                       .arg(result.value(QStringLiteral("imported")).toLongLong());
    return true;
}

PlayHistoryEntry LibraryController::playHistory(const QString& identity) const
{
    return m_userState ? m_userState->playHistoryFor(identity) : PlayHistoryEntry{};
}

void LibraryController::flushPendingWrites(bool mayWait)
{
    if (!isAvailable() || m_pendingPlays.isEmpty())
        return;
    // During playback the GUI must never wait for a scan's write lock.
    if (!mayWait)
        m_catalogue.setBusyTimeout(0);
    auto busy = [](const QString& error) {
        return error.contains(QLatin1String("locked"), Qt::CaseInsensitive)
            || error.contains(QLatin1String("busy"), Qt::CaseInsensitive);
    };
    bool deferred = false;
    bool playsWritten = false;
    QList<qint64> written;
    while (!m_pendingPlays.isEmpty()) {
        const PendingPlay play = m_pendingPlays.first();
        QString error;
        if (!m_catalogue.setPlayStats(play.songId, play.stats, &error)) {
            if (busy(error)) {
                deferred = true;
                break;
            }
            qCWarning(lcLibraryController).noquote() << "Could not show the play in the library:" << error;
        } else {
            playsWritten = true;
            written.append(play.songId);
        }
        m_pendingPlays.removeFirst();
    }
    if (!mayWait)
        m_catalogue.setBusyTimeout(5000);
    if (playsWritten && dependsOnPlays(m_sort))
        invalidateBrowseCache();
    if (deferred && !mayWait) {
        if (!m_writeRetry) {
            m_writeRetry = new QTimer(this);
            m_writeRetry->setSingleShot(true);
            m_writeRetry->setInterval(2000);
            connect(m_writeRetry, &QTimer::timeout, this, [this] { flushPendingWrites(false); });
        }
        m_writeRetry->start();
    }
    for (const qint64 songId : std::as_const(written))
        emit playStatsChanged(songId);
}

SongPlayStats LibraryController::playStats(qint64 songId) const
{
    return isAvailable() ? m_catalogue.playStats(songId) : SongPlayStats{};
}

qint64 LibraryController::songIdForMp3File(const QString& mp3Path) const
{
    if (!isAvailable())
        return 0;
    const QString file = Catalogue::canonicalPath(mp3Path);
    for (const CatalogueRoot& root : m_catalogue.roots()) {
        if (!Catalogue::pathIsInsideOrEqual(file, root.path) || file == Catalogue::canonicalPath(root.path))
            continue;
        const QString relative = QDir(Catalogue::canonicalPath(root.path)).relativeFilePath(file);
        if (const qint64 songId = m_catalogue.findSongByMp3Path(root.path, relative))
            return songId;
    }
    return 0;
}

QList<CatalogueSearchRow> LibraryController::browse(QString* error) const
{
    if (!isAvailable()) {
        if (error)
            *error = m_openError;
        return {};
    }
    if (error)
        error->clear();
    if (!m_browseCacheValid) {
        QString browseError;
        const QList<CatalogueSearchRow> rows = m_catalogue.browseActive(&browseError, m_sort);
        if (!browseError.isEmpty()) {
            if (error)
                *error = browseError;
            return {};
        }
        m_browseRows = rows;
        m_browseCacheValid = true;
    }
    return m_browseRows;
}

PlaybackPaths LibraryController::playbackPathsFor(qint64 songId, QString* error) const
{
    if (!isAvailable()) {
        if (error)
            *error = m_openError;
        return {};
    }
    return m_catalogue.activePlaybackPathsFor(songId, error);
}

PlaybackPaths LibraryController::playbackPathsForAny(qint64 songId, QString* error) const
{
    if (!isAvailable()) {
        if (error)
            *error = m_openError;
        return {};
    }
    return m_catalogue.playbackPathsFor(songId, error);
}

std::optional<SongRef> LibraryController::songRef(qint64 songId, QString* error) const
{
    if (!isAvailable()) {
        if (error)
            *error = m_openError;
        return std::nullopt;
    }
    return m_catalogue.songRef(songId, error);
}

qint64 LibraryController::findSongByMp3Path(const QString& rootPath,
                                            const QString& relPath,
                                            QString* error) const
{
    if (!isAvailable()) {
        if (error)
            *error = m_openError;
        return 0;
    }
    return m_catalogue.findSongByMp3Path(rootPath, relPath, error);
}

PlaylistSongResolution LibraryController::resolvePlaylistSong(
    const PlaylistEntry& entry, QString* error) const
{
    if (!isAvailable()) {
        if (error)
            *error = m_openError;
        return {};
    }
    if (error)
        error->clear();

    // A relative path is the durable identity. Catalogue song IDs are only a
    // cache because a recovered/rebuilt catalogue may reuse them for another song.
    if (!entry.mp3RelPath.isEmpty()) {
        QString lookupError;
        const qint64 exact = m_catalogue.findSongByMp3Path(
            entry.rootPath, entry.mp3RelPath, &lookupError);
        if (!lookupError.isEmpty()) {
            if (error)
                *error = lookupError;
            return {};
        }
        if (exact != 0)
            return {exact, exact != entry.songId};

        // A disconnected root has no present-file match, but the old ID may
        // still safely identify it when its stored source path is identical.
        const auto storedIdSong = m_catalogue.songRef(entry.songId, &lookupError);
        if (!lookupError.isEmpty()) {
            if (error)
                *error = lookupError;
            return {};
        }
        if (storedIdSong
            && Catalogue::playlistSnapshotPathsMatch(
                entry.rootPath, entry.mp3RelPath,
                storedIdSong->rootPath, storedIdSong->mp3RelPath))
            return {entry.songId, false};

        // The media root may have moved (for example, a new Windows drive
        // letter). Accept a relative-path match only when it identifies one
        // current song in the active root.
        const qint64 moved = m_catalogue.findUniqueActiveSongByMp3Path(
            entry.mp3RelPath, &lookupError);
        if (!lookupError.isEmpty()) {
            if (error)
                *error = lookupError;
            return {};
        }
        if (moved == 0)
            return {};
        const auto candidate = m_catalogue.songRef(moved, &lookupError);
        if (!lookupError.isEmpty()) {
            if (error)
                *error = lookupError;
            return {};
        }
        bool metadataMatches = false;
        if (candidate && !entry.discId.trimmed().isEmpty() && entry.track > 0) {
            metadataMatches = candidate->discId.trimmed().compare(
                                  entry.discId.trimmed(), Qt::CaseInsensitive) == 0
                && candidate->track == entry.track;
        } else if (candidate && !entry.title.trimmed().isEmpty()
                   && !candidate->title.trimmed().isEmpty()) {
            metadataMatches = candidate->title.trimmed().compare(
                                  entry.title.trimmed(), Qt::CaseInsensitive) == 0;
        }
        return metadataMatches
            ? PlaylistSongResolution{moved, moved != entry.songId}
            : PlaylistSongResolution{};
    }

    // Legacy snapshots without a path have no durable identity to verify.
    return m_catalogue.songRef(entry.songId, error)
        ? PlaylistSongResolution{entry.songId, false}
        : PlaylistSongResolution{};
}

QStringList LibraryController::libraryRoots() const
{
    QStringList result = m_knownRoots;
    if (isAvailable()) {
        for (const CatalogueRoot& root : m_catalogue.roots()) {
            if (!result.contains(root.path))
                result.append(root.path);
        }
    }
    return result;
}

bool LibraryController::chooseRoot(const QString& path, QString* error)
{
    if (!isAvailable()) {
        if (error)
            *error = m_openError;
        return false;
    }
    for (const QString& protectedPath : std::as_const(m_protectedStoragePaths)) {
        if (Catalogue::mayBeInsideOrEqual(protectedPath, path)) {
            if (error) {
                *error = QStringLiteral(
                    "Application storage must not be inside library root: %1").arg(path);
            }
            return false;
        }
    }
    qint64 rootId = 0;
    if (!m_catalogue.addRoot(path, &rootId, error)
        || !m_catalogue.setActiveRoot(rootId, error))
        return false;
    invalidateBrowseCache();
    emit catalogueChanged();
    m_rootWasConnected = true;
    emit stateChanged();
    requestSongKeySummary();  // another folder: other songs, other keys
    startScan(m_catalogue.activeRoot().path);
    return true;
}

void LibraryController::startConfiguredScan()
{
    if (!hasActiveRoot()) {
        emit stateChanged();
        return;
    }
    recheckRoot();
}

void LibraryController::recheckRoot()
{
    const bool connected = isRootConnected();
    emit stateChanged();
    if (connected && !m_rootWasConnected)
        startScan(activeRoot().path);
    m_rootWasConnected = connected;
}

void LibraryController::requestRefreshScan()
{
    if (isRootConnected())
        startScan(activeRoot().path);
}

void LibraryController::requestMetadataReprocess()
{
    if (!m_scanner || !isAvailable())
        return;
    m_scanner->setKeyYield(true);  // a key batch in progress ends at once
    if (m_scanning) {
        m_pendingReprocess = true;
        return;
    }
    m_scanning = true;
    m_reprocessing = true;
    m_phase = QStringLiteral("metadata_reprocess");
    m_done = 0;
    m_total = -1;
    m_scanner->prepareScan();
    emit stateChanged();
    emit metadataReprocessRequested();
}

void LibraryController::requestMetadataReprocessWithTitleScreens()
{
    if (!m_scanner || !isAvailable())
        return;
    m_scanner->requestTitleScreensOnce();
    requestMetadataReprocess();
}

QString LibraryController::progressText() const
{
    if (!m_scanning)
        return QString();
    const QString work = m_reprocessing ? QStringLiteral("Reprocessing metadata")
                                        : QStringLiteral("Scanning library");
    if (m_total > 0)
        return QStringLiteral("%1: %2 %L3 of %L4").arg(work, m_phase).arg(m_done).arg(m_total);
    return QStringLiteral("%1: %2").arg(work, m_phase);
}

QList<ReviewRow> LibraryController::reviewList(ReviewFilter filter, const QString& text,
                                               int limit, QString* error) const
{
    if (!isAvailable()) {
        if (error)
            *error = m_openError;
        return {};
    }
    return m_catalogue.reviewList(filter, text, limit, error);
}

qint64 LibraryController::reviewCount(ReviewFilter filter, QString* error) const
{
    if (!isAvailable()) {
        if (error)
            *error = m_openError;
        return 0;
    }
    return m_catalogue.reviewCount(filter, error);
}

QVariantMap LibraryController::reviewDetail(qint64 songId, QString* error) const
{
    if (!isAvailable()) {
        if (error)
            *error = m_openError;
        return {};
    }
    return m_catalogue.reviewDetail(songId, error);
}

bool LibraryController::setManualOverride(qint64 songId,
                                          const std::optional<QString>& artist,
                                          const std::optional<QString>& title,
                                          QString* error)
{
    // A name edit changes only the name: trusted label, series, disc, track
    // and the origin of an import stay exactly as they are.
    QMutexLocker lock(&MetadataOverrideStore::synchronisation());
    MetadataOverride values = existingTrusted(songId);
    values.artist = artist;
    values.title = title;
    return setTrustedMetadataLocked(songId, values, error);
}

MetadataOverride LibraryController::existingTrusted(qint64 songId) const
{
    MetadataOverride values;
    if (!m_overrideStore || !m_overrideStore->isOpen())
        return values;
    const auto key = m_catalogue.metadataOverrideSnapshot(songId);
    if (!key)
        return values;
    const auto stored = m_overrideStore->overrideFor(key->rootPath, key->mp3RelPath);
    if (stored)
        return *stored;
    // The song's folder was chosen again and the next sync has not moved its
    // values here yet: they are the newest copy that would follow it.
    QString error;
    const QList<MetadataOverride> copies =
        m_catalogue.movedCopiesOf(songId, m_overrideStore->all(&error), &error);
    if (!error.isEmpty())
        return values;
    for (const MetadataOverride& copy : copies) {
        if (copy.updatedAt >= values.updatedAt)
            values = copy;
    }
    return values;
}

bool LibraryController::setTrustedMetadata(qint64 songId, const MetadataOverride& values,
                                           QString* error)
{
    QMutexLocker lock(&MetadataOverrideStore::synchronisation());
    return setTrustedMetadataLocked(songId, values, error);
}

bool LibraryController::setTrustedMetadataLocked(qint64 songId, const MetadataOverride& values,
                                                 QString* error)
{
    if (!m_overrideStore || !m_overrideStore->isOpen()) {
        if (error)
            *error = QStringLiteral("Metadata override store is unavailable");
        return false;
    }
    auto value = m_catalogue.metadataOverrideSnapshot(songId, error);
    if (!value)
        return false;
    value->artist = values.artist;
    value->title = values.title;
    value->label = values.label;
    value->series = values.series;
    value->trustedDiscId = values.trustedDiscId;
    value->trustedTrack = values.trustedTrack;
    value->originalKey = values.originalKey;
    value->origin = values.origin;
    value->createdAt = values.createdAt;  // kept by a row already stored
    value->updatedAt = QDateTime::currentMSecsSinceEpoch();
    if (!storeOverrideLocked(songId, *value, &*value, error))
        return false;
    if (!m_catalogue.setTrustedMetadata(songId, *value, value->updatedAt, error))
        return false;
    invalidateBrowseCache();
    emit catalogueChanged();
    return true;
}

bool LibraryController::clearManualOverride(qint64 songId, QString* error)
{
    QMutexLocker lock(&MetadataOverrideStore::synchronisation());
    if (!m_overrideStore || !m_overrideStore->isOpen()) {
        if (error)
            *error = QStringLiteral("Metadata override store is unavailable");
        return false;
    }
    const auto value = m_catalogue.metadataOverrideSnapshot(songId, error);
    if (!value)
        return false;
    // "Use Automatic Name" returns the name to automatic; other trusted
    // values (label, series, disc, track, original key) are kept.
    MetadataOverride remaining = existingTrusted(songId);
    remaining.artist.reset();
    remaining.title.reset();
    if (remaining.hasValues())
        return setTrustedMetadataLocked(songId, remaining, error);
    if (!storeOverrideLocked(songId, *value, nullptr, error))
        return false;
    if (!m_catalogue.clearManualOverride(songId, error))
        return false;
    invalidateBrowseCache();
    emit catalogueChanged();
    return true;
}

bool LibraryController::storeOverrideLocked(qint64 songId, const MetadataOverride& identity,
                                            const MetadataOverride* value, QString* error)
{
    // A copy left behind when a moved correction could not be tidied up
    // would otherwise move onto the song again, or win over its newer
    // values when its old folder is chosen again.
    QString readError;
    const QList<MetadataOverride> stored = m_overrideStore->all(&readError);
    const QList<MetadataOverride> copies = readError.isEmpty()
        ? m_catalogue.movedCopiesOf(songId, stored, &readError) : QList<MetadataOverride>();
    if (!readError.isEmpty()) {
        if (error)
            *error = readError;
        return false;
    }
    return value ? m_overrideStore->setOverrideAndRemoveCopies(*value, copies, error)
                 : m_overrideStore->clearOverrideAndCopies(identity.rootPath, identity.mp3RelPath,
                                                           copies, error);
}

void LibraryController::setPlaybackActive(bool active)
{
    if (m_scanner)
        m_scanner->setPaused(active);
    if (active == m_playbackActive)
        return;
    m_playbackActive = active;
    // Key analysis stops for the song (the scanner's pause ends its batch)
    // and waits a while after it: Autoplay may be about to start the next.
    if (active) {
        if (m_keyTimer)
            m_keyTimer->stop();
    } else {
        scheduleKeyBatch(m_keyTimings.afterPlaybackMs);
    }
    emit songKeySummaryChanged();
}

void LibraryController::startScan(const QString& rootPath)
{
    if (!m_scanner || rootPath.isEmpty())
        return;
    m_scanner->setKeyYield(true);  // a key batch in progress ends at once
    if (m_scanning) {
        if (m_scanningRoot == rootPath)
            return;
        m_pendingRoot = rootPath;
        m_scanner->requestCancel();
        return;
    }
    m_scanning = true;
    m_reprocessing = false;
    m_ready = false;
    m_scanningRoot = rootPath;
    m_phase = QStringLiteral("walk");
    m_done = 0;
    m_total = -1;
    m_scanner->prepareScan();
    emit stateChanged();
    emit scanRequested(rootPath);
}

void LibraryController::onProgress(const QString& phase, qint64 done, qint64 total,
                                   const QString& currentRelPath)
{
    Q_UNUSED(currentRelPath)
    m_phase = phase;
    m_done = done;
    m_total = total;
    emit progressChanged(phase, done, total);
    emit stateChanged();
}

void LibraryController::onFinished(const QVariantMap& summary)
{
    invalidateBrowseCache();
    m_scanning = false;
    m_reprocessing = false;
    m_scanningRoot.clear();
    emit scanFinished(summary);
    emit catalogueChanged();
    emit stateChanged();
    // Songs found, gone or given back their trusted keys: count keys again.
    requestSongKeySummary();
    startPendingWork();
}

void LibraryController::invalidateBrowseCache()
{
    m_browseRows.clear();
    m_browseCacheValid = false;
    m_songCount = -1;
    // Counts made before this change are out of date.
    m_reviewSummary.reset();
    ++m_summaryGeneration;
}

void LibraryController::onFailed(const QString& message)
{
    qCWarning(lcLibraryController).noquote() << "Library scan failed:" << message;
    invalidateBrowseCache();
    m_scanning = false;
    m_reprocessing = false;
    m_scanningRoot.clear();
    emit catalogueChanged();
    emit stateChanged();
    startPendingWork();
}

void LibraryController::startPendingWork()
{
    if (!m_pendingRoot.isEmpty()) {
        const QString root = m_pendingRoot;
        m_pendingRoot.clear();
        QTimer::singleShot(0, this, [this, root] { startScan(root); });
        return;
    }
    if (m_pendingReprocess) {
        m_pendingReprocess = false;
        QTimer::singleShot(0, this, &LibraryController::requestMetadataReprocess);
        return;
    }
    // Songs a scan added (or a drive that came back) may need keys.
    scheduleKeyBatch(m_keyTimings.startMs);
}

void LibraryController::setSongKeyAnalysisEnabled(bool enabled)
{
    if (enabled == m_keysEnabled)
        return;
    m_keysEnabled = enabled;
    qCInfo(lcLibraryController) << "Song key analysis" << (enabled ? "turned on" : "turned off");
    if (enabled) {
        scheduleKeyBatch(m_keyTimings.startMs);
        requestSongKeySummary();
    } else {
        if (m_keyTimer)
            m_keyTimer->stop();
        if (m_scanner && m_keyBatchInFlight)
            m_scanner->setKeyYield(true);
    }
    emit songKeySummaryChanged();
}

bool LibraryController::keyBatchAllowed() const
{
    // Whether the music drive is there is left to the batch (on the worker
    // thread): a drive that stopped answering never holds up the interface.
    return m_keysEnabled && m_scanner && isAvailable() && !m_scanning && !m_keyBatchInFlight
        && m_pendingRoot.isEmpty() && !m_pendingReprocess && !m_playbackActive;
}

void LibraryController::holdSongKeysForSong()
{
    if (!m_keysEnabled || !m_scanner)
        return;
    if (m_keyBatchInFlight)
        m_scanner->setKeyYield(true);
    scheduleKeyBatch(m_keyTimings.afterPlaybackMs);
}

void LibraryController::scheduleKeyBatch(int delayMs)
{
    if (!m_keysEnabled || !m_keyTimer || !m_scanner)
        return;
    // More work may have come (a scan, turned on again): "done" is stale.
    m_keyChainDone = false;
    // Never sooner than a wait already asked for (e.g. after playback).
    if (m_keyTimer->isActive())
        delayMs = std::max(delayMs, m_keyTimer->remainingTime());
    m_keyTimer->start(delayMs);
}

void LibraryController::startKeyBatch()
{
    if (!keyBatchAllowed())
        return;  // whatever holds it up starts the chain again when it ends
    if (!m_keyEngine)
        m_keyEngine = m_keyEngineFactory ? m_keyEngineFactory() : createSongKeyEngine();
    if (!m_keyEngine) {
        qCWarning(lcLibraryController) << "Song keys cannot be worked out here: no audio decoder";
        return;
    }
    m_keyBatchInFlight = true;
    m_keyChainDone = false;
    m_keyUnavailable = false;
    m_scanner->setKeyYield(false);
    LibraryScanner* scanner = m_scanner;
    std::shared_ptr<SongKeyEngine> engine = m_keyEngine;
    QMetaObject::invokeMethod(m_scanner, [scanner, engine] { scanner->analyseSongKeys(engine); },
                              Qt::QueuedConnection);
    emit songKeySummaryChanged();
}

void LibraryController::onKeyBatchFinished(const QVariantMap& summary)
{
    if (!m_scanner)
        return;  // closing
    m_keyBatchInFlight = false;
    const QString reason = summary.value(QStringLiteral("reason")).toString();
    const qint64 decoded = summary.value(QStringLiteral("decoded")).toLongLong();
    const qint64 reused = summary.value(QStringLiteral("reused")).toLongLong();
    const qint64 failed = summary.value(QStringLiteral("failed")).toLongLong();
    const qint64 decodeMs = summary.value(QStringLiteral("decodeMs")).toLongLong();
    if (summary.contains(QStringLiteral("total"))) {
        SongKeySummary counts;
        counts.total = summary.value(QStringLiteral("total")).toLongLong();
        counts.analysed = summary.value(QStringLiteral("analysed")).toLongLong();
        counts.confident = summary.value(QStringLiteral("confident")).toLongLong();
        counts.manual = summary.value(QStringLiteral("manual")).toLongLong();
        m_keySummary = counts;
    }
    m_keyMsTimed += decodeMs;
    m_keySongsTimed += decoded;
    if (decoded + reused + failed > 0) {
        qCInfo(lcLibraryController).noquote()
            << "Song keys:" << decoded << "analysed," << reused << "already known," << failed
            << "unreadable;" << (decoded > 0 ? decodeMs / decoded : 0) << "ms per song;"
            << (m_keySummary ? QStringLiteral("%1 of %2 done").arg(m_keySummary->analysed).arg(m_keySummary->total)
                             : QString())
            << "(" << reason << ")";
    }
    if (decoded + reused > 0)
        emit songKeysChanged();
    emit songKeySummaryChanged();
    m_keyChainDone = reason == QLatin1String("done");
    m_keyUnavailable = reason == QLatin1String("unavailable");
    if (reason == QLatin1String("more"))
        scheduleKeyBatch(m_keyTimings.restMs);
    else if (reason == QLatin1String("stopped"))
        scheduleKeyBatch(m_keyTimings.retryMs);
    // "done", "offline" and "unavailable" (no decoder, or no lasting cache)
    // wait for the next scan to finish; the decoder is then made afresh.
    if (reason == QLatin1String("unavailable"))
        m_keyEngine.reset();
}

void LibraryController::requestSongKeySummary()
{
    if (!isAvailable())
        return;
    if (m_keySummaryRunning) {
        m_keySummaryAgain = true;  // something changed during the count
        return;
    }
    m_keySummaryRunning = true;
    const QString path = m_catalogue.databasePath();
    const QPointer<LibraryController> self(this);
    background::run(QStringLiteral("SongKeySummary"), [self, path] {
        QString error;
        const std::optional<SongKeySummary> summary = Catalogue::readSongKeySummary(path, &error);
        if (!summary)
            qCWarning(lcLibraryController).noquote() << "Could not count song keys:" << error;
        if (QCoreApplication* app = QCoreApplication::instance()) {
            QMetaObject::invokeMethod(app, [self, summary] {
                if (!self)
                    return;
                self->m_keySummaryRunning = false;
                if (summary)
                    self->m_keySummary = summary;
                emit self->songKeySummaryChanged();
                if (std::exchange(self->m_keySummaryAgain, false))
                    self->requestSongKeySummary();
            }, Qt::QueuedConnection);
        }
    });
}

std::optional<SongKeyInfo> LibraryController::songKey(qint64 songId) const
{
    if (!isAvailable() || songId <= 0)
        return std::nullopt;
    QString error;
    std::optional<SongKeyInfo> key = m_catalogue.songKey(songId, &error);
    if (!error.isEmpty())
        qCWarning(lcLibraryController).noquote() << error;
    return key && key->shown() ? key : std::nullopt;
}

std::optional<SongKeyInfo> LibraryController::songKeyDetails(qint64 songId) const
{
    if (!isAvailable() || songId <= 0)
        return std::nullopt;
    QString error;
    std::optional<SongKeyInfo> key = m_catalogue.songKey(songId, &error);
    if (!error.isEmpty())
        qCWarning(lcLibraryController).noquote() << error;
    return key;
}

bool LibraryController::setManualOriginalKey(qint64 songId, std::optional<int> keyIndex,
                                             QString* error)
{
    if (keyIndex && (*keyIndex < 0 || *keyIndex > 23)) {
        if (error)
            *error = QStringLiteral("Not a song key: %1").arg(*keyIndex);
        return false;
    }
    {
        QMutexLocker lock(&MetadataOverrideStore::synchronisation());
        if (!m_overrideStore || !m_overrideStore->isOpen()) {
            if (error)
                *error = QStringLiteral("Metadata override store is unavailable");
            return false;
        }
        auto value = m_catalogue.metadataOverrideSnapshot(songId, error);
        if (!value)
            return false;
        // Only the key changes: names and details the user trusted stay.
        const MetadataOverride existing = existingTrusted(songId);
        value->artist = existing.artist;
        value->title = existing.title;
        value->label = existing.label;
        value->series = existing.series;
        value->trustedDiscId = existing.trustedDiscId;
        value->trustedTrack = existing.trustedTrack;
        value->origin = existing.origin;
        value->createdAt = existing.createdAt;
        value->originalKey = keyIndex;
        value->updatedAt = QDateTime::currentMSecsSinceEpoch();
        if (value->hasValues()) {
            if (!storeOverrideLocked(songId, *value, &*value, error)
                || !m_catalogue.setTrustedMetadata(songId, *value, value->updatedAt, error))
                return false;
        } else if (!storeOverrideLocked(songId, *value, nullptr, error)
                   || !m_catalogue.clearManualOverride(songId, error)) {
            return false;
        }
    }
    qCInfo(lcLibraryController).noquote()
        << "Original key of song" << songId << "set by hand to"
        << (keyIndex ? songKeyName(*keyIndex) : QStringLiteral("none (detected key used)"));
    // The names are unchanged: the library keeps its place; keys are looked up again.
    emit songKeysChanged();
    requestSongKeySummary();
    return true;
}

QString LibraryController::songKeyStatusText() const
{
    if (!m_keySummary)
        return m_keySummaryRunning ? QStringLiteral("Counting songs...") : QString();
    const SongKeySummary& counts = *m_keySummary;
    if (counts.total == 0)
        return QStringLiteral("No songs to analyse yet.");
    QString found = QStringLiteral("%L1 of %L2 songs analysed; keys shown for %L3.")
                        .arg(counts.analysed).arg(counts.total).arg(counts.confident);
    if (counts.manual > 0)
        found += QStringLiteral(" %L1 set by hand.").arg(counts.manual);
    if (!m_keysEnabled)
        return found;
    if (counts.remaining() == 0)
        return found + QStringLiteral(" All done.");
    if (m_keyUnavailable && !m_keyBatchInFlight) {
        return found + QStringLiteral(" Stopped: song keys cannot be worked out just now (see "
                                      "the log). It tries again after the next library scan.");
    }
    if (m_keyChainDone && !m_keyBatchInFlight) {
        return found + QStringLiteral(" Done for now: %L1 could not be read and will be tried again "
                                      "next time.").arg(counts.remaining());
    }
    QString doing;
    if (m_playbackActive)
        doing = QStringLiteral("Paused while a song plays.");
    else if (m_scanning)
        doing = QStringLiteral("Waiting for the library scan.");
    else if (!isRootConnected())
        doing = QStringLiteral("Waiting for the music drive.");
    else if (m_keyBatchInFlight)
        doing = QStringLiteral("Working.");
    else
        doing = QStringLiteral("Resting.");
    QString left;
    if (const qint64 perSong = songKeyMsPerSong(); perSong > 0) {
        const qint64 minutes = std::max<qint64>(1, counts.remaining() * perSong / 60000);
        left = minutes >= 120 ? QStringLiteral(" About %L1 hours left.").arg((minutes + 30) / 60)
                              : QStringLiteral(" About %L1 minutes left.").arg(minutes);
    }
    return found + left + QLatin1Char(' ') + doing;
}
