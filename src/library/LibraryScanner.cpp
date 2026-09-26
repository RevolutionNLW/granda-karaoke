#include "library/LibraryScanner.h"

#include "library/Catalogue.h"
#include "library/FilenameParser.h"
#include "library/Id3Reader.h"
#include "library/MetadataResolver.h"
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

QString foldedPairKey(const QString& directory, const QString& stem)
{
    return directory.toCaseFolded() + QChar(0x1f) + stem.toCaseFolded();
}

} // namespace

LibraryScanner::LibraryScanner(QString databasePath, QString cacheDirectory, QObject* parent)
    : QObject(parent)
    , m_databasePath(std::move(databasePath))
    , m_cacheDirectory(std::move(cacheDirectory))
{
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
    if (!catalogue.open(&error, {rootPath}) || !catalogue.addRoot(rootPath, nullptr, &error)) {
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
    if (ok && !shouldStop() && !rootOfflineNow() && waitWhilePaused()) {
        ok = phase(QStringLiteral("metadata"), [&] { return MetadataResolver::resolve(catalogue, rootId, &error); });
        if (ok)
            emit libraryReady();
    }
    if (ok && !shouldStop() && !rootOfflineNow() && m_options.readTags && waitWhilePaused())
        ok = phase(QStringLiteral("tags"), [&] { return enrichTags(catalogue, rootId, rootPath, counts, &error); });
    if (ok && !shouldStop() && !rootOfflineNow() && m_options.readTags && waitWhilePaused()) {
        ok = phase(QStringLiteral("metadata_after_tags"), [&] { return MetadataResolver::resolve(catalogue, rootId, &error); });
        if (ok)
            emit libraryReady();
    }
    if (ok && !shouldStop() && !rootOfflineNow() && waitWhilePaused())
        ok = phase(QStringLiteral("duplicates"), [&] { return mergeZipDuplicates(catalogue, rootId, rootPath, counts, &error); });
    if (ok && !shouldStop() && !rootOfflineNow() && waitWhilePaused())
        ok = MetadataResolver::resolve(catalogue, rootId, &error);

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
        "zip_status=CASE WHEN files.size<>excluded.size OR files.mtime_ms<>excluded.mtime_ms THEN NULL ELSE files.zip_status END,"
        "zip_detail=CASE WHEN files.size<>excluded.size OR files.mtime_ms<>excluded.mtime_ms THEN NULL ELSE files.zip_detail END"));

    while (!directories.isEmpty()) {
        if (!waitWhilePaused()) {
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
        if (!waitWhilePaused()) {
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
            if (!waitWhilePaused()) {
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
        if (!waitWhilePaused()) {
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
        if (!waitWhilePaused())
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
        if (!waitWhilePaused())
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
    return true;
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
        if (!waitWhilePaused())
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
