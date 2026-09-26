#include "library/Catalogue.h"

#include "library/FilenameParser.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>
#include <QUuid>

namespace {

Q_LOGGING_CATEGORY(lcCatalogue, "fks.catalogue")

QString sqlError(const QSqlQuery& query, const QString& context)
{
    return QStringLiteral("%1: %2").arg(context, query.lastError().text());
}

QString escapedLike(QString value)
{
    value.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    value.replace(QLatin1Char('%'), QStringLiteral("\\%"));
    value.replace(QLatin1Char('_'), QStringLiteral("\\_"));
    return value;
}

QVariantMap groupedCounts(QSqlDatabase database, const QString& sql, QString* error)
{
    QVariantMap result;
    QSqlQuery query(database);
    if (!query.exec(sql)) {
        if (error)
            *error = sqlError(query, QStringLiteral("Statistics query failed"));
        return result;
    }
    while (query.next())
        result.insert(query.value(0).toString(), query.value(1));
    return result;
}

bool indicatesCorruption(const QString& detail)
{
    const QString folded = detail.toLower();
    return folded.contains(QStringLiteral("malformed"))
        || folded.contains(QStringLiteral("not a database"))
        || folded.contains(QStringLiteral("database disk image is malformed"));
}

QString lexicalPath(QString path)
{
    path = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    path = path.toCaseFolded();
#endif
    return path;
}

bool lexicalPathIsInsideOrEqual(const QString& candidate, const QString& root)
{
    QString child = lexicalPath(candidate);
    QString parent = lexicalPath(root);
    if (child == parent)
        return true;
    if (!parent.endsWith(QDir::separator()))
        parent.append(QDir::separator());
    return child.startsWith(parent);
}

bool storedStorageIsSafe(const QString& databasePath, const QString& cacheDirectory,
                         const QStringList& storedRoots, QString* error)
{
    // Stored roots were canonicalised when they were added.  Do not resolve
    // them again here: DB-only commands must not probe a disconnected volume.
    for (const QString& root : storedRoots) {
        if (lexicalPathIsInsideOrEqual(databasePath, root)) {
            if (error)
                *error = QStringLiteral("Database and SQLite sidecars must not be inside library root: %1")
                             .arg(root);
            return false;
        }
        if (!cacheDirectory.isEmpty() && lexicalPathIsInsideOrEqual(cacheDirectory, root)) {
            if (error)
                *error = QStringLiteral("Cache directory must not be inside library root: %1").arg(root);
            return false;
        }
    }
    return true;
}

} // namespace

Catalogue::Catalogue(QString databasePath, QString cacheDirectory)
    : m_databasePath(QFileInfo(databasePath).absoluteFilePath())
    , m_cacheDirectory(cacheDirectory.isEmpty() ? QString() : QFileInfo(cacheDirectory).absoluteFilePath())
    , m_connectionName(QStringLiteral("fks-catalogue-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)))
{
}

Catalogue::~Catalogue()
{
    close();
}

QString Catalogue::canonicalPath(const QString& path)
{
    QFileInfo info(path);
    QString canonical = info.canonicalFilePath();
    if (!canonical.isEmpty())
        return QDir::cleanPath(canonical);

    QStringList missing;
    QFileInfo cursor = info;
    while (!cursor.exists()) {
        missing.prepend(cursor.fileName());
        const QString parent = cursor.absolutePath();
        if (parent == cursor.absoluteFilePath())
            break;
        cursor.setFile(parent);
    }
    canonical = cursor.canonicalFilePath();
    if (canonical.isEmpty())
        canonical = cursor.absoluteFilePath();
    QDir directory(canonical);
    for (const QString& part : missing)
        canonical = directory.filePath(part), directory.setPath(canonical);
    return QDir::cleanPath(canonical);
}

QString Catalogue::normalizedPlaylistRelativePath(const QString& path)
{
    QString normalized = path;
    normalized.replace(QLatin1Char('\\'), QLatin1Char('/'));
    return QDir::cleanPath(normalized);
}

bool Catalogue::playlistSnapshotPathsMatch(const QString& firstRoot,
                                           const QString& firstRelativePath,
                                           const QString& secondRoot,
                                           const QString& secondRelativePath)
{
    // Drive-letter and mount-point changes are handled by the active-root
    // fallback. Relinking deliberately does not support case-only changes:
    // real moved-root snapshots retain the catalogue's relative-path spelling.
    return QDir::fromNativeSeparators(canonicalPath(firstRoot))
            == QDir::fromNativeSeparators(canonicalPath(secondRoot))
        && normalizedPlaylistRelativePath(firstRelativePath)
            == normalizedPlaylistRelativePath(secondRelativePath);
}

bool Catalogue::pathIsInsideOrEqual(const QString& candidate, const QString& root)
{
    QString child = canonicalPath(candidate);
    QString parent = canonicalPath(root);
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    child = child.toCaseFolded();
    parent = parent.toCaseFolded();
#endif
    if (child == parent)
        return true;
    if (!parent.endsWith(QDir::separator()))
        parent.append(QDir::separator());
    return child.startsWith(parent);
}

bool Catalogue::storageIsSafe(const QString& databasePath, const QString& cacheDirectory,
                              const QStringList& libraryRoots, QString* error)
{
    for (const QString& root : libraryRoots) {
        if (pathIsInsideOrEqual(databasePath, root)) {
            if (error)
                *error = QStringLiteral("Database and SQLite sidecars must not be inside library root: %1")
                             .arg(root);
            return false;
        }
        if (!cacheDirectory.isEmpty() && pathIsInsideOrEqual(cacheDirectory, root)) {
            if (error)
                *error = QStringLiteral("Cache directory must not be inside library root: %1").arg(root);
            return false;
        }
    }
    return true;
}

void Catalogue::setError(const QString& message, QString* error) const
{
    m_lastError = message;
    if (error)
        *error = message;
}

bool Catalogue::open(QString* error, const QStringList& libraryRoots)
{
    if (m_database.isOpen())
        return true;
    if (!storageIsSafe(m_databasePath, m_cacheDirectory, libraryRoots, error)) {
        m_lastError = error ? *error : QStringLiteral("Unsafe catalogue storage location");
        qCritical(lcCatalogue).noquote() << m_lastError;
        return false;
    }

    const QFileInfo dbInfo(m_databasePath);
    if (!QDir().mkpath(dbInfo.absolutePath())) {
        setError(QStringLiteral("Could not create database directory: %1").arg(dbInfo.absolutePath()), error);
        return false;
    }
    m_database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_database.setDatabaseName(m_databasePath);
    m_database.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000"));
    if (!m_database.open()) {
        const QString detail = m_database.lastError().text();
        close();
        if (dbInfo.exists() && indicatesCorruption(detail)
            && recoverCorruptDatabase(detail, error))
            return open(error, libraryRoots);
        setError(QStringLiteral("Could not open catalogue: %1").arg(detail), error);
        return false;
    }

    QSqlQuery versionQuery(m_database);
    if (!versionQuery.exec(QStringLiteral("PRAGMA user_version")) || !versionQuery.next()) {
        const QString detail = versionQuery.lastError().text();
        close();
        if (dbInfo.exists() && indicatesCorruption(detail)
            && recoverCorruptDatabase(detail, error))
            return open(error, libraryRoots);
        setError(QStringLiteral("Could not read catalogue schema version: %1").arg(detail), error);
        return false;
    }
    const int version = versionQuery.value(0).toInt();
    versionQuery.finish();
    if (version > SchemaVersion) {
        const QString message = QStringLiteral("Catalogue schema version %1 is newer than supported version %2; refusing to open")
                                    .arg(version).arg(SchemaVersion);
        qCritical(lcCatalogue).noquote() << message;
        close();
        setError(message, error);
        return false;
    }
    if (version > 0) {
        QStringList storedRoots;
        QSqlQuery rootsQuery(m_database);
        if (!rootsQuery.exec(QStringLiteral("SELECT path FROM library_roots"))) {
            const QString detail = rootsQuery.lastError().text();
            rootsQuery.finish();
            close();
            if (indicatesCorruption(detail) && recoverCorruptDatabase(detail, error))
                return open(error, libraryRoots);
            setError(QStringLiteral("Could not validate catalogue storage location: %1").arg(detail), error);
            return false;
        }
        while (rootsQuery.next())
            storedRoots.append(rootsQuery.value(0).toString());
        rootsQuery.finish();
        QString storageError;
        if (!storedStorageIsSafe(m_databasePath, m_cacheDirectory, storedRoots, &storageError)) {
            close();
            qCritical(lcCatalogue).noquote() << storageError;
            setError(storageError, error);
            return false;
        }
    }
    if (!m_cacheDirectory.isEmpty() && !QDir().mkpath(m_cacheDirectory)) {
        close();
        setError(QStringLiteral("Could not create cache directory: %1").arg(m_cacheDirectory), error);
        return false;
    }
    if (!ensureSchema(error)) {
        const QString detail = error ? *error : m_lastError;
        close();
        if (version == 0 && dbInfo.exists() && indicatesCorruption(detail)
            && recoverCorruptDatabase(detail, error))
            return open(error, libraryRoots);
        return false;
    }
    return true;
}

void Catalogue::close()
{
    if (!m_database.isValid())
        return;
    m_database.close();
    m_database = QSqlDatabase();
    QSqlDatabase::removeDatabase(m_connectionName);
}

bool Catalogue::isOpen() const
{
    return m_database.isOpen();
}

bool Catalogue::execute(const QString& sql, QString* error) const
{
    QSqlQuery query(m_database);
    if (query.exec(sql))
        return true;
    setError(sqlError(query, QStringLiteral("SQL statement failed")), error);
    return false;
}

bool Catalogue::ensureSchema(QString* error)
{
    if (!execute(QStringLiteral("PRAGMA foreign_keys=ON"), error)
        || !execute(QStringLiteral("PRAGMA journal_mode=WAL"), error)
        || !execute(QStringLiteral("PRAGMA busy_timeout=5000"), error))
        return false;

    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("PRAGMA user_version")) || !query.next()) {
        setError(sqlError(query, QStringLiteral("Could not inspect schema")), error);
        return false;
    }
    const int currentVersion = query.value(0).toInt();
    if (currentVersion == SchemaVersion)
        return true;

    if (!m_database.transaction()) {
        setError(QStringLiteral("Could not begin schema migration: %1").arg(m_database.lastError().text()), error);
        return false;
    }
    const QStringList statements = {
        QStringLiteral("CREATE TABLE IF NOT EXISTS library_roots(id INTEGER PRIMARY KEY, path TEXT NOT NULL UNIQUE, added_at INTEGER NOT NULL, last_scan_started INTEGER, last_scan_completed INTEGER, online INTEGER NOT NULL DEFAULT 1, active INTEGER NOT NULL DEFAULT 0)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS files(id INTEGER PRIMARY KEY, root_id INTEGER NOT NULL REFERENCES library_roots(id) ON DELETE CASCADE, rel_path TEXT NOT NULL, rel_dir TEXT NOT NULL, file_name TEXT NOT NULL, ext TEXT NOT NULL, kind TEXT NOT NULL, size INTEGER NOT NULL, mtime_ms INTEGER NOT NULL, last_seen_scan INTEGER, present INTEGER NOT NULL DEFAULT 1, tags_state TEXT NOT NULL DEFAULT 'none', raw_tags_json TEXT, crc32 INTEGER, sha256 BLOB, zip_status TEXT, zip_detail TEXT, UNIQUE(root_id, rel_path))"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS zip_members(id INTEGER PRIMARY KEY, zip_file_id INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE, name TEXT NOT NULL, method INTEGER NOT NULL, crc32 INTEGER NOT NULL, compressed_size INTEGER NOT NULL, uncompressed_size INTEGER NOT NULL, encrypted INTEGER NOT NULL, damaged INTEGER NOT NULL DEFAULT 0, kind TEXT NOT NULL)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS songs(id INTEGER PRIMARY KEY, title TEXT, artist TEXT, title_raw TEXT, artist_raw TEXT, disc_id TEXT, disc_prefix TEXT, track INTEGER NOT NULL DEFAULT 0, display_title TEXT, display_artist TEXT, search_text TEXT NOT NULL DEFAULT '', metadata_source TEXT NOT NULL DEFAULT 'fallback', confidence TEXT NOT NULL DEFAULT 'none', best_source_id INTEGER, playable INTEGER NOT NULL DEFAULT 0)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS sources(id INTEGER PRIMARY KEY, song_id INTEGER REFERENCES songs(id) ON DELETE SET NULL, root_id INTEGER NOT NULL REFERENCES library_roots(id) ON DELETE CASCADE, kind TEXT NOT NULL, mp3_file_id INTEGER REFERENCES files(id), graphics_file_id INTEGER REFERENCES files(id), zip_file_id INTEGER REFERENCES files(id), zip_mp3_member TEXT, zip_graphics_member TEXT, playable INTEGER NOT NULL DEFAULT 0, unplayable_reason TEXT, parsed_json TEXT NOT NULL, UNIQUE(root_id, kind, mp3_file_id, graphics_file_id, zip_file_id, zip_mp3_member, zip_graphics_member))"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS scan_runs(id INTEGER PRIMARY KEY, root_id INTEGER NOT NULL REFERENCES library_roots(id) ON DELETE CASCADE, started INTEGER NOT NULL, finished INTEGER, status TEXT NOT NULL, phase TEXT NOT NULL, counts_json TEXT)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_files_root_present_kind ON files(root_id, present, kind)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_files_dir_stem ON files(root_id, rel_dir, file_name)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_zip_members_file_kind ON zip_members(zip_file_id, kind)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_sources_song ON sources(song_id)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_sources_mp3_file ON sources(mp3_file_id)"),
        QStringLiteral("CREATE UNIQUE INDEX IF NOT EXISTS idx_sources_loose_identity ON sources(root_id, kind, mp3_file_id, graphics_file_id) WHERE zip_file_id IS NULL"),
        QStringLiteral("CREATE UNIQUE INDEX IF NOT EXISTS idx_sources_zip_identity ON sources(root_id, kind, zip_file_id, zip_mp3_member, zip_graphics_member) WHERE zip_file_id IS NOT NULL"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_songs_search ON songs(search_text)"),
        QStringLiteral("CREATE UNIQUE INDEX IF NOT EXISTS idx_library_roots_one_active ON library_roots(active) WHERE active=1"),
        QStringLiteral("PRAGMA user_version=4")
    };
    if (currentVersion == 1
        && !query.exec(QStringLiteral(
            "ALTER TABLE zip_members ADD COLUMN damaged INTEGER NOT NULL DEFAULT 0"))) {
        m_database.rollback();
        setError(sqlError(query, QStringLiteral("Schema migration failed")), error);
        return false;
    }
    if (currentVersion > 0 && currentVersion < 3
        && !query.exec(QStringLiteral(
            "ALTER TABLE library_roots ADD COLUMN active INTEGER NOT NULL DEFAULT 0"))) {
        m_database.rollback();
        setError(sqlError(query, QStringLiteral("Schema migration failed")), error);
        return false;
    }
    for (const QString& statement : statements) {
        if (!query.exec(statement)) {
            m_database.rollback();
            setError(sqlError(query, QStringLiteral("Schema migration failed")), error);
            return false;
        }
    }
    if (!query.exec(QStringLiteral(
            "UPDATE library_roots SET active=1 WHERE id=(SELECT min(id) FROM library_roots) "
            "AND NOT EXISTS(SELECT 1 FROM library_roots WHERE active=1)"))) {
        m_database.rollback();
        setError(sqlError(query, QStringLiteral("Schema migration failed")), error);
        return false;
    }
    if (!m_database.commit()) {
        setError(QStringLiteral("Could not commit schema migration: %1").arg(m_database.lastError().text()), error);
        return false;
    }
    return true;
}

bool Catalogue::recoverCorruptDatabase(const QString& detail, QString* error)
{
    const QString suffix = QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmsszzz"));
    const QString moved = m_databasePath + QStringLiteral(".corrupt-") + suffix;
    if (!QFile::rename(m_databasePath, moved)) {
        setError(QStringLiteral("Catalogue is corrupt (%1) and could not be moved aside").arg(detail), error);
        return false;
    }
    for (const QString& sidecar : {QStringLiteral("-wal"), QStringLiteral("-shm")}) {
        const QString path = m_databasePath + sidecar;
        if (QFileInfo::exists(path))
            QFile::rename(path, moved + sidecar);
    }
    qWarning(lcCatalogue).noquote() << "Moved corrupt catalogue to" << moved << "because:" << detail;
    return true;
}

bool Catalogue::addRoot(const QString& path, qint64* id, QString* error)
{
    if (!isOpen()) {
        setError(QStringLiteral("Catalogue is not open"), error);
        return false;
    }
    const QString root = canonicalPath(path);
    if (!QFileInfo(root).isDir()) {
        setError(QStringLiteral("Library root is not a directory: %1").arg(path), error);
        return false;
    }
    QStringList allRoots;
    for (const CatalogueRoot& existing : roots(error))
        allRoots.append(existing.path);
    allRoots.append(root);
    if (!storageIsSafe(m_databasePath, m_cacheDirectory, allRoots, error)) {
        m_lastError = error ? *error : QStringLiteral("Unsafe catalogue storage location");
        return false;
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("INSERT INTO library_roots(path, added_at, online) VALUES(?, ?, 1) ON CONFLICT(path) DO UPDATE SET online=1"));
    query.addBindValue(root);
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    if (!query.exec()) {
        setError(sqlError(query, QStringLiteral("Could not add library root")), error);
        return false;
    }
    QSqlQuery find(m_database);
    find.prepare(QStringLiteral("SELECT id FROM library_roots WHERE path=?"));
    find.addBindValue(root);
    if (!find.exec() || !find.next()) {
        setError(sqlError(find, QStringLiteral("Could not retrieve library root")), error);
        return false;
    }
    if (id)
        *id = find.value(0).toLongLong();
    QSqlQuery activateFirst(m_database);
    if (!activateFirst.exec(QStringLiteral(
            "UPDATE library_roots SET active=1 WHERE id=(SELECT min(id) FROM library_roots) "
            "AND NOT EXISTS(SELECT 1 FROM library_roots WHERE active=1)"))) {
        setError(sqlError(activateFirst, QStringLiteral("Could not select initial library root")), error);
        return false;
    }
    return true;
}

bool Catalogue::setActiveRoot(qint64 rootId, QString* error)
{
    if (!isOpen()) {
        setError(QStringLiteral("Catalogue is not open"), error);
        return false;
    }
    QSqlQuery exists(m_database);
    exists.prepare(QStringLiteral("SELECT 1 FROM library_roots WHERE id=?"));
    exists.addBindValue(rootId);
    if (!exists.exec() || !exists.next()) {
        setError(QStringLiteral("Library root was not found"), error);
        return false;
    }
    if (!m_database.transaction()) {
        setError(QStringLiteral("Could not change active library root: %1")
                     .arg(m_database.lastError().text()), error);
        return false;
    }
    QSqlQuery clear(m_database);
    if (!clear.exec(QStringLiteral("UPDATE library_roots SET active=0"))) {
        m_database.rollback();
        setError(sqlError(clear, QStringLiteral("Could not clear active library root")), error);
        return false;
    }
    QSqlQuery set(m_database);
    set.prepare(QStringLiteral("UPDATE library_roots SET active=1 WHERE id=?"));
    set.addBindValue(rootId);
    if (!set.exec() || !m_database.commit()) {
        m_database.rollback();
        setError(sqlError(set, QStringLiteral("Could not set active library root")), error);
        return false;
    }
    return true;
}

CatalogueRoot Catalogue::activeRoot(QString* error) const
{
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral(
            "SELECT id,path,online,active FROM library_roots WHERE active=1 LIMIT 1"))) {
        setError(sqlError(query, QStringLiteral("Could not read active library root")), error);
        return {};
    }
    if (!query.next())
        return {};
    return {query.value(0).toLongLong(), query.value(1).toString(),
            query.value(2).toBool(), query.value(3).toBool()};
}

QList<CatalogueRoot> Catalogue::roots(QString* error) const
{
    QList<CatalogueRoot> result;
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("SELECT id, path, online, active FROM library_roots ORDER BY id"))) {
        setError(sqlError(query, QStringLiteral("Could not list roots")), error);
        return result;
    }
    while (query.next())
        result.append({query.value(0).toLongLong(), query.value(1).toString(),
                       query.value(2).toBool(), query.value(3).toBool()});
    return result;
}

QList<CatalogueSearchRow> Catalogue::search(const QString& text, int limit,
                                            bool includeUnplayable, QString* error) const
{
    return searchImpl(text, limit, includeUnplayable, false, error);
}

QList<CatalogueSearchRow> Catalogue::searchActive(const QString& text, int limit,
                                                  QString* error) const
{
    return searchImpl(text, limit, false, true, error);
}

QList<CatalogueSearchRow> Catalogue::browseActive(QString* error) const
{
    QList<CatalogueSearchRow> result;
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral(
            "SELECT so.id,so.display_title,so.display_artist,so.disc_id,so.track,"
            "so.playable,so.confidence FROM songs so WHERE EXISTS("
            "SELECT 1 FROM sources active_source "
            "JOIN library_roots active_root ON active_root.id=active_source.root_id "
            "WHERE active_source.song_id=so.id AND active_source.kind='loose_cdg' "
            "AND (active_source.playable=1 "
            "OR active_source.unplayable_reason='root_offline') "
            "AND active_root.active=1) "
            "ORDER BY CASE WHEN trim(coalesce(so.display_artist,''))<>'' THEN 0 ELSE 1 END,"
            "lower(so.display_artist),lower(so.display_title),so.disc_id,so.track,so.id"))) {
        setError(sqlError(query, QStringLiteral("Browse failed")), error);
        return result;
    }
    while (query.next()) {
        result.append({query.value(0).toLongLong(), query.value(1).toString(),
                       query.value(2).toString(), query.value(3).toString(),
                       query.value(4).toInt(), query.value(5).toBool(),
                       query.value(6).toString()});
    }
    return result;
}

QList<CatalogueSearchRow> Catalogue::searchImpl(const QString& text, int limit,
                                                bool includeUnplayable, bool activeOnly,
                                                QString* error) const
{
    QList<CatalogueSearchRow> result;
    const QString normalized = normalizeForSearch(text);
    const QStringList tokens = normalized.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    QString sql = QStringLiteral("SELECT so.id,so.display_title,so.display_artist,so.disc_id,so.track,so.playable,so.confidence FROM songs so WHERE 1=1");
    if (!includeUnplayable && !activeOnly)
        sql += QStringLiteral(" AND so.playable=1");
    if (activeOnly) {
        sql += QStringLiteral(
            " AND EXISTS(SELECT 1 FROM sources active_source "
            "JOIN library_roots active_root ON active_root.id=active_source.root_id "
            "WHERE active_source.song_id=so.id AND active_source.kind='loose_cdg' "
            "AND (active_source.playable=1 OR active_source.unplayable_reason='root_offline') "
            "AND active_root.active=1)");
    }
    for (qsizetype i = 0; i < tokens.size(); ++i)
        sql += QStringLiteral(" AND so.search_text LIKE ? ESCAPE '\\'");
    sql += QStringLiteral(" ORDER BY CASE WHEN lower(so.display_title) LIKE ? ESCAPE '\\' OR lower(so.display_artist) LIKE ? ESCAPE '\\' THEN 0 WHEN lower(so.display_artist) LIKE ? ESCAPE '\\' THEN 1 WHEN lower(so.display_title) LIKE ? ESCAPE '\\' THEN 2 ELSE 3 END, lower(so.display_artist), lower(so.display_title) LIMIT ?");
    QSqlQuery query(m_database);
    query.prepare(sql);
    for (const QString& token : tokens)
        query.addBindValue(QStringLiteral("%") + escapedLike(token) + QStringLiteral("%"));
    const QString prefix = escapedLike(normalized) + QStringLiteral("%");
    query.addBindValue(prefix);
    query.addBindValue(prefix);
    const QString contains = QStringLiteral("%") + escapedLike(normalized) + QStringLiteral("%");
    query.addBindValue(contains);
    query.addBindValue(contains);
    query.addBindValue(qBound(1, limit, 1000));
    if (!query.exec()) {
        setError(sqlError(query, QStringLiteral("Search failed")), error);
        return result;
    }
    while (query.next()) {
        result.append({query.value(0).toLongLong(), query.value(1).toString(),
                       query.value(2).toString(), query.value(3).toString(),
                       query.value(4).toInt(), query.value(5).toBool(),
                       query.value(6).toString()});
    }
    return result;
}

PlaybackPaths Catalogue::playbackPathsFor(qint64 songId, QString* error) const
{
    return playbackPathsForImpl(songId, false, error);
}

PlaybackPaths Catalogue::activePlaybackPathsFor(qint64 songId, QString* error) const
{
    return playbackPathsForImpl(songId, true, error);
}

std::optional<SongRef> Catalogue::songRef(qint64 songId, QString* error) const
{
    if (!isOpen()) {
        setError(QStringLiteral("Catalogue is not open"), error);
        return std::nullopt;
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT so.id,so.display_title,so.display_artist,so.disc_id,so.track,"
        "r.path,mf.rel_path FROM songs so "
        "LEFT JOIN sources s ON s.song_id=so.id AND s.kind='loose_cdg' "
        "LEFT JOIN library_roots r ON r.id=s.root_id "
        "LEFT JOIN files mf ON mf.id=s.mp3_file_id "
        "WHERE so.id=? ORDER BY CASE WHEN s.id=so.best_source_id THEN 0 ELSE 1 END,"
        "mf.present DESC,s.playable DESC,s.id LIMIT 1"));
    query.addBindValue(songId);
    if (!query.exec()) {
        setError(sqlError(query, QStringLiteral("Song reference lookup failed")), error);
        return std::nullopt;
    }
    if (!query.next())
        return std::nullopt;
    return SongRef{query.value(0).toLongLong(), query.value(1).toString(),
                   query.value(2).toString(), query.value(3).toString(),
                   query.value(4).toInt(), query.value(5).toString(),
                   query.value(6).toString()};
}

qint64 Catalogue::findSongByMp3Path(const QString& rootPath, const QString& relPath,
                                    QString* error) const
{
    if (!isOpen()) {
        setError(QStringLiteral("Catalogue is not open"), error);
        return 0;
    }
    QSqlQuery rootQuery(m_database);
    rootQuery.prepare(QStringLiteral("SELECT id FROM library_roots WHERE path=?"));
    rootQuery.addBindValue(canonicalPath(rootPath));
    if (!rootQuery.exec()) {
        setError(sqlError(rootQuery, QStringLiteral("Song root lookup failed")), error);
        return 0;
    }

    QSet<qint64> matches;
    while (rootQuery.next()) {
        QSqlQuery query(m_database);
        query.prepare(playlistSongLookupSql());
        query.addBindValue(rootQuery.value(0));
        query.addBindValue(normalizedPlaylistRelativePath(relPath));
        if (!query.exec()) {
            setError(sqlError(query, QStringLiteral("Song path lookup failed")), error);
            return 0;
        }
        while (query.next())
            matches.insert(query.value(0).toLongLong());
    }
    return matches.size() == 1 ? *matches.constBegin() : 0;
}

qint64 Catalogue::findUniqueActiveSongByMp3Path(const QString& relPath,
                                                QString* error) const
{
    if (!isOpen()) {
        setError(QStringLiteral("Catalogue is not open"), error);
        return 0;
    }
    QSqlQuery rootQuery(m_database);
    if (!rootQuery.exec(QStringLiteral(
            "SELECT id FROM library_roots WHERE active=1"))) {
        setError(sqlError(rootQuery, QStringLiteral("Active song root lookup failed")), error);
        return 0;
    }

    QSet<qint64> matches;
    while (rootQuery.next()) {
        QSqlQuery query(m_database);
        query.prepare(playlistSongLookupSql());
        query.addBindValue(rootQuery.value(0));
        query.addBindValue(normalizedPlaylistRelativePath(relPath));
        if (!query.exec()) {
            setError(sqlError(query, QStringLiteral("Active song path lookup failed")), error);
            return 0;
        }
        while (query.next())
            matches.insert(query.value(0).toLongLong());
    }
    return matches.size() == 1 ? *matches.constBegin() : 0;
}

QString Catalogue::playlistSongLookupSql()
{
    return QStringLiteral(
        "SELECT s.song_id FROM files f "
        "JOIN sources s ON s.mp3_file_id=f.id "
        "WHERE f.root_id=? AND f.rel_path=? "
        "AND f.present=1 AND s.song_id IS NOT NULL");
}

PlaybackPaths Catalogue::playbackPathsForImpl(qint64 songId, bool activeOnly,
                                              QString* error) const
{
    PlaybackPaths result;
    QSqlQuery query(m_database);
    QString sql = QStringLiteral(
        "SELECT r.path,mf.rel_path,gf.rel_path FROM songs so "
        "JOIN sources s ON s.song_id=so.id JOIN library_roots r ON r.id=s.root_id "
        "JOIN files mf ON mf.id=s.mp3_file_id JOIN files gf ON gf.id=s.graphics_file_id "
        "WHERE so.id=? AND s.kind='loose_cdg' "
        "AND mf.present=1 AND gf.present=1");
    if (activeOnly) {
        sql += QStringLiteral(
            " AND r.active=1 AND (s.playable=1 OR s.unplayable_reason='root_offline')");
    } else {
        sql += QStringLiteral(" AND so.playable=1 AND s.playable=1");
    }
    sql += QStringLiteral(" ORDER BY CASE WHEN s.id=so.best_source_id THEN 0 ELSE 1 END,s.id LIMIT 1");
    query.prepare(sql);
    query.addBindValue(songId);
    if (!query.exec()) {
        setError(sqlError(query, QStringLiteral("Playback lookup failed")), error);
        return result;
    }
    if (query.next()) {
        QDir root(query.value(0).toString());
        result.mp3Path = root.filePath(query.value(1).toString());
        result.graphicsPath = root.filePath(query.value(2).toString());
        return result;
    }
    QSqlQuery reason(m_database);
    reason.prepare(QStringLiteral("SELECT COALESCE(s.unplayable_reason, 'file_missing') FROM sources s WHERE s.song_id=? ORDER BY s.playable DESC, CASE s.kind WHEN 'loose_cdg' THEN 0 WHEN 'loose_mcg' THEN 1 ELSE 2 END LIMIT 1"));
    reason.addBindValue(songId);
    if (!reason.exec()) {
        setError(sqlError(reason, QStringLiteral("Playback reason lookup failed")), error);
        return result;
    }
    result.reason = reason.next() ? reason.value(0).toString() : QStringLiteral("file_missing");
    return result;
}

qint64 Catalogue::activeSongCount(QString* error) const
{
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral(
            "SELECT count(DISTINCT s.song_id) FROM sources s "
            "JOIN library_roots r ON r.id=s.root_id "
            "WHERE r.active=1 AND s.kind='loose_cdg' "
            "AND (s.playable=1 OR s.unplayable_reason='root_offline')")) || !query.next()) {
        setError(sqlError(query, QStringLiteral("Could not count active songs")), error);
        return 0;
    }
    return query.value(0).toLongLong();
}

QVariantMap Catalogue::stats(QString* error) const
{
    QVariantMap result;
    result.insert(QStringLiteral("filesByKind"), groupedCounts(m_database, QStringLiteral("SELECT kind, count(*) FROM files WHERE present=1 GROUP BY kind"), error));
    result.insert(QStringLiteral("sourcesByKind"), groupedCounts(m_database, QStringLiteral("SELECT kind, count(*) FROM sources GROUP BY kind"), error));
    result.insert(QStringLiteral("unplayableByReason"), groupedCounts(m_database, QStringLiteral("SELECT unplayable_reason, count(*) FROM sources WHERE playable=0 GROUP BY unplayable_reason"), error));
    result.insert(QStringLiteral("zipsByStatus"), groupedCounts(m_database, QStringLiteral("SELECT COALESCE(zip_status, 'pending'), count(*) FROM files WHERE present=1 AND kind='zip' GROUP BY zip_status"), error));
    result.insert(QStringLiteral("confidence"), groupedCounts(m_database, QStringLiteral("SELECT confidence, count(*) FROM songs GROUP BY confidence"), error));
    result.insert(QStringLiteral("metadataSource"), groupedCounts(m_database, QStringLiteral("SELECT metadata_source, count(*) FROM songs GROUP BY metadata_source"), error));
    result.insert(QStringLiteral("parsedKinds"), groupedCounts(m_database, QStringLiteral("SELECT CAST(json_extract(parsed_json,'$.kind') AS TEXT), count(*) FROM sources GROUP BY 1"), error));
    auto scalar = [&](const QString& sql) -> qlonglong {
        QSqlQuery query(m_database);
        return query.exec(sql) && query.next() ? query.value(0).toLongLong() : 0;
    };
    result.insert(QStringLiteral("songs"), scalar(QStringLiteral("SELECT count(*) FROM songs")));
    result.insert(QStringLiteral("playable"), scalar(QStringLiteral("SELECT count(*) FROM songs WHERE playable=1")));
    result.insert(QStringLiteral("orphans"), scalar(QStringLiteral(
        "SELECT count(*) FROM files f WHERE f.present=1 AND f.kind IN ('mp3','cdg','mcg') "
        "AND f.id NOT IN (SELECT mp3_file_id FROM sources WHERE mp3_file_id IS NOT NULL "
        "UNION SELECT graphics_file_id FROM sources WHERE graphics_file_id IS NOT NULL)")));
    result.insert(QStringLiteral("duplicatesMerged"), scalar(QStringLiteral("SELECT count(*) FROM songs WHERE (SELECT count(*) FROM sources WHERE song_id=songs.id)>1")));
    return result;
}

QList<QVariantMap> Catalogue::sample(int count, const QString& confidence, QString* error) const
{
    QList<QVariantMap> result;
    QSqlQuery query(m_database);
    QString sql = QStringLiteral("SELECT so.id, so.title_raw, so.artist_raw, so.display_title, so.display_artist, so.metadata_source, so.confidence, s.kind, s.parsed_json, COALESCE(f.rel_path, s.zip_mp3_member) FROM songs so JOIN sources s ON s.id=so.best_source_id LEFT JOIN files f ON f.id=s.mp3_file_id");
    if (!confidence.isEmpty())
        sql += QStringLiteral(" WHERE so.confidence=?");
    sql += QStringLiteral(" ORDER BY random() LIMIT ?");
    query.prepare(sql);
    if (!confidence.isEmpty())
        query.addBindValue(confidence);
    query.addBindValue(qBound(1, count, 1000));
    if (!query.exec()) {
        setError(sqlError(query, QStringLiteral("Sample query failed")), error);
        return result;
    }
    while (query.next()) {
        QVariantMap row;
        row.insert(QStringLiteral("songId"), query.value(0));
        row.insert(QStringLiteral("titleRaw"), query.value(1));
        row.insert(QStringLiteral("artistRaw"), query.value(2));
        row.insert(QStringLiteral("title"), query.value(3));
        row.insert(QStringLiteral("artist"), query.value(4));
        row.insert(QStringLiteral("metadataSource"), query.value(5));
        row.insert(QStringLiteral("confidence"), query.value(6));
        row.insert(QStringLiteral("source"), query.value(7));
        row.insert(QStringLiteral("parsed"), query.value(8));
        row.insert(QStringLiteral("path"), query.value(9));
        result.append(row);
    }
    return result;
}

QVariantMap Catalogue::explain(const QString& relativePath, QString* error) const
{
    QVariantMap result;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT f.rel_path, f.kind, f.present, f.tags_state, f.raw_tags_json, f.zip_status, f.zip_detail, s.id, s.kind, s.playable, s.unplayable_reason, s.parsed_json, so.display_title, so.display_artist, so.metadata_source, so.confidence FROM files f LEFT JOIN sources s ON s.mp3_file_id=f.id OR s.graphics_file_id=f.id OR s.zip_file_id=f.id LEFT JOIN songs so ON so.id=s.song_id WHERE f.rel_path=? COLLATE NOCASE LIMIT 1"));
    query.addBindValue(QDir::fromNativeSeparators(relativePath));
    if (!query.exec()) {
        setError(sqlError(query, QStringLiteral("Explain query failed")), error);
        return result;
    }
    if (!query.next()) {
        setError(QStringLiteral("Path is not catalogued: %1").arg(relativePath), error);
        return result;
    }
    const QStringList keys = {QStringLiteral("path"), QStringLiteral("fileKind"), QStringLiteral("present"), QStringLiteral("tagsState"), QStringLiteral("tags"), QStringLiteral("zipStatus"), QStringLiteral("zipDetail"), QStringLiteral("sourceId"), QStringLiteral("sourceKind"), QStringLiteral("playable"), QStringLiteral("reason"), QStringLiteral("parsed"), QStringLiteral("title"), QStringLiteral("artist"), QStringLiteral("metadataSource"), QStringLiteral("confidence")};
    for (int i = 0; i < keys.size(); ++i)
        result.insert(keys.at(i), query.value(i));
    return result;
}

QByteArray Catalogue::computeSha256(qint64 fileId, QString* error)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT r.path, f.rel_path, f.sha256 FROM files f JOIN library_roots r ON r.id=f.root_id WHERE f.id=? AND f.present=1"));
    query.addBindValue(fileId);
    if (!query.exec() || !query.next()) {
        setError(QStringLiteral("File is not available for hashing"), error);
        return {};
    }
    if (!query.value(2).isNull())
        return query.value(2).toByteArray();
    QFile file(QDir(query.value(0).toString()).filePath(query.value(1).toString()));
    if (!file.open(QIODevice::ReadOnly)) {
        setError(QStringLiteral("Could not open source read-only for hashing: %1").arg(file.errorString()), error);
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        setError(QStringLiteral("Could not read source while hashing: %1").arg(file.errorString()), error);
        return {};
    }
    const QByteArray digest = hash.result();
    QSqlQuery update(m_database);
    update.prepare(QStringLiteral("UPDATE files SET sha256=? WHERE id=?"));
    update.addBindValue(digest);
    update.addBindValue(fileId);
    if (!update.exec()) {
        setError(sqlError(update, QStringLiteral("Could not store SHA-256")), error);
        return {};
    }
    return digest;
}

bool Catalogue::mergeLooseDuplicates(qint64 firstSourceId, qint64 secondSourceId, QString* error)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT id, song_id, mp3_file_id, graphics_file_id FROM sources WHERE id IN (?, ?) AND kind='loose_cdg' ORDER BY id"));
    query.addBindValue(firstSourceId);
    query.addBindValue(secondSourceId);
    if (!query.exec()) {
        setError(sqlError(query, QStringLiteral("Could not inspect duplicate candidates")), error);
        return false;
    }
    QList<QList<qint64>> rows;
    while (query.next())
        rows.append({query.value(0).toLongLong(), query.value(1).toLongLong(), query.value(2).toLongLong(), query.value(3).toLongLong()});
    if (rows.size() != 2) {
        setError(QStringLiteral("Both duplicate candidates must be loose CDG sources"), error);
        return false;
    }
    const QByteArray firstMp3 = computeSha256(rows[0][2], error);
    const QByteArray secondMp3 = computeSha256(rows[1][2], error);
    const QByteArray firstGraphics = computeSha256(rows[0][3], error);
    const QByteArray secondGraphics = computeSha256(rows[1][3], error);
    if (firstMp3.isEmpty() || secondMp3.isEmpty() || firstGraphics.isEmpty()
        || secondGraphics.isEmpty())
        return false;
    if (firstMp3 != secondMp3 || firstGraphics != secondGraphics) {
        setError(QStringLiteral("Sources are not content-identical"), error);
        return false;
    }
    QSqlQuery update(m_database);
    update.prepare(QStringLiteral("UPDATE sources SET song_id=? WHERE id=?"));
    update.addBindValue(rows[0][1]);
    update.addBindValue(rows[1][0]);
    if (!update.exec()) {
        setError(sqlError(update, QStringLiteral("Could not merge duplicate sources")), error);
        return false;
    }
    QSqlQuery cleanup(m_database);
    cleanup.prepare(QStringLiteral("DELETE FROM songs WHERE id=? AND NOT EXISTS(SELECT 1 FROM sources WHERE song_id=?)"));
    cleanup.addBindValue(rows[1][1]);
    cleanup.addBindValue(rows[1][1]);
    return cleanup.exec();
}
