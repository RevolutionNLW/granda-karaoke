#include "LibraryController.h"

#include "library/FilenameParser.h"
#include "library/LibraryScanner.h"
#include "ocr/PlatformTitleScreenOcr.h"
#include "library/MetadataResolver.h"

#include <QDir>
#include <QFileInfo>
#include <QMutexLocker>
#include <QDateTime>
#include <QLoggingCategory>
#include <QTimer>

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
    if (m_scanner) {
        m_scanner->requestCancel();
        m_scannerThread.quit();
        if (!m_scannerThread.wait(5000)) {
            qCCritical(lcLibraryController)
                << "Library scanner did not stop within 5 seconds; terminating worker";
            m_scannerThread.terminate();
            m_scannerThread.wait(1000);
        }
    }
    flushPendingWrites(true);  // the scan has stopped: write anything still waiting
    m_catalogue.close();
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
    const qint64 count = m_catalogue.activeSongCount(&error);
    if (!error.isEmpty()) {
        qCWarning(lcLibraryController).noquote() << error;
        return QStringLiteral("Song library is unavailable");
    }
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
        if (Catalogue::pathIsInsideOrEqual(protectedPath, path)) {
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
    MetadataOverride values = existingTrusted(songId);
    values.artist = artist;
    values.title = title;
    return setTrustedMetadata(songId, values, error);
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
    return stored ? *stored : values;
}

bool LibraryController::setTrustedMetadata(qint64 songId, const MetadataOverride& values,
                                           QString* error)
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
    value->artist = values.artist;
    value->title = values.title;
    value->label = values.label;
    value->series = values.series;
    value->trustedDiscId = values.trustedDiscId;
    value->trustedTrack = values.trustedTrack;
    value->origin = values.origin;
    value->updatedAt = QDateTime::currentMSecsSinceEpoch();
    if (!m_overrideStore->setOverride(*value, error))
        return false;
    if (!m_catalogue.setTrustedMetadata(songId, *value, value->updatedAt, error))
        return false;
    invalidateBrowseCache();
    emit catalogueChanged();
    return true;
}

bool LibraryController::clearManualOverride(qint64 songId, QString* error)
{
    if (!m_overrideStore || !m_overrideStore->isOpen()) {
        if (error)
            *error = QStringLiteral("Metadata override store is unavailable");
        return false;
    }
    const auto value = m_catalogue.metadataOverrideSnapshot(songId, error);
    if (!value)
        return false;
    // "Use Automatic Name" returns the name to automatic; other trusted
    // values (label, series, disc, track) are kept.
    MetadataOverride remaining = existingTrusted(songId);
    remaining.artist.reset();
    remaining.title.reset();
    if (remaining.hasValues())
        return setTrustedMetadata(songId, remaining, error);
    QMutexLocker lock(&MetadataOverrideStore::synchronisation());
    if (!m_overrideStore->clearOverride(value->rootPath, value->mp3RelPath, error))
        return false;
    if (!m_catalogue.clearManualOverride(songId, error))
        return false;
    invalidateBrowseCache();
    emit catalogueChanged();
    return true;
}

void LibraryController::setPlaybackActive(bool active)
{
    if (m_scanner)
        m_scanner->setPaused(active);
}

void LibraryController::startScan(const QString& rootPath)
{
    if (!m_scanner || rootPath.isEmpty())
        return;
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
    startPendingWork();
}

void LibraryController::invalidateBrowseCache()
{
    m_browseRows.clear();
    m_browseCacheValid = false;
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
    }
}
