#include "library/LibraryScanner.h"

#include "library/Catalogue.h"
#include "library/CatalogueTools.h"
#include "library/ContentIdentity.h"
#include "library/FilenameParser.h"
#include "library/Id3Reader.h"
#include "library/MetadataResolver.h"
#include "library/MetadataOverrideStore.h"
#include "library/SidecarParser.h"
#include "library/TitleScreenText.h"
#include "library/UserStateStore.h"
#include "cdg/CdgDecoder.h"
#include "cdg/CdgTitleFrames.h"
#include "library/ZipDirectory.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QMutexLocker>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>

#include <array>
#include <utility>

namespace {

QString queryError(const QSqlQuery& query, const QString& context)
{
    return QStringLiteral("%1: %2").arg(context, query.lastError().text());
}

QString kindForExtension(const QString& extension)
{
    const QString ext = extension.toLower();
    if (ext == QLatin1String("mp3") || ext == QLatin1String("cdg")
        || ext == QLatin1String("mcg") || ext == QLatin1String("zip"))
        return ext;
    return QStringLiteral("other");
}

bool skippedName(const QString& name)
{
    if (name.startsWith(QLatin1Char('.')))
        return true;
    return name.compare(QStringLiteral("Thumbs.db"), Qt::CaseInsensitive) == 0
        || name.compare(QStringLiteral("desktop.ini"), Qt::CaseInsensitive) == 0
        || name.compare(QStringLiteral("$RECYCLE.BIN"), Qt::CaseInsensitive) == 0
        || name.compare(QStringLiteral("System Volume Information"), Qt::CaseInsensitive) == 0;
}

QString zipStatusName(ZipStatus status)
{
    switch (status) {
    case ZipStatus::Ok: return QStringLiteral("ok");
    case ZipStatus::NotZip: return QStringLiteral("not_zip");
    case ZipStatus::Truncated: return QStringLiteral("zip_damaged");
    case ZipStatus::Corrupt: return QStringLiteral("zip_damaged");
    case ZipStatus::Unreadable: return QStringLiteral("zip_damaged");
    }
    return QStringLiteral("zip_damaged");
}

QString memberKind(const QString& name)
{
    return kindForExtension(QFileInfo(name).suffix());
}

QJsonObject tagsJson(const Id3Tags& tags)
{
    QJsonObject result;
    result.insert(QStringLiteral("title"), tags.title);
    result.insert(QStringLiteral("artist"), tags.artist);
    result.insert(QStringLiteral("album"), tags.album);
    result.insert(QStringLiteral("albumArtist"), tags.albumArtist);
    result.insert(QStringLiteral("track"), tags.track);
    result.insert(QStringLiteral("version"), tags.version);
    result.insert(QStringLiteral("hasV1"), tags.hasV1);
    result.insert(QStringLiteral("hasV2"), tags.hasV2);
    return result;
}

quint32 crc32Update(quint32 crc, const QByteArray& data)
{
    static const std::array<quint32, 256> table = [] {
        std::array<quint32, 256> values{};
        for (quint32 i = 0; i < 256; ++i) {
            quint32 value = i;
            for (int bit = 0; bit < 8; ++bit)
                value = (value & 1U) ? (value >> 1U) ^ 0xedb88320U : value >> 1U;
            values[i] = value;
        }
        return values;
    }();
    for (const uchar byte : data)
        crc = table[(crc ^ byte) & 0xffU] ^ (crc >> 8U);
    return crc;
}

// After a failed read: true (and the scan is flagged offline) when the whole
// library root has gone, e.g. the drive was unplugged. Callers then stop the
// phase without recording anything for the file, so the next scan resumes it.
bool rootGone(const QString& rootPath, QVariantMap& counts)
{
    if (QFileInfo(rootPath).isDir())
        return false;
    counts.insert(QStringLiteral("rootOffline"), true);
    return true;
}

bool readCrc32(const QString& path, const std::atomic_bool& cancelled,
               const std::atomic_bool& paused,
               quint32* crc, QString* error)
{
    while (paused.load() && !cancelled.load())
        QThread::msleep(20);
    if (cancelled.load())
        return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("Could not open source read-only for CRC-32: %1").arg(file.errorString());
        return false;
    }
    quint32 value = 0xffffffffU;
    while (!file.atEnd()) {
        while (paused.load() && !cancelled.load())
            QThread::msleep(20);
        if (cancelled.load())
            return false;
        const QByteArray chunk = file.read(1024 * 1024);
        if (chunk.isEmpty() && file.error() != QFileDevice::NoError) {
            if (error)
                *error = QStringLiteral("Could not read source for CRC-32: %1").arg(file.errorString());
            return false;
        }
        value = crc32Update(value, chunk);
    }
    *crc = value ^ 0xffffffffU;
    return true;
}

struct FileRow {
    qint64 id = 0;
    QString relPath;
    QString relDir;
    QString fileName;
    QString stem;
    QString kind;
    qint64 size = 0;
    bool present = false;
};

constexpr qint64 kMaximumSidecarBytes = 64 * 1024;

bool rootOfflineNowIn(const QVariantMap& counts)
{
    return counts.value(QStringLiteral("rootOffline")).toBool();
}

QString foldedPairKey(const QString& directory, const QString& stem)
{
    return directory.toCaseFolded() + QChar(0x1f) + stem.toCaseFolded();
}

} // namespace

LibraryScanner::LibraryScanner(QString databasePath, QString cacheDirectory,
                               QString overrideStorePath, QObject* parent)
    : QObject(parent)
    , m_databasePath(std::move(databasePath))
    , m_cacheDirectory(std::move(cacheDirectory))
    , m_overrideStorePath(std::move(overrideStorePath))
{
}

bool LibraryScanner::resolveAndSync(Catalogue& catalogue, qint64 rootId,
                                    const QString& phase, QString* error,
                                    bool* cancelled)
{
    MetadataResolver::Options options;
    options.shouldStop = [this] { return !waitWhilePaused(); };
    options.progress = [this, &phase](qint64 done, qint64 total) {
        reportProgress(phase, done, total, QString(), true);
    };
    const MetadataResolver::Status status = MetadataResolver::resolve(
        catalogue, rootId, options, error);
    if (cancelled)
        *cancelled = status == MetadataResolver::Status::Cancelled;
    if (status == MetadataResolver::Status::Failed)
        return false;
    if (status == MetadataResolver::Status::Cancelled)
        return true;
    if (m_overrideStorePath.isEmpty())
        return true;
    // Corrections are user-owned and live outside the catalogue. If their store
    // is unreadable the catalogue keeps the corrections it already mirrors, and
    // the scan itself must not fail because of it.
    QString syncError;
    QStringList roots = m_knownRoots;
    for (const CatalogueRoot& root : catalogue.roots(&syncError))
        roots.append(root.path);
    // The worker never replaces a damaged store: it just leaves the
    // catalogue's mirror alone, and the application re-seeds the store.
    QMutexLocker lock(&MetadataOverrideStore::synchronisation());
    MetadataOverrideStore store(m_overrideStorePath);
    QList<MetadataOverride> overrides;
    if (syncError.isEmpty() && store.open(&syncError, roots, false))
        overrides = store.all(&syncError);
    if (syncError.isEmpty() && overrides.isEmpty() && catalogue.hasTrustedMirror(&syncError))
        syncError = QStringLiteral("the override store is empty but the catalogue holds "
                                   "corrections; keeping them");
    if (!syncError.isEmpty()) {
        qWarning().noquote() << "Manual metadata corrections were not synchronised:"
                             << syncError;
        return true;
    }
    if (!catalogue.applyManualOverrides(overrides, error))
        return false;
    return true;
}

void LibraryScanner::reconcilePlayHistory(Catalogue& catalogue, QVariantMap& counts)
{
    // Play history is user state kept outside the catalogue. Every scan
    // rebuilds the catalogue's copy: first for songs whose files are where
    // they were last seen (no file reads), then, for songs that moved or were
    // renamed, by comparing content identities of same-size candidates. Each
    // candidate is read (read-only) at most once per scan.
    if (m_userStatePath.isEmpty())
        return;
    QString error;
    QStringList roots = m_knownRoots;
    for (const CatalogueRoot& root : catalogue.roots(&error))
        roots.append(root.path);
    UserStateStore store(m_userStatePath);
    QList<PlayHistoryEntry> unmatched;
    {
        QMutexLocker lock(&UserStateStore::synchronisation());
        if (!error.isEmpty() || !store.open(&error, roots, false)) {
            qWarning().noquote() << "Play history was not reconnected:" << error;
            return;
        }
        const QList<PlayHistoryEntry> history = store.playHistory(&error);
        if (!error.isEmpty() || !catalogue.rebuildPlayProjection(history, &unmatched, &error)) {
            qWarning().noquote() << "Play history was not reconnected:" << error;
            return;
        }
    }

    QMap<QPair<qint64, qint64>, QList<PlayHistoryEntry>> bySize;
    for (const PlayHistoryEntry& entry : std::as_const(unmatched)) {
        if (entry.mp3Size > 0 && entry.cdgSize > 0)
            bySize[{entry.mp3Size, entry.cdgSize}].append(entry);
    }
    struct Found {
        PlayHistoryEntry snapshot;
        Catalogue::PlayCandidate candidate;
    };
    QList<Found> found;
    for (auto group = bySize.cbegin(); group != bySize.cend(); ++group) {
        QHash<QString, QList<PlayHistoryEntry>> byIdentity;
        for (const PlayHistoryEntry& entry : group.value())
            byIdentity[entry.identity].append(entry);
        for (const Catalogue::PlayCandidate& candidate :
             catalogue.playCandidates(group.key().first, group.key().second, &error)) {
            if (!waitWhilePaused())
                return;
            const QDir root(candidate.rootPath);
            m_sourceFileReads += 2;
            const QString identity = contentIdentity(root.filePath(candidate.mp3RelPath),
                                                     root.filePath(candidate.cdgRelPath));
            for (const PlayHistoryEntry& entry : byIdentity.value(identity))
                found.append({entry, candidate});
        }
    }

    qint64 relinked = 0;
    QMutexLocker lock(&UserStateStore::synchronisation());
    QSet<QString> located;
    for (const Found& match : std::as_const(found)) {
        // Newer plays may have arrived while files were being read.
        const PlayHistoryEntry current = store.playHistoryFor(match.snapshot.identity, &error);
        if (!error.isEmpty() || current.identity.isEmpty()) {
            error.clear();
            continue;
        }
        // Identical copies share the history, as they share Key/Tempo.
        if (!catalogue.setPlayStats(match.candidate.songId, {current.playCount, current.lastPlayedMs}, &error))
            qWarning().noquote() << "Play history was not reconnected:" << error;
        if (located.contains(current.identity))
            continue;
        located.insert(current.identity);
        ++relinked;
        PlayHistoryEntry moved = current;
        moved.rootPath = match.candidate.rootPath;
        moved.mp3RelPath = match.candidate.mp3RelPath;
        if (!store.updateLocation(moved, match.snapshot, &error))
            qWarning().noquote() << "Play history location was not updated:" << error;
    }
    counts.insert(QStringLiteral("playHistoryRelinked"), relinked);
}

void LibraryScanner::reprocessMetadata()
{
    m_sourceFileReads = 0;
    m_scanTimer.start();
    m_progressTimer.invalidate();
    QVariantMap summary;
    QString error;
    if (!waitWhilePaused()) {
        summary.insert(QStringLiteral("status"), QStringLiteral("cancelled"));
        summary.insert(QStringLiteral("sourceFileReads"), qulonglong(m_sourceFileReads));
        emit finished(summary);
        return;
    }
    Catalogue catalogue(m_databasePath, m_cacheDirectory);
    if (!catalogue.open(&error, m_knownRoots)) {
        emit failed(error);
        return;
    }
    // Stored raw file names are parsed again (database only) when the parser
    // rules have changed since they were last parsed.
    if (catalogue.catalogueMeta(QStringLiteral("parser_version"), &error).toInt()
            < kFilenameParserVersion
        && error.isEmpty()) {
        qint64 reparsed = 0;
        if (!CatalogueTools::reparseStoredNames(catalogue, &reparsed, &error)) {
            emit failed(error);
            return;
        }
        summary.insert(QStringLiteral("reparsedNames"), reparsed);
    }
    if (!error.isEmpty()) {
        emit failed(error);
        return;
    }
    bool cancelled = false;
    if (!resolveAndSync(catalogue, -1, QStringLiteral("metadata_reprocess"),
                        &error, &cancelled)) {
        emit failed(error);
        return;
    }
    // Track lists and content matching read the music drive (read-only). They
    // run for connected roots only, pause during playback and resume from
    // their per-file caches; the cheap resolve above already refreshed the
    // library, so it stays searchable throughout.
    QVariantMap counts;
    bool matched = false;
    if (!cancelled) {
        const QList<CatalogueRoot> roots = catalogue.roots(&error);
        if (!error.isEmpty()) {
            emit failed(error);
            return;
        }
        for (const CatalogueRoot& root : roots) {
            if (shouldStop() || !QFileInfo(root.path).isDir())
                continue;
            counts.remove(QStringLiteral("rootOffline"));
            if (!readSidecars(catalogue, root.id, root.path, counts, &error)) {
                emit failed(error);
                return;
            }
            if (counts.value(QStringLiteral("sidecarsRead")).toLongLong() > 0
                && !resolveAndSync(catalogue, -1, QStringLiteral("metadata_reprocess"),
                                   &error, &cancelled)) {
                emit failed(error);
                return;
            }
            if (cancelled || rootOfflineNowIn(counts))
                break;
            if (!m_options.identifyDuplicates)
                continue;
            if (!identifyDuplicates(catalogue, root.id, root.path, counts, &error)) {
                emit failed(error);
                return;
            }
            matched = true;
        }
        cancelled = shouldStop();
    }
    if (matched && !cancelled) {
        emit libraryReady();
        if (!resolveAndSync(catalogue, -1, QStringLiteral("metadata_reprocess"),
                            &error, &cancelled)) {
            emit failed(error);
            return;
        }
    }
    // Title screens are read only for songs that are still unresolved after
    // every cheaper stage, so this runs after the resolve above.
    bool readScreens = false;
    const bool titleScreens = m_titleScreensRequested.exchange(false) || m_options.readTitleScreens;
    if (!cancelled && titleScreens) {
        const QList<CatalogueRoot> roots = catalogue.roots(&error);
        if (!error.isEmpty()) {
            emit failed(error);
            return;
        }
        for (const CatalogueRoot& root : roots) {
            if (shouldStop() || !QFileInfo(root.path).isDir())
                continue;
            counts.remove(QStringLiteral("rootOffline"));
            if (!readTitleScreens(catalogue, root.id, root.path, counts, &error)) {
                emit failed(error);
                return;
            }
            readScreens = true;
        }
        cancelled = shouldStop();
    }
    if (readScreens && !cancelled) {
        if (!resolveAndSync(catalogue, -1, QStringLiteral("metadata_reprocess"),
                            &error, &cancelled)) {
            emit failed(error);
            return;
        }
        emit libraryReady();
    }
    summary.insert(QStringLiteral("counts"), counts);
    summary.insert(QStringLiteral("status"), cancelled ? QStringLiteral("cancelled")
                                                        : QStringLiteral("completed"));
    summary.insert(QStringLiteral("sourceFileReads"), qulonglong(m_sourceFileReads));
    summary.insert(QStringLiteral("stats"), catalogue.metadataStats(&error));
    if (!error.isEmpty()) {
        emit failed(error);
        return;
    }
    if (!cancelled)
        emit libraryReady();
    emit finished(summary);
}

void LibraryScanner::requestCancel()
{
    m_cancelled.store(true);
}

bool LibraryScanner::shouldStop() const
{
    return m_cancelled.load()
        || (m_options.limitSeconds > 0 && m_scanTimer.elapsed() >= qint64(m_options.limitSeconds) * 1000);
}

bool LibraryScanner::pauseOutsideTransaction(QSqlDatabase& database) const
{
    // Never sleep through playback while holding the write lock: other
    // connections (such as a correction saved from the review screen) must
    // be able to write meanwhile.
    if (m_paused.load() && !m_cancelled.load()) {
        // If the batch cannot be committed, stop rather than sleep holding
        // the lock; if a new batch cannot start, stop rather than continue
        // outside a transaction. Either way the next scan resumes the work.
        if (!database.commit()) {
            qWarning().noquote() << "Library scan stopped: could not commit before pausing:"
                                 << database.lastError().text();
            database.rollback();
            return false;
        }
        const bool resume = waitWhilePaused();
        if (!database.transaction()) {
            qWarning().noquote() << "Library scan stopped: could not resume its batch:"
                                 << database.lastError().text();
            return false;
        }
        return resume;
    }
    return !shouldStop();
}

bool LibraryScanner::waitWhilePaused() const
{
    while (m_paused.load() && !m_cancelled.load())
        QThread::msleep(20);
    return !shouldStop();
}

void LibraryScanner::reportProgress(const QString& phase, qint64 done, qint64 total,
                                    const QString& relPath, bool force)
{
    if (force || !m_progressTimer.isValid() || m_progressTimer.elapsed() >= 100) {
        emit progress(phase, done, total, relPath);
        m_progressTimer.restart();
    }
}

void LibraryScanner::scan(const QString& requestedRoot)
{
    m_sourceFileReads = 0;
    m_scanTimer.start();
    m_progressTimer.invalidate();
    QVariantMap summary;
    QVariantMap counts;
    QVariantMap timings;
    QString error;

    if (!waitWhilePaused()) {
        summary.insert(QStringLiteral("status"), QStringLiteral("cancelled"));
        emit finished(summary);
        return;
    }
    const QString rootPath = Catalogue::canonicalPath(requestedRoot);
    if (!QFileInfo(rootPath).isDir()) {
        emit failed(QStringLiteral("Library root is not a directory: %1").arg(requestedRoot));
        return;
    }
    Catalogue catalogue(m_databasePath, m_cacheDirectory);
    if (!catalogue.open(&error, m_knownRoots + QStringList{rootPath})
        || !catalogue.addRoot(rootPath, nullptr, &error)) {
        emit failed(error);
        return;
    }
    qint64 rootId = 0;
    if (!catalogue.addRoot(rootPath, &rootId, &error)) {
        emit failed(error);
        return;
    }
    QSqlDatabase database = catalogue.database();
    const qint64 started = QDateTime::currentMSecsSinceEpoch();
    QSqlQuery run(database);
    run.prepare(QStringLiteral("INSERT INTO scan_runs(root_id, started, status, phase) VALUES(?, ?, 'running', 'walk')"));
    run.addBindValue(rootId);
    run.addBindValue(started);
    if (!run.exec()) {
        emit failed(queryError(run, QStringLiteral("Could not start scan run")));
        return;
    }
    const qint64 scanId = run.lastInsertId().toLongLong();
    QSqlQuery rootStart(database);
    rootStart.prepare(QStringLiteral("UPDATE library_roots SET last_scan_started=?, online=1 WHERE id=?"));
    rootStart.addBindValue(started);
    rootStart.addBindValue(rootId);
    rootStart.exec();

    auto phase = [&](const QString& name, auto operation) -> bool {
        QElapsedTimer timer;
        timer.start();
        QSqlQuery phaseUpdate(database);
        phaseUpdate.prepare(QStringLiteral("UPDATE scan_runs SET phase=? WHERE id=?"));
        phaseUpdate.addBindValue(name);
        phaseUpdate.addBindValue(scanId);
        phaseUpdate.exec();
        const bool ok = operation();
        timings.insert(name, timer.elapsed());
        return ok;
    };

    bool ok = waitWhilePaused()
        && phase(QStringLiteral("walk"), [&] { return walk(catalogue, rootId, scanId, rootPath, counts, &error); });
    // Any phase may find the root gone (drive unplugged), not only the walk.
    const auto rootOfflineNow = [&] { return counts.value(QStringLiteral("rootOffline")).toBool(); };
    if (ok && !shouldStop() && !rootOfflineNow() && waitWhilePaused())
        ok = phase(QStringLiteral("zip_directories"), [&] { return readZipDirectories(catalogue, rootId, rootPath, counts, &error); });
    if (ok && !shouldStop() && !rootOfflineNow() && waitWhilePaused())
        ok = phase(QStringLiteral("pairing"), [&] { return pairSources(catalogue, rootId, counts, &error); });
    if (ok && !shouldStop() && !rootOfflineNow()) {
        // Pairing parsed every current file name with today's rules.
        QSqlQuery parserVersion(database);
        parserVersion.prepare(QStringLiteral(
            "INSERT INTO catalogue_meta(key,value) VALUES('parser_version',?) "
            "ON CONFLICT(key) DO UPDATE SET value=excluded.value"));
        parserVersion.addBindValue(kFilenameParserVersion);
        parserVersion.exec();
    }
    if (ok && !shouldStop() && !rootOfflineNow() && waitWhilePaused()) {
        ok = phase(QStringLiteral("metadata"), [&] {
            return resolveAndSync(catalogue, rootId, QStringLiteral("metadata"), &error);
        });
        if (ok)
            emit libraryReady();
    }
    if (ok && !shouldStop() && !rootOfflineNow() && waitWhilePaused())
        ok = phase(QStringLiteral("sidecars"), [&] {
            return readSidecars(catalogue, rootId, rootPath, counts, &error);
        });
    if (ok && !shouldStop() && !rootOfflineNow() && m_options.readTags && waitWhilePaused())
        ok = phase(QStringLiteral("tags"), [&] { return enrichTags(catalogue, rootId, rootPath, counts, &error); });
    // Tags and track lists are both evidence, so the library is refreshed with
    // them before content matching decides which songs still need a name.
    if (ok && !shouldStop() && !rootOfflineNow() && waitWhilePaused()) {
        ok = phase(QStringLiteral("metadata_after_tags"), [&] {
            return resolveAndSync(catalogue, rootId,
                                  QStringLiteral("metadata_after_tags"), &error);
        });
        if (ok)
            emit libraryReady();
    }
    if (ok && !shouldStop() && !rootOfflineNow() && waitWhilePaused())
        ok = phase(QStringLiteral("duplicates"), [&] { return mergeZipDuplicates(catalogue, rootId, rootPath, counts, &error); });
    if (ok && !shouldStop() && !rootOfflineNow() && m_options.identifyDuplicates
        && waitWhilePaused())
        ok = phase(QStringLiteral("content_matching"), [&] {
            return identifyDuplicates(catalogue, rootId, rootPath, counts, &error);
        });
    // Cached title-screen readings reconnect by content (a rebuilt catalogue,
    // moved files); nothing is recognised during a scan.
    if (ok && !shouldStop() && !rootOfflineNow() && waitWhilePaused())
        ok = phase(QStringLiteral("title_screen_cache"), [&] {
            return readTitleScreens(catalogue, rootId, rootPath, counts, &error, false);
        });
    if (ok && !shouldStop() && !rootOfflineNow() && waitWhilePaused())
        ok = resolveAndSync(catalogue, rootId, QStringLiteral("metadata"), &error);
    if (ok && !shouldStop() && !rootOfflineNow() && waitWhilePaused())
        ok = phase(QStringLiteral("play_history"), [&] {
            reconcilePlayHistory(catalogue, counts);
            return true;
        });

    const bool rootOffline = rootOfflineNow();
    if (rootOffline) {
        QSqlQuery offlineRoot(database);
        offlineRoot.prepare(QStringLiteral("UPDATE library_roots SET online=0 WHERE id=?"));
        offlineRoot.addBindValue(rootId);
        offlineRoot.exec();
        QSqlQuery unavailable(database);
        unavailable.prepare(QStringLiteral("UPDATE sources SET playable=0, unplayable_reason='root_offline' WHERE root_id=?"));
        unavailable.addBindValue(rootId);
        unavailable.exec();
        QSqlQuery songs(database);
        songs.prepare(QStringLiteral("UPDATE songs SET playable=EXISTS(SELECT 1 FROM sources WHERE song_id=songs.id AND playable=1) WHERE EXISTS(SELECT 1 FROM sources WHERE song_id=songs.id AND root_id=?)"));
        songs.addBindValue(rootId);
        songs.exec();
    }

    const bool stopped = shouldStop();
    const bool walkComplete = counts.value(QStringLiteral("walkComplete"), true).toBool();
    const QString status = !ok ? QStringLiteral("failed")
                              : stopped ? QStringLiteral("cancelled")
                              : rootOffline ? QStringLiteral("offline")
                              : !walkComplete ? QStringLiteral("incomplete")
                                              : QStringLiteral("completed");
    counts.insert(QStringLiteral("sourceFileReads"), qulonglong(m_sourceFileReads));
    summary.insert(QStringLiteral("status"), status);
    summary.insert(QStringLiteral("counts"), counts);
    summary.insert(QStringLiteral("timingsMs"), timings);
    summary.insert(QStringLiteral("elapsedMs"), m_scanTimer.elapsed());
    summary.insert(QStringLiteral("root"), rootPath);
    QSqlQuery finish(database);
    finish.prepare(QStringLiteral("UPDATE scan_runs SET finished=?, status=?, phase=?, counts_json=? WHERE id=?"));
    finish.addBindValue(QDateTime::currentMSecsSinceEpoch());
    finish.addBindValue(status);
    finish.addBindValue(status);
    finish.addBindValue(QString::fromUtf8(QJsonDocument::fromVariant(counts).toJson(QJsonDocument::Compact)));
    finish.addBindValue(scanId);
    finish.exec();
    if (ok && !stopped && !rootOffline && walkComplete) {
        QSqlQuery complete(database);
        complete.prepare(QStringLiteral("UPDATE library_roots SET last_scan_completed=?, online=1 WHERE id=?"));
        complete.addBindValue(QDateTime::currentMSecsSinceEpoch());
        complete.addBindValue(rootId);
        complete.exec();
    }
    if (!ok) {
        emit failed(error);
        return;
    }
    emit finished(summary);
}

bool LibraryScanner::walk(Catalogue& catalogue, qint64 rootId, qint64 scanId,
                          const QString& rootPath, QVariantMap& counts, QString* error)
{
    QSqlDatabase database = catalogue.database();
    QList<QString> directories = {rootPath};
    qint64 done = 0;
    qint64 changed = 0;
    qint64 unchanged = 0;
    bool complete = true;
    bool rootOffline = false;
    qint64 skippedDirectories = 0;
    database.transaction();
    QSqlQuery lookup(database);
    lookup.prepare(QStringLiteral("SELECT id, size, mtime_ms FROM files WHERE root_id=? AND rel_path=?"));
    QSqlQuery upsert(database);
    upsert.prepare(QStringLiteral(
        "INSERT INTO files(root_id, rel_path, rel_dir, file_name, ext, kind, size, mtime_ms, last_seen_scan, present, tags_state) "
        "VALUES(?,?,?,?,?,?,?,?,?,1,?) ON CONFLICT(root_id, rel_path) DO UPDATE SET "
        "rel_dir=excluded.rel_dir,file_name=excluded.file_name,ext=excluded.ext,kind=excluded.kind,size=excluded.size,mtime_ms=excluded.mtime_ms,last_seen_scan=excluded.last_seen_scan,present=1,"
        "tags_state=CASE WHEN files.size<>excluded.size OR files.mtime_ms<>excluded.mtime_ms THEN excluded.tags_state ELSE files.tags_state END,"
        "raw_tags_json=CASE WHEN files.size<>excluded.size OR files.mtime_ms<>excluded.mtime_ms THEN NULL ELSE files.raw_tags_json END,"
        "crc32=CASE WHEN files.size<>excluded.size OR files.mtime_ms<>excluded.mtime_ms THEN NULL ELSE files.crc32 END,"
        "sha256=CASE WHEN files.size<>excluded.size OR files.mtime_ms<>excluded.mtime_ms THEN NULL ELSE files.sha256 END,"
        "quick_sha256=CASE WHEN files.size<>excluded.size OR files.mtime_ms<>excluded.mtime_ms THEN NULL ELSE files.quick_sha256 END,"
        "content_sha256=CASE WHEN files.size<>excluded.size OR files.mtime_ms<>excluded.mtime_ms THEN NULL ELSE files.content_sha256 END,"
        "cdg_packets=CASE WHEN files.size<>excluded.size OR files.mtime_ms<>excluded.mtime_ms THEN NULL ELSE files.cdg_packets END,"
        "zip_status=CASE WHEN files.size<>excluded.size OR files.mtime_ms<>excluded.mtime_ms THEN NULL ELSE files.zip_status END,"
        "zip_detail=CASE WHEN files.size<>excluded.size OR files.mtime_ms<>excluded.mtime_ms THEN NULL ELSE files.zip_detail END"));

    while (!directories.isEmpty()) {
        if (!pauseOutsideTransaction(database)) {
            complete = false;
            break;
        }
        if (!QFileInfo(rootPath).isDir()) {
            complete = false;
            rootOffline = true;
            QSqlQuery offline(database);
            offline.prepare(QStringLiteral("UPDATE library_roots SET online=0 WHERE id=?"));
            offline.addBindValue(rootId);
            offline.exec();
            break;
        }
        const QString directoryPath = directories.takeLast();
        if (!pauseOutsideTransaction(database)) {
            complete = false;
            break;
        }
        QDir directory(directoryPath);
        if (!directory.exists() || !directory.isReadable()) {
            complete = false;
            if (directoryPath == rootPath) {
                rootOffline = true;
                break;
            }
            ++skippedDirectories;
            qWarning().noquote() << "Skipping unreadable library folder:"
                                 << QDir(rootPath).relativeFilePath(directoryPath);
            continue;
        }
        QDirIterator iterator(directoryPath,
                              QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                              QDirIterator::NoIteratorFlags);
        while (true) {
            if (!pauseOutsideTransaction(database)) {
                complete = false;
                break;
            }
            if (!iterator.hasNext())
                break;
            if ((done % 64) == 0 && !QFileInfo(rootPath).isDir()) {
                complete = false;
                rootOffline = true;
                break;
            }
            iterator.next();
            const QFileInfo info = iterator.fileInfo();
            if (skippedName(info.fileName()) || info.isSymLink()
#if QT_VERSION >= QT_VERSION_CHECK(6, 2, 0)
                || info.isJunction()
#endif
                )
                continue;
            if (info.isDir()) {
                directories.append(info.absoluteFilePath());
                continue;
            }
            if (!info.isFile())
                continue;
            const QString relPath = QDir::fromNativeSeparators(QDir(rootPath).relativeFilePath(info.absoluteFilePath()));
            const QString relDir = QFileInfo(relPath).path() == QLatin1String(".")
                ? QStringLiteral("") : QFileInfo(relPath).path();
            // suffix() is a null QString for extensionless names, which binds as SQL NULL.
            const QString ext = info.suffix().isEmpty() ? QStringLiteral("") : info.suffix().toLower();
            const QString kind = kindForExtension(ext);
            const qint64 size = info.size();
            const qint64 mtime = info.lastModified().toMSecsSinceEpoch();
            lookup.bindValue(0, rootId);
            lookup.bindValue(1, relPath);
            if (!lookup.exec()) {
                database.rollback();
                *error = queryError(lookup, QStringLiteral("Could not inspect file row"));
                return false;
            }
            const bool existed = lookup.next();
            const qint64 existingId = existed ? lookup.value(0).toLongLong() : 0;
            const qint64 existingSize = existed ? lookup.value(1).toLongLong() : 0;
            const qint64 existingMtime = existed ? lookup.value(2).toLongLong() : 0;
            lookup.finish();
            const bool wasChanged = !existed || existingSize != size || existingMtime != mtime;
            changed += wasChanged;
            unchanged += !wasChanged;
            const QString tagsState = kind == QLatin1String("mp3") && size > 0
                ? QStringLiteral("pending") : QStringLiteral("none");
            const QVariantList values = {rootId, relPath, relDir, info.fileName(), ext, kind,
                                         size, mtime, scanId, tagsState};
            for (int i = 0; i < values.size(); ++i)
                upsert.bindValue(i, values.at(i));
            if (!upsert.exec()) {
                database.rollback();
                *error = queryError(upsert, QStringLiteral("Could not upsert file"));
                return false;
            }
            if (wasChanged && existed && kind == QLatin1String("zip")) {
                QSqlQuery clear(database);
                clear.prepare(QStringLiteral("DELETE FROM zip_members WHERE zip_file_id=?"));
                clear.addBindValue(existingId);
                clear.exec();
            }
            ++done;
            reportProgress(QStringLiteral("walk"), done, -1, relPath);
            if ((done % 500) == 0) {
                if (!database.commit() || !database.transaction()) {
                    *error = QStringLiteral("Could not commit walk batch: %1").arg(database.lastError().text());
                    return false;
                }
            }
        }
    }
    if (complete) {
        if (!pauseOutsideTransaction(database)) {
            complete = false;
        } else if (!QFileInfo(rootPath).isDir()) {
            complete = false;
            rootOffline = true;
        }
    }
    if (!database.commit()) {
        *error = QStringLiteral("Could not commit walk: %1").arg(database.lastError().text());
        return false;
    }
    if (complete && QFileInfo(rootPath).isDir()) {
        QSqlQuery missing(database);
        missing.prepare(QStringLiteral("UPDATE files SET present=0 WHERE root_id=? AND (last_seen_scan IS NULL OR last_seen_scan<>?)"));
        missing.addBindValue(rootId);
        missing.addBindValue(scanId);
        if (!missing.exec()) {
            *error = queryError(missing, QStringLiteral("Could not mark missing files"));
            return false;
        }
    }
    if (rootOffline && !shouldStop()) {
        QSqlQuery offline(database);
        offline.prepare(QStringLiteral("UPDATE library_roots SET online=0 WHERE id=?"));
        offline.addBindValue(rootId);
        if (!offline.exec()) {
            *error = queryError(offline, QStringLiteral("Could not mark root offline"));
            return false;
        }
        counts.insert(QStringLiteral("rootOffline"), true);
    }
    counts.insert(QStringLiteral("walkComplete"), complete);
    counts.insert(QStringLiteral("skippedUnreadableDirectories"), skippedDirectories);
    counts.insert(QStringLiteral("walked"), done);
    counts.insert(QStringLiteral("changed"), changed);
    counts.insert(QStringLiteral("unchanged"), unchanged);
    reportProgress(QStringLiteral("walk"), done, done, QString(), true);
    return true;
}

bool LibraryScanner::readZipDirectories(Catalogue& catalogue, qint64 rootId,
                                        const QString& rootPath, QVariantMap& counts,
                                        QString* error)
{
    QSqlDatabase database = catalogue.database();
    QSqlQuery list(database);
    list.prepare(QStringLiteral("SELECT id, rel_path FROM files WHERE root_id=? AND present=1 AND kind='zip' AND zip_status IS NULL ORDER BY id"));
    list.addBindValue(rootId);
    if (!list.exec()) {
        *error = queryError(list, QStringLiteral("Could not list pending ZIP files"));
        return false;
    }
    QList<QPair<qint64, QString>> zips;
    while (list.next())
        zips.append({list.value(0).toLongLong(), list.value(1).toString()});
    list.finish();
    qint64 done = 0;
    if (!database.transaction()) {
        *error = QStringLiteral("Could not begin ZIP-directory batch: %1").arg(database.lastError().text());
        return false;
    }
    QElapsedTimer batchTimer;
    batchTimer.start();
    for (const auto& zip : zips) {
        if (shouldStop())
            break;
        if (!pauseOutsideTransaction(database))
            break;
        ++m_sourceFileReads;
        const ZipDirectoryResult result = readZipDirectory(QDir(rootPath).filePath(zip.second));
        if (result.status == ZipStatus::Unreadable && rootGone(rootPath, counts))
            break;
        QSqlQuery clear(database);
        clear.prepare(QStringLiteral("DELETE FROM zip_members WHERE zip_file_id=?"));
        clear.addBindValue(zip.first);
        clear.exec();
        QSqlQuery insert(database);
        insert.prepare(QStringLiteral("INSERT INTO zip_members(zip_file_id,name,method,crc32,compressed_size,uncompressed_size,encrypted,damaged,kind) VALUES(?,?,?,?,?,?,?,?,?)"));
        for (const ZipMember& member : result.members) {
            if (member.isDirectory)
                continue;
            insert.bindValue(0, zip.first);
            insert.bindValue(1, member.name);
            insert.bindValue(2, member.method);
            insert.bindValue(3, qulonglong(member.crc32));
            insert.bindValue(4, qulonglong(member.compressedSize));
            insert.bindValue(5, qulonglong(member.uncompressedSize));
            insert.bindValue(6, member.encrypted);
            insert.bindValue(7, member.damaged);
            insert.bindValue(8, memberKind(member.name));
            if (!insert.exec()) {
                *error = queryError(insert, QStringLiteral("Could not store ZIP member"));
                return false;
            }
        }
        QSqlQuery update(database);
        update.prepare(QStringLiteral("UPDATE files SET zip_status=?, zip_detail=? WHERE id=?"));
        update.addBindValue(zipStatusName(result.status));
        update.addBindValue(result.detail);
        update.addBindValue(zip.first);
        if (!update.exec()) {
            *error = queryError(update, QStringLiteral("Could not store ZIP status"));
            return false;
        }
        ++done;
        reportProgress(QStringLiteral("zip_directories"), done, zips.size(), zip.second);
        if ((done % 500) == 0 || batchTimer.elapsed() >= 2000) {
            if (!database.commit() || !database.transaction()) {
                *error = QStringLiteral("Could not commit ZIP-directory batch: %1").arg(database.lastError().text());
                return false;
            }
            batchTimer.restart();
        }
    }
    if (!database.commit()) {
        *error = QStringLiteral("Could not commit ZIP directories: %1").arg(database.lastError().text());
        return false;
    }
    counts.insert(QStringLiteral("zipsRead"), done);
    reportProgress(QStringLiteral("zip_directories"), done, zips.size(), QString(), true);
    return true;
}

bool LibraryScanner::pairSources(Catalogue& catalogue, qint64 rootId,
                                 QVariantMap& counts, QString* error)
{
    QSqlDatabase database = catalogue.database();
    QSqlQuery mark(database);
    mark.prepare(QStringLiteral("UPDATE sources SET playable=0, unplayable_reason='file_missing' WHERE root_id=?"));
    mark.addBindValue(rootId);
    if (!mark.exec()) {
        *error = queryError(mark, QStringLiteral("Could not reset source availability"));
        return false;
    }
    QSqlQuery query(database);
    query.prepare(QStringLiteral("SELECT id, rel_path, rel_dir, file_name, kind, size, present FROM files WHERE root_id=? AND kind IN ('mp3','cdg','mcg') ORDER BY id"));
    query.addBindValue(rootId);
    if (!query.exec()) {
        *error = queryError(query, QStringLiteral("Could not load loose files"));
        return false;
    }
    QHash<QString, FileRow> mp3s;
    QHash<QString, FileRow> cdgs;
    QHash<QString, FileRow> mcgs;
    while (query.next()) {
        FileRow row{query.value(0).toLongLong(), query.value(1).toString(),
                    query.value(2).toString(), query.value(3).toString(),
                    QFileInfo(query.value(3).toString()).completeBaseName(),
                    query.value(4).toString(), query.value(5).toLongLong(),
                    query.value(6).toBool()};
        const QString key = foldedPairKey(row.relDir, row.stem);
        if (row.kind == QLatin1String("mp3")) mp3s.insert(key, row);
        else if (row.kind == QLatin1String("cdg")) cdgs.insert(key, row);
        else mcgs.insert(key, row);
    }
    query.finish();
    QSqlQuery find(database);
    find.prepare(QStringLiteral("SELECT id FROM sources WHERE root_id=? AND kind=? AND mp3_file_id=? AND graphics_file_id=? AND zip_file_id IS NULL"));
    QSqlQuery insert(database);
    insert.prepare(QStringLiteral("INSERT INTO sources(root_id,kind,mp3_file_id,graphics_file_id,playable,unplayable_reason,parsed_json) VALUES(?,?,?,?,?,?,?)"));
    QSqlQuery update(database);
    update.prepare(QStringLiteral("UPDATE sources SET playable=?,unplayable_reason=?,parsed_json=? WHERE id=?"));
    qint64 loosePairs = 0;
    if (!database.transaction()) {
        *error = QStringLiteral("Could not begin loose-pair batch: %1").arg(database.lastError().text());
        return false;
    }
    for (auto it = mp3s.cbegin(); it != mp3s.cend(); ++it) {
        if (shouldStop())
            break;
        if (!pauseOutsideTransaction(database))
            break;
        const FileRow mp3 = it.value();
        const bool hasCdg = cdgs.contains(it.key());
        const bool hasMcg = mcgs.contains(it.key());
        if (!hasCdg && !hasMcg)
            continue;
        const FileRow graphics = hasCdg ? cdgs.value(it.key()) : mcgs.value(it.key());
        const QString sourceKind = hasCdg ? QStringLiteral("loose_cdg") : QStringLiteral("loose_mcg");
        const bool present = mp3.present && graphics.present;
        const bool nonEmpty = mp3.size > 0 && graphics.size > 0;
        const bool available = present && nonEmpty;
        const bool playable = available && hasCdg;
        const QString reason = playable ? QString()
            : !present ? QStringLiteral("file_missing")
            : !nonEmpty ? QStringLiteral("file_empty")
                        : QStringLiteral("mcg_graphics_unsupported");
        const ParsedName parsed = parseSongName(mp3.relDir, mp3.fileName);
        const QString json = QString::fromUtf8(QJsonDocument(parsedNameJson(parsed, mp3.relPath, mp3.fileName)).toJson(QJsonDocument::Compact));
        find.bindValue(0, rootId);
        find.bindValue(1, sourceKind);
        find.bindValue(2, mp3.id);
        find.bindValue(3, graphics.id);
        if (!find.exec()) {
            *error = queryError(find, QStringLiteral("Could not inspect loose source"));
            return false;
        }
        const bool sourceExists = find.next();
        const qint64 sourceId = sourceExists ? find.value(0).toLongLong() : 0;
        find.finish();
        if (sourceExists) {
            update.bindValue(0, playable);
            update.bindValue(1, reason.isEmpty() ? QVariant() : QVariant(reason));
            update.bindValue(2, json);
            update.bindValue(3, sourceId);
            if (!update.exec()) {
                *error = queryError(update, QStringLiteral("Could not refresh loose source"));
                return false;
            }
        } else {
            insert.bindValue(0, rootId);
            insert.bindValue(1, sourceKind);
            insert.bindValue(2, mp3.id);
            insert.bindValue(3, graphics.id);
            insert.bindValue(4, playable);
            insert.bindValue(5, reason.isEmpty() ? QVariant() : QVariant(reason));
            insert.bindValue(6, json);
            if (!insert.exec()) {
                *error = queryError(insert, QStringLiteral("Could not create loose source"));
                return false;
            }
        }
        ++loosePairs;
        if ((loosePairs % 500) == 0) {
            if (!database.commit() || !database.transaction()) {
                *error = QStringLiteral("Could not commit loose-pair batch: %1").arg(database.lastError().text());
                return false;
            }
        }
    }
    if (!database.commit()) {
        *error = QStringLiteral("Could not commit loose pairing: %1").arg(database.lastError().text());
        return false;
    }

    QSqlQuery zipFiles(database);
    zipFiles.prepare(QStringLiteral("SELECT id FROM files WHERE root_id=? AND present=1 AND kind='zip' ORDER BY id"));
    zipFiles.addBindValue(rootId);
    if (!zipFiles.exec()) {
        *error = queryError(zipFiles, QStringLiteral("Could not load ZIP files"));
        return false;
    }
    QList<qint64> zipRows;
    while (zipFiles.next())
        zipRows.append(zipFiles.value(0).toLongLong());
    zipFiles.finish();
    qint64 zipPairs = 0;
    qint64 nested = 0;
    if (!database.transaction()) {
        *error = QStringLiteral("Could not begin ZIP-pair batch: %1").arg(database.lastError().text());
        return false;
    }
    for (qint64 zipId : zipRows) {
        if (shouldStop())
            break;
        QSqlQuery members(database);
        members.prepare(QStringLiteral("SELECT name,method,encrypted,damaged,kind FROM zip_members WHERE zip_file_id=? ORDER BY id"));
        members.addBindValue(zipId);
        if (!members.exec()) {
            *error = queryError(members, QStringLiteral("Could not load ZIP members"));
            return false;
        }
        struct Member { QString name; int method; bool encrypted; bool damaged; };
        QHash<QString, Member> zipMp3;
        QHash<QString, Member> zipCdg;
        bool hasNested = false;
        while (members.next()) {
            const QString name = members.value(0).toString();
            const QString kind = members.value(4).toString();
            if (kind == QLatin1String("zip"))
                hasNested = true;
            const QString stem = QFileInfo(name).completeBaseName().toCaseFolded();
            const Member member{name, members.value(1).toInt(), members.value(2).toBool(),
                                members.value(3).toBool()};
            if (kind == QLatin1String("mp3")) zipMp3.insert(stem, member);
            else if (kind == QLatin1String("cdg")) zipCdg.insert(stem, member);
        }
        members.finish();
        if (hasNested) {
            QSqlQuery nestedUpdate(database);
            nestedUpdate.prepare(QStringLiteral("UPDATE files SET zip_status='zip_nested', zip_detail='ZIP contains nested ZIP members' WHERE id=?"));
            nestedUpdate.addBindValue(zipId);
            nestedUpdate.exec();
            ++nested;
            continue;
        }
        for (auto it = zipMp3.cbegin(); it != zipMp3.cend(); ++it) {
            if (!zipCdg.contains(it.key()))
                continue;
            const Member mp3 = it.value();
            const Member cdg = zipCdg.value(it.key());
            const bool supported = !mp3.encrypted && !cdg.encrypted
                && zipMethodSupported(quint16(mp3.method)) && zipMethodSupported(quint16(cdg.method));
            const QString reason = mp3.damaged || cdg.damaged ? QStringLiteral("zip_damaged")
                : supported ? QStringLiteral("zip_playback_not_implemented")
                            : QStringLiteral("zip_compression_unsupported");
            const QString memberDir = QFileInfo(mp3.name).path() == QLatin1String(".") ? QString() : QFileInfo(mp3.name).path();
            const ParsedName parsed = parseSongName(memberDir, QFileInfo(mp3.name).fileName());
            const QString json = QString::fromUtf8(QJsonDocument(parsedNameJson(parsed, mp3.name, QFileInfo(mp3.name).fileName())).toJson(QJsonDocument::Compact));
            QSqlQuery existing(database);
            existing.prepare(QStringLiteral("SELECT id FROM sources WHERE root_id=? AND kind='zip_cdg' AND zip_file_id=? AND zip_mp3_member=? AND zip_graphics_member=?"));
            existing.addBindValue(rootId);
            existing.addBindValue(zipId);
            existing.addBindValue(mp3.name);
            existing.addBindValue(cdg.name);
            if (!existing.exec()) {
                *error = queryError(existing, QStringLiteral("Could not inspect ZIP source"));
                return false;
            }
            const bool sourceExists = existing.next();
            const qint64 sourceId = sourceExists ? existing.value(0).toLongLong() : 0;
            existing.finish();
            if (sourceExists) {
                QSqlQuery refresh(database);
                refresh.prepare(QStringLiteral("UPDATE sources SET playable=0,unplayable_reason=?,parsed_json=? WHERE id=?"));
                refresh.addBindValue(reason);
                refresh.addBindValue(json);
                refresh.addBindValue(sourceId);
                if (!refresh.exec()) {
                    *error = queryError(refresh, QStringLiteral("Could not refresh ZIP source"));
                    return false;
                }
            } else {
                QSqlQuery create(database);
                create.prepare(QStringLiteral("INSERT INTO sources(root_id,kind,zip_file_id,zip_mp3_member,zip_graphics_member,playable,unplayable_reason,parsed_json) VALUES(?,?,?,?,?,0,?,?)"));
                create.addBindValue(rootId);
                create.addBindValue(QStringLiteral("zip_cdg"));
                create.addBindValue(zipId);
                create.addBindValue(mp3.name);
                create.addBindValue(cdg.name);
                create.addBindValue(reason);
                create.addBindValue(json);
                if (!create.exec()) {
                    *error = queryError(create, QStringLiteral("Could not create ZIP source"));
                    return false;
                }
            }
            ++zipPairs;
            if ((zipPairs % 500) == 0) {
                if (!database.commit() || !database.transaction()) {
                    *error = QStringLiteral("Could not commit ZIP-pair batch: %1").arg(database.lastError().text());
                    return false;
                }
            }
        }
    }
    if (!database.commit()) {
        *error = QStringLiteral("Could not commit ZIP pairing: %1").arg(database.lastError().text());
        return false;
    }
    counts.insert(QStringLiteral("loosePairs"), loosePairs);
    counts.insert(QStringLiteral("zipPairs"), zipPairs);
    counts.insert(QStringLiteral("nestedZips"), nested);
    reportProgress(QStringLiteral("pairing"), loosePairs + zipPairs, loosePairs + zipPairs, QString(), true);
    return catalogue.ensureSongRows(rootId, error);
}

bool LibraryScanner::enrichTags(Catalogue& catalogue, qint64 rootId,
                                const QString& rootPath, QVariantMap& counts,
                                QString* error)
{
    QSqlDatabase database = catalogue.database();
    QSqlQuery query(database);
    query.prepare(QStringLiteral("SELECT id,rel_path FROM files WHERE root_id=? AND present=1 AND kind='mp3' AND tags_state='pending' ORDER BY id"));
    query.addBindValue(rootId);
    if (!query.exec()) {
        *error = queryError(query, QStringLiteral("Could not list pending tags"));
        return false;
    }
    QList<QPair<qint64, QString>> files;
    while (query.next())
        files.append({query.value(0).toLongLong(), query.value(1).toString()});
    qint64 done = 0;
    QSqlQuery update(database);
    update.prepare(QStringLiteral("UPDATE files SET tags_state='read',raw_tags_json=? WHERE id=?"));
    if (!database.transaction()) {
        *error = QStringLiteral("Could not begin tag-enrichment batch: %1").arg(database.lastError().text());
        return false;
    }
    QElapsedTimer batchTimer;
    batchTimer.start();
    for (const auto& file : files) {
        if (shouldStop())
            break;
        if (!pauseOutsideTransaction(database))
            break;
        ++m_sourceFileReads;
        const QString path = QDir(rootPath).filePath(file.second);
        const Id3Tags tags = readId3Tags(path);
        // Empty tags from a file that has vanished are not a real result: keep it
        // pending so a later scan reads it.
        if (tags.isEmpty() && !QFileInfo::exists(path)) {
            if (rootGone(rootPath, counts))
                break;
            continue;
        }
        update.bindValue(0, QString::fromUtf8(QJsonDocument(tagsJson(tags)).toJson(QJsonDocument::Compact)));
        update.bindValue(1, file.first);
        if (!update.exec()) {
            *error = queryError(update, QStringLiteral("Could not store ID3 tags"));
            return false;
        }
        ++done;
        reportProgress(QStringLiteral("tags"), done, files.size(), file.second);
        if ((done % 500) == 0 || batchTimer.elapsed() >= 2000) {
            if (!database.commit() || !database.transaction()) {
                *error = QStringLiteral("Could not commit tag-enrichment batch: %1").arg(database.lastError().text());
                return false;
            }
            batchTimer.restart();
        }
    }
    if (!database.commit()) {
        *error = QStringLiteral("Could not commit tag enrichment: %1").arg(database.lastError().text());
        return false;
    }
    counts.insert(QStringLiteral("tagsRead"), done);
    reportProgress(QStringLiteral("tags"), done, files.size(), QString(), true);
    return true;
}

bool LibraryScanner::mergeZipDuplicates(Catalogue& catalogue, qint64 rootId,
                                        const QString& rootPath, QVariantMap& counts,
                                        QString* error)
{
    QSqlDatabase database = catalogue.database();
    QSqlQuery zips(database);
    zips.prepare(QStringLiteral(
        "SELECT zs.id,zs.song_id,zs.zip_file_id,zs.zip_mp3_member,zs.zip_graphics_member,zs.parsed_json,"
        "zm.uncompressed_size,zm.crc32,zg.uncompressed_size,zg.crc32 "
        "FROM sources zs JOIN zip_members zm ON zm.zip_file_id=zs.zip_file_id AND zm.name=zs.zip_mp3_member "
        "JOIN zip_members zg ON zg.zip_file_id=zs.zip_file_id AND zg.name=zs.zip_graphics_member "
        "WHERE zs.root_id=? AND zs.kind='zip_cdg'"));
    zips.addBindValue(rootId);
    if (!zips.exec()) {
        *error = queryError(zips, QStringLiteral("Could not load ZIP duplicate candidates"));
        return false;
    }
    qint64 merged = 0;
    while (zips.next()) {
        if (shouldStop() || counts.value(QStringLiteral("rootOffline")).toBool())
            break;
        if (!waitWhilePaused())
            break;
        const QJsonObject zipParsed = QJsonDocument::fromJson(zips.value(5).toByteArray()).object();
        const QString zipStem = QFileInfo(zipParsed.value(QStringLiteral("rawPath")).toString()).completeBaseName().toCaseFolded();
        QSqlQuery loose(database);
        loose.prepare(QStringLiteral(
            "SELECT s.id,s.song_id,m.id,m.rel_path,m.crc32,g.id,g.rel_path,g.crc32,s.parsed_json "
            "FROM sources s JOIN files m ON m.id=s.mp3_file_id JOIN files g ON g.id=s.graphics_file_id "
            "WHERE s.root_id=? AND s.kind='loose_cdg' AND m.present=1 AND g.present=1 AND m.size=? AND g.size=?"));
        loose.addBindValue(rootId);
        loose.addBindValue(zips.value(6));
        loose.addBindValue(zips.value(8));
        if (!loose.exec()) {
            *error = queryError(loose, QStringLiteral("Could not inspect loose duplicate candidates"));
            return false;
        }
        while (loose.next()) {
            const QJsonObject looseParsed = QJsonDocument::fromJson(loose.value(8).toByteArray()).object();
            const QString looseStem = QFileInfo(looseParsed.value(QStringLiteral("rawPath")).toString()).completeBaseName().toCaseFolded();
            const bool sameIdentity = (!zipStem.isEmpty() && zipStem == looseStem)
                || (!zipParsed.value(QStringLiteral("discId")).toString().isEmpty()
                    && zipParsed.value(QStringLiteral("discId")) == looseParsed.value(QStringLiteral("discId"))
                    && zipParsed.value(QStringLiteral("track")) == looseParsed.value(QStringLiteral("track")));
            if (!sameIdentity)
                continue;
            // A source that can't be read here (e.g. a transient drive error) only
            // skips this candidate; its crc32 stays NULL so the next scan retries it.
            QString readError;
            auto crcFor = [&](int idColumn, int pathColumn, int crcColumn, quint32* value) -> bool {
                if (!loose.value(crcColumn).isNull()) {
                    *value = loose.value(crcColumn).toUInt();
                    return true;
                }
                ++m_sourceFileReads;
                if (!readCrc32(QDir(rootPath).filePath(loose.value(pathColumn).toString()),
                               m_cancelled, m_paused, value, &readError))
                    return false;
                QSqlQuery store(database);
                store.prepare(QStringLiteral("UPDATE files SET crc32=? WHERE id=?"));
                store.addBindValue(qulonglong(*value));
                store.addBindValue(loose.value(idColumn));
                return store.exec();
            };
            quint32 mp3Crc = 0;
            quint32 cdgCrc = 0;
            if (!crcFor(2, 3, 4, &mp3Crc) || !crcFor(5, 6, 7, &cdgCrc)) {
                if (shouldStop())
                    return true;
                if (readError.isEmpty()) {
                    *error = QStringLiteral("Could not store duplicate checksum: %1")
                                 .arg(database.lastError().text());
                    return false;
                }
                if (rootGone(rootPath, counts))
                    break;
                qWarning().noquote() << "Skipping ZIP duplicate check:" << readError
                                     << loose.value(3).toString();
                continue;
            }
            if (mp3Crc != zips.value(7).toUInt() || cdgCrc != zips.value(9).toUInt())
                continue;
            const qint64 oldSong = zips.value(1).toLongLong();
            QSqlQuery attach(database);
            attach.prepare(QStringLiteral("UPDATE sources SET song_id=? WHERE id=?"));
            attach.addBindValue(loose.value(1));
            attach.addBindValue(zips.value(0));
            if (!attach.exec()) {
                *error = queryError(attach, QStringLiteral("Could not merge ZIP duplicate"));
                return false;
            }
            QSqlQuery cleanup(database);
            cleanup.prepare(QStringLiteral("DELETE FROM songs WHERE id=? AND NOT EXISTS(SELECT 1 FROM sources WHERE song_id=?)"));
            cleanup.addBindValue(oldSong);
            cleanup.addBindValue(oldSong);
            cleanup.exec();
            ++merged;
            break;
        }
    }
    counts.insert(QStringLiteral("zipDuplicatesMerged"), merged);
    reportProgress(QStringLiteral("duplicates"), merged, merged, QString(), true);
    return true;
}

bool LibraryScanner::identifyDuplicates(Catalogue& catalogue, qint64 rootId,
                                        const QString& rootPath, QVariantMap& counts,
                                        QString* error)
{
    // Weakly named songs are compared with well-named songs whose CDG has
    // exactly the same size. The size only groups files; identity is proven
    // later by the resolver from complete content digests. Work is linear:
    // each file gets a cheap quick digest once, full digests are computed
    // only inside quick-digest collisions, and every digest is cached per
    // file (so an interrupted pass resumes without reading anything twice).
    // Each digest is saved on its own; no transaction is held while a file
    // is read or while playback pauses the work.
    QSqlDatabase database = catalogue.database();
    // Digests made under an older definition of the drawing-packet count are
    // recomputed (only the few files that were ever fully hashed).
    if (catalogue.catalogueMeta(QStringLiteral("content_digest_version")).toInt() < kContentDigestVersion) {
        QSqlQuery reset(database);
        QSqlQuery version(database);
        version.prepare(QStringLiteral(
            "INSERT INTO catalogue_meta(key,value) VALUES('content_digest_version',?) "
            "ON CONFLICT(key) DO UPDATE SET value=excluded.value"));
        version.addBindValue(kContentDigestVersion);
        if (!reset.exec(QStringLiteral(
                "UPDATE files SET content_sha256=NULL,cdg_packets=NULL WHERE kind='cdg' "
                "AND content_sha256 IS NOT NULL")) || !version.exec()) {
            *error = queryError(reset, QStringLiteral("Could not reset content digests"));
            return false;
        }
    }
    struct Member {
        qint64 cdg = 0;
        qint64 mp3 = 0;
        QString cdgPath;
        QString mp3Path;
        bool weak = false;
        QByteArray quick;
        QByteArray content;
    };
    // Every playable pair whose song is weak or strong, grouped by CDG size,
    // keeping only sizes that hold at least one of each.
    QSqlQuery members(database);
    members.prepare(QStringLiteral(
        "SELECT g.size,g.id,g.rel_path,g.quick_sha256,g.content_sha256,m.id,m.rel_path,"
        "so.base_confidence IN ('unresolved','low') FROM songs so "
        "JOIN sources s ON s.song_id=so.id AND s.kind='loose_cdg' "
        "JOIN files g ON g.id=s.graphics_file_id JOIN files m ON m.id=s.mp3_file_id "
        "WHERE s.root_id=? AND g.present=1 AND m.present=1 AND g.size>0 "
        "AND so.base_confidence IN ('unresolved','low','medium','high') "
        "AND g.size IN (SELECT tg.size FROM songs tso "
        "JOIN sources ts ON ts.song_id=tso.id AND ts.kind='loose_cdg' "
        "JOIN files tg ON tg.id=ts.graphics_file_id "
        "WHERE ts.root_id=? AND tg.present=1 AND tso.base_confidence IN ('unresolved','low') "
        "INTERSECT SELECT dg.size FROM songs dso "
        "JOIN sources ds ON ds.song_id=dso.id AND ds.kind='loose_cdg' "
        "JOIN files dg ON dg.id=ds.graphics_file_id "
        "WHERE ds.root_id=? AND dg.present=1 AND dso.base_confidence IN ('high','medium')) "
        "ORDER BY g.size,g.id"));
    members.addBindValue(rootId);
    members.addBindValue(rootId);
    members.addBindValue(rootId);
    if (!members.exec()) {
        *error = queryError(members, QStringLiteral("Could not list duplicate candidates"));
        return false;
    }
    QMap<qint64, QList<Member>> bySize;
    qint64 memberCount = 0;
    while (members.next()) {
        Member member;
        member.cdg = members.value(1).toLongLong();
        member.cdgPath = members.value(2).toString();
        member.quick = members.value(3).toByteArray();
        member.content = members.value(4).toByteArray();
        member.mp3 = members.value(5).toLongLong();
        member.mp3Path = members.value(6).toString();
        member.weak = members.value(7).toBool();
        bySize[members.value(0).toLongLong()].append(member);
        ++memberCount;
    }
    members.finish();

    qint64 digestsComputed = 0;
    bool stopped = false;
    // Returns false only for a database failure. An unreadable file leaves
    // an empty digest: that file is skipped and retried by a later pass.
    auto compute = [&](qint64 fileId, const QString& relPath, const char* column, bool isCdg,
                       QByteArray* digest) -> bool {
        digest->clear();
        if (!waitWhilePaused()) {
            stopped = true;
            return true;
        }
        const QString path = QDir(rootPath).filePath(relPath);
        ++m_sourceFileReads;
        qint64 packets = -1;
        const bool quick = qstrcmp(column, "quick_sha256") == 0;
        const bool ok = quick ? quickFileDigest(path, digest)
            : isCdg ? cdgContentDigest(path, digest, &packets)
                    : mp3AudioDigest(path, digest);
        if (!ok) {
            digest->clear();
            if (rootGone(rootPath, counts))
                stopped = true;
            return true;
        }
        ++digestsComputed;
        QSqlQuery store(database);
        if (packets >= 0) {
            store.prepare(QStringLiteral("UPDATE files SET %1=?,cdg_packets=? WHERE id=?")
                              .arg(QLatin1String(column)));
            store.addBindValue(*digest);
            store.addBindValue(packets);
        } else {
            store.prepare(QStringLiteral("UPDATE files SET %1=? WHERE id=?").arg(QLatin1String(column)));
            store.addBindValue(*digest);
        }
        store.addBindValue(fileId);
        if (!store.exec()) {
            *error = queryError(store, QStringLiteral("Could not store content digest"));
            return false;
        }
        return true;
    };
    auto cachedMp3 = [&](qint64 fileId, QByteArray* digest) {
        QSqlQuery query(database);
        query.prepare(QStringLiteral("SELECT content_sha256 FROM files WHERE id=?"));
        query.addBindValue(fileId);
        *digest = query.exec() && query.next() ? query.value(0).toByteArray() : QByteArray();
    };

    qint64 done = 0;
    qint64 proven = 0;
    for (auto group = bySize.begin(); group != bySize.end() && !stopped && !shouldStop(); ++group) {
        QList<Member>& list = group.value();
        for (Member& member : list) {
            if (member.quick.isEmpty()
                && !compute(member.cdg, member.cdgPath, "quick_sha256", true, &member.quick))
                return false;
            ++done;
            reportProgress(QStringLiteral("content_matching"), done, memberCount, member.cdgPath);
            if (stopped || shouldStop())
                break;
        }
        QHash<QByteArray, QList<int>> byQuick;
        for (int i = 0; i < list.size(); ++i) {
            if (!list.at(i).quick.isEmpty())
                byQuick[list.at(i).quick].append(i);
        }
        for (const QList<int>& same : std::as_const(byQuick)) {
            bool weak = false;
            bool strong = false;
            for (int i : same)
                (list.at(i).weak ? weak : strong) = true;
            if (!weak || !strong)
                continue;
            QHash<QByteArray, QList<int>> byContent;
            for (int i : same) {
                Member& member = list[i];
                if (member.content.isEmpty()
                    && !compute(member.cdg, member.cdgPath, "content_sha256", true, &member.content))
                    return false;
                if (stopped || shouldStop())
                    break;
                if (!member.content.isEmpty())
                    byContent[member.content].append(i);
            }
            for (const QList<int>& identical : std::as_const(byContent)) {
                bool hasWeak = false;
                bool hasStrong = false;
                for (int i : identical)
                    (list.at(i).weak ? hasWeak : hasStrong) = true;
                if (!hasWeak || !hasStrong)
                    continue;
                ++proven;
                for (int i : identical) {
                    QByteArray audio;
                    cachedMp3(list.at(i).mp3, &audio);
                    if (audio.isEmpty()
                        && !compute(list.at(i).mp3, list.at(i).mp3Path, "content_sha256", false, &audio))
                        return false;
                    if (stopped || shouldStop())
                        break;
                }
            }
        }
    }
    counts.insert(QStringLiteral("contentCandidateFiles"), memberCount);
    counts.insert(QStringLiteral("contentDigestsComputed"), digestsComputed);
    counts.insert(QStringLiteral("contentIdenticalCdgGroups"), proven);
    reportProgress(QStringLiteral("content_matching"), done, memberCount, QString(), true);
    return true;
}

bool LibraryScanner::readSidecars(Catalogue& catalogue, qint64 rootId, const QString& rootPath,
                                  QVariantMap& counts, QString* error)
{
    // Small text files that sit in a folder with songs may be disc track
    // lists. Each is read once (read-only) and re-read only when it changes.
    QSqlDatabase database = catalogue.database();
    QSqlQuery list(database);
    list.prepare(QStringLiteral(
        "SELECT f.id,f.rel_path,f.size,f.mtime_ms FROM files f "
        "LEFT JOIN sidecar_files sf ON sf.file_id=f.id "
        "WHERE f.root_id=? AND f.present=1 AND f.kind='other' AND lower(f.ext)='txt' "
        "AND f.size>0 AND f.size<=? "
        "AND (sf.file_id IS NULL OR sf.size<>f.size OR sf.mtime_ms<>f.mtime_ms) "
        "AND EXISTS(SELECT 1 FROM files m WHERE m.root_id=f.root_id AND m.rel_dir=f.rel_dir "
        "AND m.kind='mp3' AND m.present=1) ORDER BY f.id"));
    list.addBindValue(rootId);
    list.addBindValue(kMaximumSidecarBytes);
    if (!list.exec()) {
        *error = queryError(list, QStringLiteral("Could not list track-list files"));
        return false;
    }
    struct Text { qint64 id; QString relPath; qint64 size; qint64 mtime; };
    QList<Text> texts;
    while (list.next())
        texts.append({list.value(0).toLongLong(), list.value(1).toString(),
                      list.value(2).toLongLong(), list.value(3).toLongLong()});
    list.finish();
    qint64 recognised = 0;
    qint64 done = 0;
    for (const Text& text : std::as_const(texts)) {
        if (shouldStop() || !waitWhilePaused())
            break;
        ++m_sourceFileReads;
        QFile file(QDir(rootPath).filePath(text.relPath));
        if (!file.open(QIODevice::ReadOnly)) {
            if (rootGone(rootPath, counts))
                break;
            continue;
        }
        const QByteArray contents = file.read(kMaximumSidecarBytes + 1);
        file.close();
        const SidecarTrackList parsed = parseTrackListSidecar(contents);
        if (!database.transaction()) {
            *error = QStringLiteral("Could not begin track-list update: %1").arg(database.lastError().text());
            return false;
        }
        QSqlQuery clear(database);
        clear.prepare(QStringLiteral("DELETE FROM sidecar_entries WHERE file_id=?"));
        clear.addBindValue(text.id);
        QSqlQuery state(database);
        state.prepare(QStringLiteral(
            "INSERT INTO sidecar_files(file_id,size,mtime_ms,state,disc_id,detail) VALUES(?,?,?,?,?,?) "
            "ON CONFLICT(file_id) DO UPDATE SET size=excluded.size,mtime_ms=excluded.mtime_ms,"
            "state=excluded.state,disc_id=excluded.disc_id,detail=excluded.detail"));
        state.addBindValue(text.id);
        state.addBindValue(text.size);
        state.addBindValue(text.mtime);
        state.addBindValue(parsed.recognised ? QStringLiteral("track_list") : QStringLiteral("ignored"));
        state.addBindValue(parsed.discId.isEmpty() ? QVariant() : QVariant(parsed.discId));
        state.addBindValue(parsed.reason.isEmpty() ? QVariant() : QVariant(parsed.reason));
        bool ok = clear.exec() && state.exec();
        QSqlQuery insert(database);
        insert.prepare(QStringLiteral(
            "INSERT INTO sidecar_entries(file_id,disc_id,track,fields_json,line) VALUES(?,?,?,?,?)"));
        for (const SidecarEntry& entry : parsed.entries) {
            if (!ok)
                break;
            insert.bindValue(0, text.id);
            insert.bindValue(1, entry.discId);
            insert.bindValue(2, entry.track);
            insert.bindValue(3, QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(entry.fields))
                                                     .toJson(QJsonDocument::Compact)));
            insert.bindValue(4, entry.line);
            ok = insert.exec();
        }
        if (!ok || !database.commit()) {
            database.rollback();
            *error = QStringLiteral("Could not store track list: %1").arg(database.lastError().text());
            return false;
        }
        recognised += parsed.recognised ? 1 : 0;
        ++done;
        reportProgress(QStringLiteral("sidecars"), done, texts.size(), text.relPath);
    }
    counts.insert(QStringLiteral("sidecarsRead"), done);
    counts.insert(QStringLiteral("sidecarTrackLists"), recognised);
    return true;
}

bool LibraryScanner::readTitleScreens(Catalogue& catalogue, qint64 rootId,
                                      const QString& rootPath, QVariantMap& counts,
                                      QString* error, bool recogniseNew)
{
    // Songs that are still unresolved are identified by the text their CDG
    // shows at the start. Results are cached by CDG content (quick digest and
    // size) in the enrichment cache, which outlives the catalogue, so a song
    // is read and recognised once, and imported results from another
    // computer match without any recognition here. Without recogniseNew
    // (every scan) only CDGs of a size with a cached reading are fingerprinted
    // to reconnect them; nothing is recognised.
    QSqlDatabase database = catalogue.database();
    const QString engine = recogniseNew && m_titleScreenOcr ? m_titleScreenOcr->name() : QString();
    QSqlQuery anyImported(database);
    if (!anyImported.exec(QStringLiteral("SELECT EXISTS(SELECT 1 FROM enrich.title_screens)"))
        || !anyImported.next()) {
        *error = queryError(anyImported, QStringLiteral("Could not inspect title screens"));
        return false;
    }
    const bool haveResults = anyImported.value(0).toBool();
    anyImported.finish();
    if (engine.isEmpty() && !haveResults)
        return true;
    QSqlQuery targets(database);
    targets.prepare(QStringLiteral(
        "SELECT g.id,g.rel_path,g.size,g.quick_sha256 FROM songs so "
        "JOIN sources s ON s.song_id=so.id AND s.kind='loose_cdg' "
        "JOIN files g ON g.id=s.graphics_file_id "
        "WHERE s.root_id=? AND g.present=1 AND g.size>0 AND so.auto_confidence='unresolved' "
        "AND (g.quick_sha256 IS NULL OR NOT EXISTS(SELECT 1 FROM enrich.title_screens t "
        "WHERE t.cdg_quick_sha256=g.quick_sha256 AND t.cdg_size=g.size "
        "AND (t.engine=? OR ?=''))) "
        // Without recognition, only an unfingerprinted CDG of a cached size can match.
        "AND (?<>'' OR (g.quick_sha256 IS NULL AND g.size IN (SELECT cdg_size FROM enrich.title_screens))) "
        "ORDER BY g.rel_dir,g.id"));
    targets.addBindValue(rootId);
    targets.addBindValue(engine);
    targets.addBindValue(engine);
    targets.addBindValue(engine);
    if (!targets.exec()) {
        *error = queryError(targets, QStringLiteral("Could not list title-screen targets"));
        return false;
    }
    struct Target { qint64 id; QString relPath; qint64 size; QByteArray quick; };
    QList<Target> list;
    while (targets.next())
        list.append({targets.value(0).toLongLong(), targets.value(1).toString(),
                     targets.value(2).toLongLong(), targets.value(3).toByteArray()});
    targets.finish();
    qint64 done = 0;
    qint64 recognised = 0;
    qint64 withoutTitleScreen = 0;
    for (Target target : std::as_const(list)) {
        if (shouldStop() || !waitWhilePaused())
            break;
        const QString path = QDir(rootPath).filePath(target.relPath);
        if (target.quick.isEmpty()) {
            ++m_sourceFileReads;
            if (!quickFileDigest(path, &target.quick)) {
                if (rootGone(rootPath, counts))
                    break;
                continue;
            }
            QSqlQuery store(database);
            store.prepare(QStringLiteral("UPDATE files SET quick_sha256=? WHERE id=?"));
            store.addBindValue(target.quick);
            store.addBindValue(target.id);
            if (!store.exec()) {
                *error = queryError(store, QStringLiteral("Could not store content digest"));
                return false;
            }
        }
        if (engine.isEmpty())
            continue;  // imported results are matched by the digest alone
        QSqlQuery known(database);
        known.prepare(QStringLiteral(
            "SELECT 1 FROM enrich.title_screens WHERE cdg_quick_sha256=? AND cdg_size=? AND engine=?"));
        known.addBindValue(target.quick);
        known.addBindValue(target.size);
        known.addBindValue(engine);
        if (known.exec() && known.next())
            continue;  // an identical CDG elsewhere was already read
        known.finish();
        ++m_sourceFileReads;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            if (rootGone(rootPath, counts))
                break;
            continue;
        }
        const QByteArray bytes = file.readAll();
        const bool readOk = file.error() == QFileDevice::NoError;
        file.close();
        if (!readOk) {
            if (rootGone(rootPath, counts))
                break;
            continue;
        }
        const auto* first = reinterpret_cast<const std::uint8_t*>(bytes.constData());
        const std::vector<std::uint8_t> stream(first, first + bytes.size());
        const std::vector<cdg::TitleFrame> frames = cdg::findTitleFrames(stream);
        QJsonArray framesJson;
        QString status = frames.empty() ? QStringLiteral("no_title_frame") : QStringLiteral("ok");
        for (const cdg::TitleFrame& frame : frames) {
            if (shouldStop())
                break;
            QList<OcrLine> lines;
            QString ocrError;
            if (!m_titleScreenOcr->recognise(frame.pixels, cdg::CdgDecoder::kWidth,
                                             cdg::CdgDecoder::kHeight, &lines, &ocrError)) {
                status = QStringLiteral("ocr_failed");
                qWarning().noquote() << "Title-screen recognition failed:" << ocrError
                                     << target.relPath;
                break;
            }
            QJsonArray linesJson;
            for (const OcrLine& line : std::as_const(lines)) {
                QJsonObject item;
                item.insert(QStringLiteral("text"), line.text);
                item.insert(QStringLiteral("confidence"), line.confidence);
                item.insert(QStringLiteral("box"), QJsonArray{line.x, line.y, line.width, line.height});
                linesJson.append(item);
            }
            QJsonObject frameJson;
            frameJson.insert(QStringLiteral("timeMs"), frame.timeMs);
            frameJson.insert(QStringLiteral("lines"), linesJson);
            framesJson.append(frameJson);
        }
        if (shouldStop())
            break;
        if (status == QLatin1String("ocr_failed"))
            continue;  // not cached: a later pass retries it
        QSqlQuery store(database);
        store.prepare(QStringLiteral(
            "INSERT OR REPLACE INTO enrich.title_screens(cdg_quick_sha256,cdg_size,engine,status,"
            "frames_json,created_at) VALUES(?,?,?,?,?,?)"));
        store.addBindValue(target.quick);
        store.addBindValue(target.size);
        store.addBindValue(engine);
        store.addBindValue(status);
        store.addBindValue(QString::fromUtf8(QJsonDocument(framesJson).toJson(QJsonDocument::Compact)));
        store.addBindValue(QDateTime::currentMSecsSinceEpoch());
        if (!store.exec()) {
            *error = queryError(store, QStringLiteral("Could not store title-screen text"));
            return false;
        }
        ++done;
        recognised += frames.empty() ? 0 : 1;
        withoutTitleScreen += frames.empty() ? 1 : 0;
        reportProgress(QStringLiteral("title_screens"), done, list.size(), target.relPath);
    }
    counts.insert(QStringLiteral("titleScreenTargets"), list.size());
    counts.insert(QStringLiteral("titleScreensRead"), done);
    counts.insert(QStringLiteral("titleScreensWithFrames"), recognised);
    counts.insert(QStringLiteral("titleScreensWithoutFrame"), withoutTitleScreen);
    return true;
}
