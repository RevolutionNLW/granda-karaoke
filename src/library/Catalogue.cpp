#include "library/Catalogue.h"

#include "library/FilenameParser.h"
#include "library/MetadataResolver.h"

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
#include <QUrl>
#include <QUuid>

#ifdef Q_OS_WIN
#include <qt_windows.h>

#include <string>
#endif

namespace {

// Windows opens "Karaoke." and "Karaoke " as the folder "Karaoke". Paths are
// compared in that same spelling, so no such name can slip past a check.
QString withWindowsNameRules(const QString& path)
{
#ifdef Q_OS_WIN
    QStringList parts = QDir::fromNativeSeparators(path).split(QLatin1Char('/'));
    for (QString& part : parts) {
        if (part == QLatin1String(".") || part == QLatin1String(".."))
            continue;
        qsizetype end = part.size();
        while (end > 0 && (part.at(end - 1) == QLatin1Char('.') || part.at(end - 1) == QLatin1Char(' ')))
            --end;
        if (end > 0)
            part.truncate(end);
    }
    return parts.join(QLatin1Char('/'));
#else
    return path;
#endif
}

#ifdef Q_OS_WIN
// Where Windows itself takes an existing file or folder to be: junctions,
// symbolic links, substituted drive letters and short 8.3 names followed, in
// the spelling stored on disk. Empty when Windows cannot say (the drive has
// gone, say). Only a handle for reading attributes is opened.
QString windowsFinalPath(const QString& existing)
{
    const std::wstring native = QDir::toNativeSeparators(existing).toStdWString();
    const HANDLE handle = CreateFileW(native.c_str(), FILE_READ_ATTRIBUTES,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return {};
    std::wstring buffer(MAX_PATH + 1, L'\0');
    const DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    DWORD length = GetFinalPathNameByHandleW(handle, buffer.data(), DWORD(buffer.size()), flags);
    if (length >= buffer.size()) {
        buffer.assign(length + 1, L'\0');
        length = GetFinalPathNameByHandleW(handle, buffer.data(), DWORD(buffer.size()), flags);
    }
    CloseHandle(handle);
    if (length == 0 || length >= buffer.size())
        return {};
    QString path = QString::fromWCharArray(buffer.data(), qsizetype(length));
    if (path.startsWith(QLatin1String("\\\\?\\UNC\\")))
        path = QStringLiteral("\\\\") + path.mid(8);
    else if (path.startsWith(QLatin1String("\\\\?\\")))
        path = path.mid(4);
    return QDir::cleanPath(QDir::fromNativeSeparators(path));
}
#endif

// An existing file or folder's own path, links followed; empty if it does
// not exist.
QString existingCanonicalPath(const QFileInfo& info)
{
#ifdef Q_OS_WIN
    const QString final = windowsFinalPath(info.absoluteFilePath());
    if (!final.isEmpty())
        return final;
#endif
    return info.canonicalFilePath();
}

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
    path = QDir::cleanPath(QFileInfo(withWindowsNameRules(path)).absoluteFilePath());
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
    // Qt paths always use '/', on Windows too: QDir::separator() is '\\' there,
    // and no path inside the folder would ever match.
    if (!parent.endsWith(QLatin1Char('/')))
        parent.append(QLatin1Char('/'));
    return child.startsWith(parent);
}

bool storedStorageIsSafe(const QString& databasePath, const QString& cacheDirectory,
                         const QStringList& storedRoots, QString* error)
{
    // Stored roots were canonicalised when they were added.  Do not resolve
    // them again here: DB-only commands must not probe a disconnected volume.
    // The catalogue and cache paths are local and are canonicalised the same
    // way, so a symlinked spelling (/var vs /private/var) cannot slip past.
    const QString canonicalDatabase = Catalogue::canonicalPath(databasePath);
    const QString canonicalCache = cacheDirectory.isEmpty() ? QString()
                                                            : Catalogue::canonicalPath(cacheDirectory);
    for (const QString& root : storedRoots) {
        if (lexicalPathIsInsideOrEqual(databasePath, root)
            || lexicalPathIsInsideOrEqual(canonicalDatabase, root)) {
            if (error)
                *error = QStringLiteral("Database and SQLite sidecars must not be inside library root: %1")
                             .arg(root);
            return false;
        }
        if (!cacheDirectory.isEmpty() && (lexicalPathIsInsideOrEqual(cacheDirectory, root)
                                          || lexicalPathIsInsideOrEqual(canonicalCache, root))) {
            if (error)
                *error = QStringLiteral("Cache directory must not be inside library root: %1").arg(root);
            return false;
        }
    }
    return true;
}

enum class ProbeResult { Readable, Damaged, Unreadable };

// Reads the schema version and the recorded library roots of an existing
// catalogue through a read-only, immutable connection. SQLite then takes no
// locks, creates no journal, WAL or shared-memory file and never rolls back a
// hot journal, so a file that turns out to be unsafe is left exactly as found.
ProbeResult probeCatalogue(const QString& path, int* version, QStringList* roots,
                           QString* detail)
{
    const QString connection = QStringLiteral("fks-catalogue-probe-%1")
                                   .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    ProbeResult result = ProbeResult::Unreadable;
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        QUrl url = QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath());
        url.setQuery(QStringLiteral("mode=ro&immutable=1"));
        database.setDatabaseName(url.toString(QUrl::FullyEncoded));
        database.setConnectOptions(QStringLiteral("QSQLITE_OPEN_URI;QSQLITE_OPEN_READONLY"));
        auto fail = [&](const QString& text) {
            *detail = text;
            result = indicatesCorruption(text) ? ProbeResult::Damaged : ProbeResult::Unreadable;
        };
        if (!database.open()) {
            fail(database.lastError().text());
        } else {
            QSqlQuery query(database);
            if (!query.exec(QStringLiteral("PRAGMA user_version")) || !query.next()) {
                fail(query.lastError().text());
            } else {
                *version = query.value(0).toInt();
                query.finish();
                result = ProbeResult::Readable;
                // A newer catalogue is refused unopened, so its roots are not needed.
                if (*version > 0 && *version <= Catalogue::SchemaVersion) {
                    if (!query.exec(QStringLiteral("SELECT path FROM library_roots"))) {
                        fail(query.lastError().text());
                    } else {
                        while (query.next())
                            roots->append(query.value(0).toString());
                    }
                }
                query.finish();
            }
            database.close();
        }
    }
    QSqlDatabase::removeDatabase(connection);
    return result;
}

// ORDER BY for a library sort over songs `so` LEFT JOINed to song_plays `p`.
// Characters skipped at the start of a name when placing it alphabetically
// ("#SELFIE" under S, "'Til" under T, "(Everything I Do)" under E). The names
// shown are never changed. (An SQL string: the apostrophe is doubled.)
const char* const kLeadingPunctuation =
    R"('  #''"()[]{}<>.,-_*!?&@$%^+=~`/\|:;¡¿‘’“”')";

// The alphabetical key of a name, and its group: letters first (A-Z, then
// letters beyond A-Z such as Ø or É, by character), then names starting with a
// digit, then symbols, and blank names last. The groups
// keep this order in both directions; only the order within them reverses.
QString alphabeticKey(const QString& column)
{
    return QStringLiteral("lower(ltrim(coalesce(%1,''),%2))")
        .arg(column, QString::fromUtf8(kLeadingPunctuation));
}

QString alphabeticGroup(const QString& column)
{
    const QString key = alphabeticKey(column);
    return QStringLiteral("CASE WHEN trim(coalesce(%1,''))='' THEN 3 WHEN %2='' THEN 2 "
                          "WHEN substr(%2,1,1) BETWEEN 'a' AND 'z' THEN 0 "
                          "WHEN unicode(substr(%2,1,1))>127 THEN 0 "
                          "WHEN substr(%2,1,1) BETWEEN '0' AND '9' THEN 1 ELSE 2 END")
        .arg(column, key);
}

// Group, key (in the given direction), then the full name as a tie-break.
QString alphabetic(const QString& column, bool descending = false)
{
    return QStringLiteral("%1,%2%3,lower(%4)")
        .arg(alphabeticGroup(column), alphabeticKey(column),
             descending ? QStringLiteral(" DESC") : QString(), column);
}

QString sortClause(LibrarySort sort)
{
    const QString artistColumn = QStringLiteral("so.display_artist");
    const QString titleColumn = QStringLiteral("so.display_title");
    const QString blankLabel = QStringLiteral("CASE WHEN trim(coalesce(so.label,''))<>'' THEN 0 ELSE 1 END");
    const QString artist = alphabetic(artistColumn);
    const QString title = alphabetic(titleColumn);
    const QString stable = QStringLiteral("%1,lower(so.label),so.disc_id,so.track,so.id").arg(blankLabel);
    const QString byArtist = QStringLiteral("%1,%2,%3").arg(artist, title, stable);
    switch (sort) {
    case LibrarySort::ArtistAsc:
        return byArtist;
    case LibrarySort::ArtistDesc:
        return QStringLiteral("%1,%2,%3").arg(alphabetic(artistColumn, true), title, stable);
    case LibrarySort::TitleAsc:
        return QStringLiteral("%1,%2,%3").arg(title, artist, stable);
    case LibrarySort::TitleDesc:
        return QStringLiteral("%1,%2,%3").arg(alphabetic(titleColumn, true), artist, stable);
    case LibrarySort::MostPlayed:
        return QStringLiteral("coalesce(p.play_count,0) DESC,") + byArtist;
    case LibrarySort::RecentlyPlayed:
        return QStringLiteral("CASE WHEN p.last_played_ms IS NULL THEN 1 ELSE 0 END,p.last_played_ms DESC,")
            + byArtist;
    case LibrarySort::LabelAsc:
        // Labels keep their own plain order; songs within a label follow the
        // artist and song order above.
        return QStringLiteral("%1,lower(so.label),%2,%3,so.disc_id,so.track,so.id")
            .arg(blankLabel, artist, title);
    }
    return byArtist;
}

int confidenceRank(const QString& value)
{
    if (value == QLatin1String("high")) return 3;
    if (value == QLatin1String("medium")) return 2;
    if (value == QLatin1String("low")) return 1;
    return 0;
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
    QFileInfo info(withWindowsNameRules(path));
    QString canonical = existingCanonicalPath(info);
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
    canonical = existingCanonicalPath(cursor);
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
    // Qt paths always use '/', on Windows too: QDir::separator() is '\\' there,
    // and no path inside the folder would ever match.
    if (!parent.endsWith(QLatin1Char('/')))
        parent.append(QLatin1Char('/'));
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
    const bool existedNonEmpty = dbInfo.exists() && dbInfo.size() > 0;
    if (existedNonEmpty) {
        // Nothing may be opened for writing, migrated, backed up or set aside
        // until the file is known to lie outside every library root it records.
        int probedVersion = 0;
        QStringList probedRoots;
        QString detail;
        switch (probeCatalogue(m_databasePath, &probedVersion, &probedRoots, &detail)) {
        case ProbeResult::Readable: {
            if (probedVersion > SchemaVersion)
                break;  // refused below with the usual message, before any write
            QString storageError;
            if (!storedStorageIsSafe(m_databasePath, m_cacheDirectory, probedRoots, &storageError)) {
                qCritical(lcCatalogue).noquote() << storageError << "- the file was left untouched";
                setError(storageError, error);
                return false;
            }
            break;
        }
        case ProbeResult::Damaged:
            // The roots recorded inside a damaged file cannot be read. Only a
            // caller that knows the configured roots independently (and has
            // already been checked against them above) may set it aside.
            if (libraryRoots.isEmpty()) {
                const QString message = QStringLiteral(
                    "Catalogue is damaged (%1) and no library folders are known to prove it lies "
                    "outside them; it was left untouched").arg(detail);
                qCritical(lcCatalogue).noquote() << message << m_databasePath;
                setError(message, error);
                return false;
            }
            if (!recoverCorruptDatabase(detail, error))
                return false;
            return open(error, libraryRoots);
        case ProbeResult::Unreadable:
            setError(QStringLiteral("Could not read catalogue: %1").arg(detail), error);
            return false;
        }
    }
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
    // Only once the catalogue is known to sit outside every stored library
    // root may anything be written beside it (backup, cache, migration).
    if (!backupBeforeV5Migration(version, existedNonEmpty, error)) {
        close();
        return false;
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
    attachEnrichmentCache(libraryRoots);
    return true;
}

QString Catalogue::enrichmentCachePath() const
{
    return QFileInfo(m_databasePath).dir().filePath(QStringLiteral("enrichment-cache.sqlite"));
}

QString Catalogue::prepareEnrichmentCache(const QString& path)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("ATTACH DATABASE ? AS enrich"));
    query.addBindValue(path);
    if (!query.exec())
        return query.lastError().text();
    const QStringList statements = {
        QStringLiteral("PRAGMA enrich.journal_mode=WAL"),
        // Raw title-screen recognition, keyed by CDG content (quick digest and
        // size) and engine. How a reading names a song is decided afresh by
        // the resolver, so changing its rules never needs recognition again.
        QStringLiteral("CREATE TABLE IF NOT EXISTS enrich.title_screens(cdg_quick_sha256 BLOB NOT NULL,"
                       "cdg_size INTEGER NOT NULL,engine TEXT NOT NULL,status TEXT NOT NULL,"
                       "frames_json TEXT,created_at INTEGER NOT NULL,"
                       "PRIMARY KEY(cdg_quick_sha256,cdg_size,engine))"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS enrich.idx_title_screens_size ON title_screens(cdg_size)"),
        QStringLiteral("PRAGMA enrich.user_version=1"),
    };
    for (const QString& statement : statements) {
        if (!query.exec(statement)) {
            const QString detail = query.lastError().text();
            query.finish();
            QSqlQuery detach(m_database);
            detach.exec(QStringLiteral("DETACH DATABASE enrich"));
            return detail.isEmpty() ? QStringLiteral("could not prepare the cache") : detail;
        }
    }
    return {};
}

void Catalogue::attachEnrichmentCache(const QStringList& libraryRoots)
{
    m_enrichmentDurable = false;
    const QString path = enrichmentCachePath();
    QStringList roots = libraryRoots;
    for (const CatalogueRoot& root : this->roots())
        roots.append(root.path);
    QString detail;
    // (A file beside a safe catalogue is safe; checked all the same.)
    if (storageIsSafe(path, {}, roots, &detail)) {
        detail = prepareEnrichmentCache(path);
        if (!detail.isEmpty() && indicatesCorruption(detail) && m_recoveryAllowed
            && QFileInfo::exists(path)) {
            const QString moved = path + QStringLiteral(".corrupt-")
                + QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmsszzz"));
            if (QFile::rename(path, moved)) {
                for (const QString& suffix : {QStringLiteral("-wal"), QStringLiteral("-shm")}) {
                    if (QFileInfo::exists(path + suffix))
                        QFile::rename(path + suffix, moved + suffix);
                }
                qWarning(lcCatalogue).noquote() << "Set damaged enrichment cache aside as" << moved;
                detail = prepareEnrichmentCache(path);
            }
        }
    }
    if (!detail.isEmpty()) {
        // Work on without it (nothing cached this session) rather than fail.
        qWarning(lcCatalogue).noquote() << "Enrichment cache unavailable:" << detail;
        prepareEnrichmentCache(QStringLiteral(":memory:"));
        return;
    }
    m_enrichmentDurable = true;
    // Readings kept in the catalogue by earlier builds move to the cache.
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("SELECT EXISTS(SELECT 1 FROM main.title_screens)")) || !query.next()
        || !query.value(0).toBool())
        return;
    query.finish();
    if (!m_database.transaction())
        return;
    if (query.exec(QStringLiteral(
            "INSERT OR IGNORE INTO enrich.title_screens(cdg_quick_sha256,cdg_size,engine,status,frames_json,"
            "created_at) SELECT cdg_quick_sha256,cdg_size,engine,status,frames_json,created_at "
            "FROM main.title_screens"))
        && query.exec(QStringLiteral("DELETE FROM main.title_screens")) && m_database.commit()) {
        qInfo(lcCatalogue) << "Moved title-screen readings into the enrichment cache";
        return;
    }
    qWarning(lcCatalogue).noquote() << "Title-screen readings were not moved:" << query.lastError().text();
    m_database.rollback();
}

bool Catalogue::ensureCurrentTables(QString* error)
{
    // Additive tables of schema 5: created in place (no data is changed), so a
    // catalogue already at version 5 gains them without another migration.
    return execute(QStringLiteral(
               "CREATE TABLE IF NOT EXISTS song_plays(song_id INTEGER PRIMARY KEY "
               "REFERENCES songs(id) ON DELETE CASCADE,play_count INTEGER NOT NULL DEFAULT 0,"
               "last_played_ms INTEGER)"), error);
}

bool Catalogue::backupBeforeV5Migration(int currentVersion, bool existedNonEmpty,
                                        QString* error)
{
    if (!existedNonEmpty || currentVersion <= 0 || currentVersion >= SchemaVersion)
        return true;
    const QString stamp = QDateTime::currentDateTime().toString(
        QStringLiteral("yyyyMMdd-HHmmss"));
    const QString backupPath = m_databasePath + QStringLiteral(".pre-v5-")
        + stamp + QStringLiteral(".bak");
    QSqlQuery backup(m_database);
    backup.prepare(QStringLiteral("VACUUM INTO ?"));
    backup.addBindValue(backupPath);
    if (!backup.exec()) {
        setError(sqlError(backup, QStringLiteral(
            "Could not create required pre-v5 catalogue backup; migration refused")), error);
        return false;
    }
    qInfo(lcCatalogue).noquote() << "Created pre-v5 catalogue backup at" << backupPath;
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
        return ensureCurrentTables(error);

    if (!m_database.transaction()) {
        setError(QStringLiteral("Could not begin schema migration: %1").arg(m_database.lastError().text()), error);
        return false;
    }
    const QStringList statements = {
        QStringLiteral("CREATE TABLE IF NOT EXISTS library_roots(id INTEGER PRIMARY KEY, path TEXT NOT NULL UNIQUE, added_at INTEGER NOT NULL, last_scan_started INTEGER, last_scan_completed INTEGER, online INTEGER NOT NULL DEFAULT 1, active INTEGER NOT NULL DEFAULT 0)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS files(id INTEGER PRIMARY KEY, root_id INTEGER NOT NULL REFERENCES library_roots(id) ON DELETE CASCADE, rel_path TEXT NOT NULL, rel_dir TEXT NOT NULL, file_name TEXT NOT NULL, ext TEXT NOT NULL, kind TEXT NOT NULL, size INTEGER NOT NULL, mtime_ms INTEGER NOT NULL, last_seen_scan INTEGER, present INTEGER NOT NULL DEFAULT 1, tags_state TEXT NOT NULL DEFAULT 'none', raw_tags_json TEXT, crc32 INTEGER, sha256 BLOB, zip_status TEXT, zip_detail TEXT, quick_sha256 BLOB, content_sha256 BLOB, cdg_packets INTEGER, UNIQUE(root_id, rel_path))"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS zip_members(id INTEGER PRIMARY KEY, zip_file_id INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE, name TEXT NOT NULL, method INTEGER NOT NULL, crc32 INTEGER NOT NULL, compressed_size INTEGER NOT NULL, uncompressed_size INTEGER NOT NULL, encrypted INTEGER NOT NULL, damaged INTEGER NOT NULL DEFAULT 0, kind TEXT NOT NULL)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS songs(id INTEGER PRIMARY KEY, title TEXT, artist TEXT, title_raw TEXT, artist_raw TEXT, disc_id TEXT, disc_prefix TEXT, track INTEGER NOT NULL DEFAULT 0, display_title TEXT, display_artist TEXT, search_text TEXT NOT NULL DEFAULT '', metadata_source TEXT NOT NULL DEFAULT 'fallback', confidence TEXT NOT NULL DEFAULT 'unresolved', best_source_id INTEGER, playable INTEGER NOT NULL DEFAULT 0,auto_title TEXT,auto_artist TEXT,auto_source TEXT,auto_confidence TEXT,resolver_version INTEGER NOT NULL DEFAULT 0,conflict INTEGER NOT NULL DEFAULT 0,evidence_json TEXT,manual_title TEXT,manual_artist TEXT,manual_updated_at INTEGER,base_confidence TEXT,label TEXT,series TEXT,label_source TEXT,manual_label TEXT,manual_series TEXT,manual_disc_id TEXT,manual_track INTEGER,manual_origin TEXT,auto_label TEXT,auto_series TEXT,auto_label_source TEXT,auto_disc_id TEXT,auto_track INTEGER)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS sources(id INTEGER PRIMARY KEY, song_id INTEGER REFERENCES songs(id) ON DELETE SET NULL, root_id INTEGER NOT NULL REFERENCES library_roots(id) ON DELETE CASCADE, kind TEXT NOT NULL, mp3_file_id INTEGER REFERENCES files(id), graphics_file_id INTEGER REFERENCES files(id), zip_file_id INTEGER REFERENCES files(id), zip_mp3_member TEXT, zip_graphics_member TEXT, playable INTEGER NOT NULL DEFAULT 0, unplayable_reason TEXT, parsed_json TEXT NOT NULL, UNIQUE(root_id, kind, mp3_file_id, graphics_file_id, zip_file_id, zip_mp3_member, zip_graphics_member))"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS scan_runs(id INTEGER PRIMARY KEY, root_id INTEGER NOT NULL REFERENCES library_roots(id) ON DELETE CASCADE, started INTEGER NOT NULL, finished INTEGER, status TEXT NOT NULL, phase TEXT NOT NULL, counts_json TEXT)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS catalogue_meta(key TEXT PRIMARY KEY,value TEXT)"),
        // Parsed track-list text files found beside songs. A file is re-read
        // only when its size or mtime changes; rows cascade with the file.
        QStringLiteral("CREATE TABLE IF NOT EXISTS sidecar_files(file_id INTEGER PRIMARY KEY REFERENCES files(id) ON DELETE CASCADE,size INTEGER NOT NULL,mtime_ms INTEGER NOT NULL,state TEXT NOT NULL,disc_id TEXT,detail TEXT)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS sidecar_entries(id INTEGER PRIMARY KEY,file_id INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE,disc_id TEXT NOT NULL,track INTEGER NOT NULL,fields_json TEXT NOT NULL,line INTEGER NOT NULL)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_sidecar_entries_file ON sidecar_entries(file_id)"),
        // Raw local-OCR output for CDG title screens, keyed by CDG content (not
        // path) so it survives renames and can be exported to another install.
        QStringLiteral("CREATE TABLE IF NOT EXISTS title_screens(cdg_quick_sha256 BLOB NOT NULL,cdg_size INTEGER NOT NULL,engine TEXT NOT NULL,status TEXT NOT NULL,frames_json TEXT,created_at INTEGER NOT NULL,PRIMARY KEY(cdg_quick_sha256,cdg_size,engine))"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_files_quick ON files(quick_sha256) WHERE quick_sha256 IS NOT NULL"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_files_root_present_kind ON files(root_id, present, kind)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_files_dir_stem ON files(root_id, rel_dir, file_name)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_zip_members_file_kind ON zip_members(zip_file_id, kind)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_sources_song ON sources(song_id)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_sources_mp3_file ON sources(mp3_file_id)"),
        QStringLiteral("CREATE UNIQUE INDEX IF NOT EXISTS idx_sources_loose_identity ON sources(root_id, kind, mp3_file_id, graphics_file_id) WHERE zip_file_id IS NULL"),
        QStringLiteral("CREATE UNIQUE INDEX IF NOT EXISTS idx_sources_zip_identity ON sources(root_id, kind, zip_file_id, zip_mp3_member, zip_graphics_member) WHERE zip_file_id IS NOT NULL"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_songs_search ON songs(search_text)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_songs_confidence ON songs(confidence)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_songs_conflict ON songs(conflict)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_files_content ON files(content_sha256) WHERE content_sha256 IS NOT NULL"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_files_kind_size ON files(kind, size)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_sources_graphics_file ON sources(graphics_file_id)"),
        QStringLiteral("CREATE UNIQUE INDEX IF NOT EXISTS idx_library_roots_one_active ON library_roots(active) WHERE active=1"),
        QStringLiteral("PRAGMA user_version=5")
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
    if (currentVersion > 0 && currentVersion < 5) {
        QSet<QString> existingColumns;
        QSqlQuery tableInfo(m_database);
        if (!tableInfo.exec(QStringLiteral("PRAGMA table_info(songs)"))) {
            m_database.rollback();
            setError(sqlError(tableInfo, QStringLiteral("Schema migration failed")), error);
            return false;
        }
        while (tableInfo.next())
            existingColumns.insert(tableInfo.value(1).toString());
        const QList<QPair<QString, QString>> columns = {
            {QStringLiteral("auto_title"), QStringLiteral("TEXT")},
            {QStringLiteral("auto_artist"), QStringLiteral("TEXT")},
            {QStringLiteral("auto_source"), QStringLiteral("TEXT")},
            {QStringLiteral("auto_confidence"), QStringLiteral("TEXT")},
            {QStringLiteral("resolver_version"), QStringLiteral("INTEGER NOT NULL DEFAULT 0")},
            {QStringLiteral("conflict"), QStringLiteral("INTEGER NOT NULL DEFAULT 0")},
            {QStringLiteral("evidence_json"), QStringLiteral("TEXT")},
            {QStringLiteral("manual_title"), QStringLiteral("TEXT")},
            {QStringLiteral("manual_artist"), QStringLiteral("TEXT")},
            {QStringLiteral("manual_updated_at"), QStringLiteral("INTEGER")},
            {QStringLiteral("base_confidence"), QStringLiteral("TEXT")},
            {QStringLiteral("label"), QStringLiteral("TEXT")},
            {QStringLiteral("series"), QStringLiteral("TEXT")},
            {QStringLiteral("label_source"), QStringLiteral("TEXT")},
            {QStringLiteral("manual_label"), QStringLiteral("TEXT")},
            {QStringLiteral("manual_series"), QStringLiteral("TEXT")},
            {QStringLiteral("manual_disc_id"), QStringLiteral("TEXT")},
            {QStringLiteral("manual_track"), QStringLiteral("INTEGER")},
            {QStringLiteral("manual_origin"), QStringLiteral("TEXT")},
            {QStringLiteral("auto_label"), QStringLiteral("TEXT")},
            {QStringLiteral("auto_series"), QStringLiteral("TEXT")},
            {QStringLiteral("auto_label_source"), QStringLiteral("TEXT")},
            {QStringLiteral("auto_disc_id"), QStringLiteral("TEXT")},
            {QStringLiteral("auto_track"), QStringLiteral("INTEGER")}};
        for (const auto& column : columns) {
            if (existingColumns.isEmpty() || existingColumns.contains(column.first))
                continue;
            if (!query.exec(QStringLiteral("ALTER TABLE songs ADD COLUMN %1 %2")
                                .arg(column.first, column.second))) {
                m_database.rollback();
                setError(sqlError(query, QStringLiteral("Schema migration failed")), error);
                return false;
            }
        }
        // Content digests used to prove duplicate copies. They are caches of
        // file content and are cleared whenever a file's size or mtime changes.
        QSet<QString> fileColumns;
        if (!tableInfo.exec(QStringLiteral("PRAGMA table_info(files)"))) {
            m_database.rollback();
            setError(sqlError(tableInfo, QStringLiteral("Schema migration failed")), error);
            return false;
        }
        while (tableInfo.next())
            fileColumns.insert(tableInfo.value(1).toString());
        const QList<QPair<QString, QString>> newFileColumns = {
            {QStringLiteral("quick_sha256"), QStringLiteral("BLOB")},
            {QStringLiteral("content_sha256"), QStringLiteral("BLOB")},
            {QStringLiteral("cdg_packets"), QStringLiteral("INTEGER")}};
        for (const auto& column : newFileColumns) {
            // No files table yet: it is created below with every column.
            if (fileColumns.isEmpty() || fileColumns.contains(column.first))
                continue;
            if (!query.exec(QStringLiteral("ALTER TABLE files ADD COLUMN %1 %2")
                                .arg(column.first, column.second))) {
                m_database.rollback();
                setError(sqlError(query, QStringLiteral("Schema migration failed")), error);
                return false;
            }
        }
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
    if (!query.exec(QStringLiteral(
            "UPDATE songs SET confidence='unresolved' WHERE confidence='none'"))) {
        m_database.rollback();
        setError(sqlError(query, QStringLiteral("Schema migration failed")), error);
        return false;
    }
    // Until the first v5 resolve, the existing names are the automatic layer.
    if (!query.exec(QStringLiteral(
            "UPDATE songs SET auto_title=COALESCE(auto_title,title),"
            "auto_artist=COALESCE(auto_artist,artist),"
            "auto_source=COALESCE(auto_source,metadata_source),"
            "auto_confidence=COALESCE(auto_confidence,confidence),"
            "auto_disc_id=COALESCE(auto_disc_id,disc_id),auto_track=COALESCE(auto_track,track)"))) {
        m_database.rollback();
        setError(sqlError(query, QStringLiteral("Schema migration failed")), error);
        return false;
    }
    if (!m_database.commit()) {
        setError(QStringLiteral("Could not commit schema migration: %1").arg(m_database.lastError().text()), error);
        return false;
    }
    return ensureCurrentTables(error);
}

bool Catalogue::recoverCorruptDatabase(const QString& detail, QString* error)
{
    if (!m_recoveryAllowed) {
        const QString message = QStringLiteral(
            "Catalogue is damaged (%1); it was left untouched").arg(detail);
        qCritical(lcCatalogue).noquote() << message << m_databasePath;
        setError(message, error);
        return false;
    }
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
            "SELECT id,path,online,active,last_scan_completed FROM library_roots WHERE active=1 LIMIT 1"))) {
        setError(sqlError(query, QStringLiteral("Could not read active library root")), error);
        return {};
    }
    if (!query.next())
        return {};
    return {query.value(0).toLongLong(), query.value(1).toString(),
            query.value(2).toBool(), query.value(3).toBool(), query.value(4).toLongLong()};
}

QList<CatalogueRoot> Catalogue::roots(QString* error) const
{
    QList<CatalogueRoot> result;
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral(
            "SELECT id, path, online, active, last_scan_completed FROM library_roots ORDER BY id"))) {
        setError(sqlError(query, QStringLiteral("Could not list roots")), error);
        return result;
    }
    while (query.next())
        result.append({query.value(0).toLongLong(), query.value(1).toString(),
                       query.value(2).toBool(), query.value(3).toBool(),
                       query.value(4).toLongLong()});
    return result;
}

QList<CatalogueSearchRow> Catalogue::search(const QString& text, int limit,
                                            bool includeUnplayable, QString* error) const
{
    return searchImpl(text, limit, includeUnplayable, false, error);
}

QList<CatalogueSearchRow> Catalogue::searchActive(const QString& text, int limit,
                                                  QString* error,
                                                  std::optional<LibrarySort> sort) const
{
    return searchImpl(text, limit, false, true, error, sort);
}

QList<CatalogueSearchRow> Catalogue::browseActive(QString* error, LibrarySort sort) const
{
    QList<CatalogueSearchRow> result;
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral(
            "SELECT so.id,so.display_title,so.display_artist,so.disc_id,so.track,"
            "so.playable,so.confidence,so.label,so.series FROM songs so "
            "LEFT JOIN song_plays p ON p.song_id=so.id WHERE EXISTS("
            "SELECT 1 FROM sources active_source "
            "JOIN library_roots active_root ON active_root.id=active_source.root_id "
            "WHERE active_source.song_id=so.id AND active_source.kind='loose_cdg' "
            "AND (active_source.playable=1 "
            "OR active_source.unplayable_reason='root_offline') "
            "AND active_root.active=1) ORDER BY ") + sortClause(sort))) {
        setError(sqlError(query, QStringLiteral("Browse failed")), error);
        return result;
    }
    while (query.next()) {
        result.append({query.value(0).toLongLong(), query.value(1).toString(),
                       query.value(2).toString(), query.value(3).toString(),
                       query.value(4).toInt(), query.value(5).toBool(),
                       query.value(6).toString(), query.value(7).toString(),
                       query.value(8).toString()});
    }
    return result;
}

QList<CatalogueSearchRow> Catalogue::searchImpl(const QString& text, int limit,
                                                bool includeUnplayable, bool activeOnly,
                                                QString* error,
                                                std::optional<LibrarySort> sort) const
{
    QList<CatalogueSearchRow> result;
    const QString normalized = normalizeForSearch(text);
    const QStringList tokens = normalized.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    QString sql = QStringLiteral("SELECT so.id,so.display_title,so.display_artist,so.disc_id,so.track,so.playable,so.confidence,so.label,so.series FROM songs so LEFT JOIN song_plays p ON p.song_id=so.id WHERE 1=1");
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
    if (sort)
        sql += QStringLiteral(" ORDER BY ") + sortClause(*sort) + QStringLiteral(" LIMIT ?");
    else
        sql += QStringLiteral(" ORDER BY CASE WHEN lower(so.display_title) LIKE ? ESCAPE '\\' OR lower(so.display_artist) LIKE ? ESCAPE '\\' THEN 0 WHEN lower(so.display_artist) LIKE ? ESCAPE '\\' THEN 1 WHEN lower(so.display_title) LIKE ? ESCAPE '\\' THEN 2 ELSE 3 END, lower(so.display_artist), lower(so.display_title) LIMIT ?");
    QSqlQuery query(m_database);
    query.prepare(sql);
    for (const QString& token : tokens)
        query.addBindValue(QStringLiteral("%") + escapedLike(token) + QStringLiteral("%"));
    if (!sort) {
        const QString prefix = escapedLike(normalized) + QStringLiteral("%");
        query.addBindValue(prefix);
        query.addBindValue(prefix);
        const QString contains = QStringLiteral("%") + escapedLike(normalized) + QStringLiteral("%");
        query.addBindValue(contains);
        query.addBindValue(contains);
    }
    query.addBindValue(qBound(1, limit, 1000));
    if (!query.exec()) {
        setError(sqlError(query, QStringLiteral("Search failed")), error);
        return result;
    }
    while (query.next()) {
        result.append({query.value(0).toLongLong(), query.value(1).toString(),
                       query.value(2).toString(), query.value(3).toString(),
                       query.value(4).toInt(), query.value(5).toBool(),
                       query.value(6).toString(), query.value(7).toString(),
                       query.value(8).toString()});
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
        "r.path,mf.rel_path,so.label FROM songs so "
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
                   query.value(6).toString(), query.value(7).toString()};
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

QString Catalogue::catalogueMeta(const QString& key, QString* error) const
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT value FROM catalogue_meta WHERE key=?"));
    query.addBindValue(key);
    if (!query.exec()) {
        setError(sqlError(query, QStringLiteral("Could not read catalogue metadata")), error);
        return {};
    }
    return query.next() ? query.value(0).toString() : QString();
}

bool Catalogue::setCatalogueMeta(const QString& key, const QString& value, QString* error)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("INSERT INTO catalogue_meta(key,value) VALUES(?,?) "
                                 "ON CONFLICT(key) DO UPDATE SET value=excluded.value"));
    query.addBindValue(key);
    query.addBindValue(value);
    if (!query.exec()) {
        setError(sqlError(query, QStringLiteral("Could not write catalogue metadata")), error);
        return false;
    }
    return true;
}

void Catalogue::setBusyTimeout(int milliseconds)
{
    execute(QStringLiteral("PRAGMA busy_timeout=%1").arg(qMax(0, milliseconds)));
}

bool Catalogue::setPlayStats(qint64 songId, const SongPlayStats& stats, QString* error)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO song_plays(song_id,play_count,last_played_ms) "
        "SELECT id,?,? FROM songs WHERE id=? "
        "ON CONFLICT(song_id) DO UPDATE SET play_count=max(play_count,excluded.play_count),"
        "last_played_ms=max(coalesce(last_played_ms,0),excluded.last_played_ms)"));
    query.addBindValue(stats.playCount);
    query.addBindValue(stats.lastPlayedMs);
    query.addBindValue(songId);
    if (!query.exec()) {
        setError(sqlError(query, QStringLiteral("Could not store play statistics")), error);
        return false;
    }
    return true;
}

bool Catalogue::rebuildPlayProjection(const QList<PlayHistoryEntry>& history,
                                      QList<PlayHistoryEntry>* unmatched, QString* error)
{
    if (!m_database.transaction()) {
        setError(QStringLiteral("Could not begin play history projection: %1")
                     .arg(m_database.lastError().text()), error);
        return false;
    }
    QSqlQuery clear(m_database);
    if (!clear.exec(QStringLiteral("DELETE FROM song_plays"))) {
        m_database.rollback();
        setError(sqlError(clear, QStringLiteral("Could not reset play statistics")), error);
        return false;
    }
    QSqlQuery find(m_database);
    find.prepare(QStringLiteral(
        "SELECT DISTINCT so.id FROM files m INDEXED BY idx_files_kind_size "
        "JOIN sources s ON s.mp3_file_id=m.id AND s.kind='loose_cdg' "
        "JOIN songs so ON so.id=s.song_id JOIN files g ON g.id=s.graphics_file_id "
        "WHERE m.kind='mp3' AND m.size=? AND m.rel_path=? AND g.size=? AND m.present=1 AND g.present=1"));
    for (const PlayHistoryEntry& entry : history) {
        find.addBindValue(entry.mp3Size);
        find.addBindValue(entry.mp3RelPath);
        find.addBindValue(entry.cdgSize);
        if (!find.exec()) {
            m_database.rollback();
            setError(sqlError(find, QStringLiteral("Could not look up play history")), error);
            return false;
        }
        QList<qint64> songs;
        while (find.next())
            songs.append(find.value(0).toLongLong());
        find.finish();
        if (songs.isEmpty() && unmatched)
            unmatched->append(entry);
        for (const qint64 songId : std::as_const(songs)) {
            if (!setPlayStats(songId, {entry.playCount, entry.lastPlayedMs}, error)) {
                m_database.rollback();
                return false;
            }
        }
    }
    if (!m_database.commit()) {
        setError(QStringLiteral("Could not commit play history projection: %1")
                     .arg(m_database.lastError().text()), error);
        return false;
    }
    return true;
}

QList<Catalogue::PlayCandidate> Catalogue::playCandidates(qint64 mp3Size, qint64 cdgSize,
                                                          QString* error) const
{
    QList<PlayCandidate> result;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT so.id,r.path,m.rel_path,g.rel_path FROM files m INDEXED BY idx_files_kind_size "
        "JOIN sources s ON s.mp3_file_id=m.id AND s.kind='loose_cdg' "
        "JOIN songs so ON so.id=s.song_id JOIN files g ON g.id=s.graphics_file_id "
        "JOIN library_roots r ON r.id=s.root_id "
        "WHERE m.kind='mp3' AND m.size=? AND g.size=? AND m.present=1 AND g.present=1 AND r.online=1 "
        "ORDER BY so.id"));
    query.addBindValue(mp3Size);
    query.addBindValue(cdgSize);
    if (!query.exec()) {
        setError(sqlError(query, QStringLiteral("Could not find play history candidates")), error);
        return result;
    }
    while (query.next())
        result.append({query.value(0).toLongLong(), query.value(1).toString(),
                       query.value(2).toString(), query.value(3).toString()});
    return result;
}

SongPlayStats Catalogue::playStats(qint64 songId, QString* error) const
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT play_count,last_played_ms FROM song_plays WHERE song_id=?"));
    query.addBindValue(songId);
    if (!query.exec()) {
        setError(sqlError(query, QStringLiteral("Could not read play statistics")), error);
        return {};
    }
    if (!query.next())
        return {};
    return {query.value(0).toInt(), query.value(1).toLongLong()};
}

bool Catalogue::hasSongs(QString* error) const
{
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("SELECT EXISTS(SELECT 1 FROM songs)")) || !query.next()) {
        setError(sqlError(query, QStringLiteral("Could not inspect songs")), error);
        return false;
    }
    return query.value(0).toBool();
}

bool Catalogue::ensureSongRows(qint64 rootId, QString* error)
{
    QSqlQuery missing(m_database);
    QString sql = QStringLiteral("SELECT id FROM sources WHERE song_id IS NULL");
    if (rootId >= 0)
        sql += QStringLiteral(" AND root_id=?");
    missing.prepare(sql);
    if (rootId >= 0)
        missing.addBindValue(rootId);
    if (!missing.exec()) {
        setError(sqlError(missing, QStringLiteral("Could not find sources without songs")), error);
        return false;
    }
    QList<qint64> sourceIds;
    while (missing.next())
        sourceIds.append(missing.value(0).toLongLong());
    missing.finish();
    if (!m_database.transaction()) {
        setError(QStringLiteral("Could not begin song creation transaction: %1")
                     .arg(m_database.lastError().text()), error);
        return false;
    }
    QSqlQuery create(m_database);
    QSqlQuery attach(m_database);
    attach.prepare(QStringLiteral("UPDATE sources SET song_id=? WHERE id=?"));
    for (qint64 sourceId : std::as_const(sourceIds)) {
        if (!create.exec(QStringLiteral("INSERT INTO songs(search_text) VALUES('')"))) {
            m_database.rollback();
            setError(sqlError(create, QStringLiteral("Could not create song")), error);
            return false;
        }
        attach.bindValue(0, create.lastInsertId());
        attach.bindValue(1, sourceId);
        if (!attach.exec()) {
            m_database.rollback();
            setError(sqlError(attach, QStringLiteral("Could not attach song")), error);
            return false;
        }
    }
    QSqlQuery cleanup(m_database);
    if (!cleanup.exec(QStringLiteral(
            "DELETE FROM songs WHERE NOT EXISTS(SELECT 1 FROM sources WHERE song_id=songs.id)"))) {
        m_database.rollback();
        setError(sqlError(cleanup, QStringLiteral("Could not remove empty songs")), error);
        return false;
    }
    if (!m_database.commit()) {
        setError(QStringLiteral("Could not commit song creation: %1")
                     .arg(m_database.lastError().text()), error);
        return false;
    }
    return true;
}

bool Catalogue::recomputeEffectiveSong(qint64 songId, QString* error)
{
    // Shown values: a trusted manual/imported value wherever one is set,
    // otherwise the automatic value the resolver stored.
    QSqlQuery song(m_database);
    song.prepare(QStringLiteral(
        "SELECT auto_title,auto_artist,auto_source,auto_confidence,manual_title,manual_artist,"
        "auto_disc_id,auto_track,best_source_id,auto_label,auto_series,auto_label_source,"
        "manual_label,manual_series,manual_disc_id,manual_track,COALESCE(manual_origin,'manual') "
        "FROM songs WHERE id=?"));
    song.addBindValue(songId);
    if (!song.exec() || !song.next()) {
        setError(song.lastError().isValid()
                     ? sqlError(song, QStringLiteral("Could not load song metadata"))
                     : QStringLiteral("Song was not found"), error);
        return false;
    }
    const QString autoTitle = song.value(0).toString();
    const QString autoArtist = song.value(1).toString();
    const QString autoSource = song.value(2).toString();
    const QString autoConfidence = song.value(3).toString().isEmpty()
        ? QStringLiteral("unresolved") : song.value(3).toString();
    const QVariant manualTitle = song.value(4);
    const QVariant manualArtist = song.value(5);
    const qint64 bestSourceId = song.value(8).toLongLong();
    const QVariant manualLabel = song.value(12);
    const QVariant manualSeries = song.value(13);
    const QVariant manualDisc = song.value(14);
    const QVariant manualTrack = song.value(15);
    const QString origin = song.value(16).toString();
    const QString discId = manualDisc.isNull() ? song.value(6).toString() : manualDisc.toString();
    const int track = manualTrack.isNull() ? song.value(7).toInt() : manualTrack.toInt();
    const QVariant label = manualLabel.isNull() ? song.value(9) : manualLabel;
    const QVariant series = manualSeries.isNull() ? song.value(10) : manualSeries;
    const QVariant labelSource = manualLabel.isNull() ? song.value(11) : QVariant(origin);
    const bool trustedName = !manualTitle.isNull() || !manualArtist.isNull();
    const QString effectiveTitle = manualTitle.isNull() ? autoTitle : manualTitle.toString();
    const QString effectiveArtist = manualArtist.isNull() ? autoArtist : manualArtist.toString();

    QString relDir;
    QJsonObject parsed;
    QJsonObject tags;
    QSqlQuery source(m_database);
    source.prepare(QStringLiteral(
        "SELECT COALESCE(f.rel_dir,''),s.parsed_json,COALESCE(f.raw_tags_json,'') "
        "FROM sources s LEFT JOIN files f ON f.id=s.mp3_file_id WHERE s.id=?"));
    source.addBindValue(bestSourceId);
    if (source.exec() && source.next()) {
        relDir = source.value(0).toString();
        parsed = QJsonDocument::fromJson(source.value(1).toByteArray()).object();
        tags = QJsonDocument::fromJson(source.value(2).toByteArray()).object();
    }
    MetadataResolver::SearchInputs search;
    search.effectiveTitle = effectiveTitle;
    search.effectiveArtist = effectiveArtist;
    search.autoTitle = autoTitle;
    search.autoArtist = autoArtist;
    search.manualTitle = manualTitle;
    search.manualArtist = manualArtist;
    search.discId = discId;
    search.track = track;
    search.relDir = relDir;
    search.parsed = parsed;
    search.tags = tags;
    search.extra = {label.toString(), series.toString(), song.value(6).toString(),
                    song.value(9).toString(), song.value(10).toString()};
    const QString searchText = MetadataResolver::buildSearchText(search);
    // The display form of an automatic artist is what the resolver shows, so
    // clearing a correction restores exactly the automatic presentation.
    const QString displayedArtist = manualArtist.isNull()
        ? MetadataResolver::displayArtistName(autoArtist) : manualArtist.toString();
    QSqlQuery update(m_database);
    update.prepare(QStringLiteral(
        "UPDATE songs SET title=?,artist=?,display_title=?,display_artist=?,search_text=?,"
        "metadata_source=?,confidence=?,disc_id=?,track=?,label=?,series=?,label_source=? "
        "WHERE id=?"));
    update.addBindValue(effectiveTitle);
    update.addBindValue(effectiveArtist);
    update.addBindValue(effectiveTitle);
    update.addBindValue(displayedArtist);
    update.addBindValue(searchText);
    update.addBindValue(trustedName ? origin : autoSource);
    update.addBindValue(!manualTitle.isNull() ? QStringLiteral("high") : autoConfidence);
    update.addBindValue(discId);
    update.addBindValue(track);
    update.addBindValue(label);
    update.addBindValue(series);
    update.addBindValue(labelSource);
    update.addBindValue(songId);
    if (update.exec())
        return true;
    setError(sqlError(update, QStringLiteral("Could not recompute effective metadata")), error);
    return false;
}

std::optional<MetadataOverride> Catalogue::metadataOverrideSnapshot(
    qint64 songId, QString* error) const
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT r.path,f.rel_path,so.auto_artist,so.auto_title,so.auto_disc_id,so.auto_track,"
        "f.file_name FROM songs so JOIN sources s ON s.song_id=so.id "
        "JOIN library_roots r ON r.id=s.root_id JOIN files f ON f.id=s.mp3_file_id "
        "WHERE so.id=? ORDER BY CASE WHEN s.id=so.best_source_id THEN 0 ELSE 1 END,s.id LIMIT 1"));
    query.addBindValue(songId);
    if (!query.exec()) {
        setError(sqlError(query, QStringLiteral("Could not snapshot song metadata")), error);
        return std::nullopt;
    }
    if (!query.next()) {
        setError(QStringLiteral("Song has no durable loose-file identity"), error);
        return std::nullopt;
    }
    MetadataOverride result;
    result.rootPath = query.value(0).toString();
    result.mp3RelPath = normalizedPlaylistRelativePath(query.value(1).toString());
    result.autoArtist = query.value(2).toString();
    result.autoTitle = query.value(3).toString();
    result.discId = query.value(4).toString();
    result.track = query.value(5).toInt();
    result.fileName = query.value(6).toString();
    return result;
}

namespace {

QVariant optionalValue(const std::optional<QString>& value)
{
    return value ? QVariant(*value) : QVariant();
}

QVariant optionalValue(const std::optional<int>& value)
{
    return value ? QVariant(*value) : QVariant();
}

const QString kSetTrusted = QStringLiteral(
    "UPDATE songs SET manual_artist=?,manual_title=?,manual_label=?,manual_series=?,"
    "manual_disc_id=?,manual_track=?,manual_origin=?,manual_updated_at=? WHERE id=?");

void bindTrusted(QSqlQuery& query, const MetadataOverride& value, qint64 updatedAt, qint64 songId)
{
    query.addBindValue(optionalValue(value.artist));
    query.addBindValue(optionalValue(value.title));
    query.addBindValue(optionalValue(value.label));
    query.addBindValue(optionalValue(value.series));
    query.addBindValue(optionalValue(value.trustedDiscId));
    query.addBindValue(optionalValue(value.trustedTrack));
    query.addBindValue(value.origin.isEmpty() ? QStringLiteral("manual") : value.origin);
    query.addBindValue(updatedAt > 0 ? updatedAt : QDateTime::currentMSecsSinceEpoch());
    query.addBindValue(songId);
}

const QString kClearTrusted = QStringLiteral(
    "manual_artist=NULL,manual_title=NULL,manual_label=NULL,manual_series=NULL,"
    "manual_disc_id=NULL,manual_track=NULL,manual_origin=NULL,manual_updated_at=NULL");

} // namespace

bool Catalogue::setManualOverride(qint64 songId,
                                  const std::optional<QString>& artist,
                                  const std::optional<QString>& title,
                                  qint64 updatedAt, QString* error)
{
    MetadataOverride value;
    value.artist = artist;
    value.title = title;
    return setTrustedMetadata(songId, value, updatedAt, error);
}

bool Catalogue::setTrustedMetadata(qint64 songId, const MetadataOverride& value,
                                   qint64 updatedAt, QString* error)
{
    if (!value.hasValues()) {
        setError(QStringLiteral("A metadata override must set at least one value"), error);
        return false;
    }
    if (!m_database.transaction()) {
        setError(QStringLiteral("Could not begin metadata override transaction: %1")
                     .arg(m_database.lastError().text()), error);
        return false;
    }
    QSqlQuery update(m_database);
    update.prepare(kSetTrusted);
    bindTrusted(update, value, updatedAt, songId);
    if (!update.exec() || update.numRowsAffected() != 1
        || !recomputeEffectiveSong(songId, error)) {
        m_database.rollback();
        if (!update.lastError().text().isEmpty())
            setError(sqlError(update, QStringLiteral("Could not set metadata override")), error);
        else if (update.numRowsAffected() != 1)
            setError(QStringLiteral("Song was not found"), error);
        return false;
    }
    if (m_database.commit())
        return true;
    setError(QStringLiteral("Could not commit metadata override: %1")
                 .arg(m_database.lastError().text()), error);
    return false;
}

bool Catalogue::clearManualOverride(qint64 songId, QString* error)
{
    if (!m_database.transaction()) {
        setError(QStringLiteral("Could not begin metadata override transaction: %1")
                     .arg(m_database.lastError().text()), error);
        return false;
    }
    QSqlQuery update(m_database);
    update.prepare(QStringLiteral("UPDATE songs SET ") + kClearTrusted + QStringLiteral(" WHERE id=?"));
    update.addBindValue(songId);
    if (!update.exec() || update.numRowsAffected() != 1
        || !recomputeEffectiveSong(songId, error)) {
        m_database.rollback();
        if (!update.lastError().text().isEmpty())
            setError(sqlError(update, QStringLiteral("Could not clear metadata override")), error);
        return false;
    }
    if (m_database.commit())
        return true;
    setError(QStringLiteral("Could not commit metadata override: %1")
                 .arg(m_database.lastError().text()), error);
    return false;
}

bool Catalogue::applyManualOverrides(const QList<MetadataOverride>& overrides,
                                     QString* error)
{
    if (!m_database.transaction()) {
        setError(QStringLiteral("Could not begin metadata override sync: %1")
                     .arg(m_database.lastError().text()), error);
        return false;
    }
    QSqlQuery manualSongs(m_database);
    if (!manualSongs.exec(QStringLiteral("SELECT id FROM songs WHERE ")
                          + MetadataResolver::hasTrustedSql())) {
        m_database.rollback();
        setError(sqlError(manualSongs, QStringLiteral("Could not inspect manual metadata")), error);
        return false;
    }
    QList<qint64> changed;
    while (manualSongs.next())
        changed.append(manualSongs.value(0).toLongLong());
    QSqlQuery clear(m_database);
    if (!clear.exec(QStringLiteral("UPDATE songs SET ") + kClearTrusted + QStringLiteral(" WHERE ")
                    + MetadataResolver::hasTrustedSql())) {
        m_database.rollback();
        setError(sqlError(clear, QStringLiteral("Could not reset manual metadata")), error);
        return false;
    }

    for (const MetadataOverride& value : overrides) {
        if (!value.hasValues())
            continue;
        qint64 songId = findSongByMp3Path(value.rootPath, value.mp3RelPath, error);
        if (error && !error->isEmpty()) {
            m_database.rollback();
            return false;
        }
        if (songId == 0) {
            // The music folder moved: accept a unique relative-path match only
            // when its automatic disc/track (or title) matches the snapshot.
            const qint64 moved = findUniqueActiveSongByMp3Path(value.mp3RelPath, error);
            if (error && !error->isEmpty()) {
                m_database.rollback();
                return false;
            }
            if (moved != 0) {
                QSqlQuery automatic(m_database);
                automatic.prepare(QStringLiteral(
                    "SELECT COALESCE(auto_disc_id,''),COALESCE(auto_track,0),COALESCE(auto_title,'') "
                    "FROM songs WHERE id=?"));
                automatic.addBindValue(moved);
                if (automatic.exec() && automatic.next()) {
                    const bool matches = (!value.discId.trimmed().isEmpty() && value.track > 0
                                          && automatic.value(0).toString().compare(
                                                 value.discId, Qt::CaseInsensitive) == 0
                                          && automatic.value(1).toInt() == value.track)
                        || (value.discId.trimmed().isEmpty() && !value.autoTitle.trimmed().isEmpty()
                            && automatic.value(2).toString().compare(value.autoTitle,
                                                                     Qt::CaseInsensitive) == 0);
                    if (matches)
                        songId = moved;
                }
            }
        }
        if (songId == 0)
            continue;
        QSqlQuery set(m_database);
        set.prepare(kSetTrusted);
        bindTrusted(set, value, value.updatedAt, songId);
        if (!set.exec()) {
            m_database.rollback();
            setError(sqlError(set, QStringLiteral("Could not apply metadata override")), error);
            return false;
        }
        if (!changed.contains(songId))
            changed.append(songId);
    }
    for (qint64 songId : std::as_const(changed)) {
        if (!recomputeEffectiveSong(songId, error)) {
            m_database.rollback();
            return false;
        }
    }
    if (m_database.commit())
        return true;
    setError(QStringLiteral("Could not commit metadata override sync: %1")
                 .arg(m_database.lastError().text()), error);
    return false;
}

QVariantMap Catalogue::metadataStats(QString* error) const
{
    QVariantMap result;
    auto scalar = [&](const QString& sql) -> qlonglong {
        QSqlQuery query(m_database);
        if (!query.exec(sql) || !query.next()) {
            setError(sqlError(query, QStringLiteral("Metadata statistics failed")), error);
            return 0;
        }
        return query.value(0).toLongLong();
    };
    result.insert(QStringLiteral("totalSongs"), scalar(QStringLiteral("SELECT count(*) FROM songs")));
    result.insert(QStringLiteral("playableSongs"), scalar(QStringLiteral("SELECT count(*) FROM songs WHERE playable=1")));
    result.insert(QStringLiteral("effectiveConfidence"), groupedCounts(m_database,
        QStringLiteral("SELECT confidence,count(*) FROM songs GROUP BY confidence"), error));
    result.insert(QStringLiteral("autoConfidence"), groupedCounts(m_database,
        QStringLiteral("SELECT COALESCE(auto_confidence,'unresolved'),count(*) FROM songs GROUP BY 1"), error));
    result.insert(QStringLiteral("autoSource"), groupedCounts(m_database,
        QStringLiteral("SELECT COALESCE(auto_source,'fallback'),count(*) FROM songs GROUP BY 1"), error));
    result.insert(QStringLiteral("manualOverrides"), scalar(
        QStringLiteral("SELECT count(*) FROM songs WHERE ") + MetadataResolver::hasTrustedSql()));
    result.insert(QStringLiteral("trustedImports"), scalar(QStringLiteral(
        "SELECT count(*) FROM songs WHERE manual_origin='import'")));
    result.insert(QStringLiteral("conflicts"), scalar(QStringLiteral(
        "SELECT count(*) FROM songs WHERE conflict=1")));
    const QString version = catalogueMeta(QStringLiteral("resolver_version"), error);
    result.insert(QStringLiteral("resolverVersion"), version.isEmpty() ? 0 : version.toInt());
    result.insert(QStringLiteral("songsAtOldResolverVersion"), scalar(QStringLiteral(
        "SELECT count(*) FROM songs WHERE resolver_version<%1").arg(MetadataResolver::Version)));
    return result;
}

QVariantMap Catalogue::compareMetadata(const QString& baselinePath, int exampleLimit,
                                       QString* error) const
{
    struct Row { QString title; QString artist; QString confidence; };
    auto load = [&](QSqlDatabase database, QHash<QString, Row>* rows) -> bool {
        QSqlQuery query(database);
        if (!query.exec(QStringLiteral(
                "SELECT r.path,f.rel_path,so.display_title,so.display_artist,so.confidence "
                "FROM songs so JOIN sources s ON s.song_id=so.id "
                "JOIN library_roots r ON r.id=s.root_id JOIN files f ON f.id=s.mp3_file_id "
                "WHERE s.id=(SELECT min(s2.id) FROM sources s2 WHERE s2.song_id=so.id "
                "AND s2.mp3_file_id IS NOT NULL)"))) {
            setError(sqlError(query, QStringLiteral("Could not load metadata comparison")), error);
            return false;
        }
        while (query.next()) {
            const QString key = query.value(0).toString() + QChar(0x1f)
                + normalizedPlaylistRelativePath(query.value(1).toString());
            rows->insert(key, {query.value(2).toString(), query.value(3).toString(),
                               query.value(4).toString()});
        }
        return true;
    };

    const QString connection = QStringLiteral("fks-compare-%1").arg(
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSqlDatabase baseline = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
    baseline.setDatabaseName(QFileInfo(baselinePath).absoluteFilePath());
    baseline.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=5000"));
    if (!baseline.open()) {
        setError(QStringLiteral("Could not open baseline catalogue read-only: %1")
                     .arg(baseline.lastError().text()), error);
        baseline = QSqlDatabase();
        QSqlDatabase::removeDatabase(connection);
        return {};
    }
    QHash<QString, Row> currentRows;
    QHash<QString, Row> baselineRows;
    QVariantMap result;
    if (load(m_database, &currentRows) && load(baseline, &baselineRows)) {
        qlonglong up = 0, down = 0, same = 0, changed = 0, matched = 0;
        QVariantList examples;
        for (auto it = currentRows.cbegin(); it != currentRows.cend(); ++it) {
            const auto before = baselineRows.constFind(it.key());
            if (before == baselineRows.cend())
                continue;
            ++matched;
            const int delta = confidenceRank(it.value().confidence)
                - confidenceRank(before->confidence);
            if (delta > 0) ++up;
            else if (delta < 0) ++down;
            else ++same;
            if (it.value().title != before->title || it.value().artist != before->artist) {
                ++changed;
                if (examples.size() < qMax(0, exampleLimit)) {
                    QVariantMap example;
                    const QStringList key = it.key().split(QChar(0x1f));
                    example.insert(QStringLiteral("rootPath"), key.value(0));
                    example.insert(QStringLiteral("mp3RelPath"), key.value(1));
                    example.insert(QStringLiteral("beforeTitle"), before->title);
                    example.insert(QStringLiteral("beforeArtist"), before->artist);
                    example.insert(QStringLiteral("afterTitle"), it.value().title);
                    example.insert(QStringLiteral("afterArtist"), it.value().artist);
                    examples.append(example);
                }
            }
        }
        result.insert(QStringLiteral("matched"), matched);
        result.insert(QStringLiteral("confidenceUp"), up);
        result.insert(QStringLiteral("confidenceDown"), down);
        result.insert(QStringLiteral("confidenceSame"), same);
        result.insert(QStringLiteral("displayChanged"), changed);
        result.insert(QStringLiteral("examples"), examples);
    }
    baseline.close();
    baseline = QSqlDatabase();
    QSqlDatabase::removeDatabase(connection);
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

namespace {

QString reviewCondition(ReviewFilter filter)
{
    switch (filter) {
    case ReviewFilter::Unresolved:
        return QStringLiteral("so.confidence='unresolved'");
    case ReviewFilter::Low:
        return QStringLiteral("so.confidence='low'");
    case ReviewFilter::Medium:
        return QStringLiteral("so.confidence='medium'");
    case ReviewFilter::Conflicts:
        return QStringLiteral("so.conflict=1");
    case ReviewFilter::Manual:
        return MetadataResolver::hasTrustedSql(QStringLiteral("so"));
    case ReviewFilter::All:
        return QStringLiteral("1=1");
    }
    return QStringLiteral("1=1");
}

const QString kReviewScope = QStringLiteral(
    " AND EXISTS(SELECT 1 FROM sources rs JOIN library_roots rr ON rr.id=rs.root_id "
    "WHERE rs.song_id=so.id AND rs.kind='loose_cdg' AND rr.active=1 "
    "AND (rs.playable=1 OR rs.unplayable_reason='root_offline'))");

} // namespace

QList<ReviewRow> Catalogue::reviewList(ReviewFilter filter, const QString& text, int limit,
                                       QString* error) const
{
    QList<ReviewRow> result;
    QString sql = QStringLiteral(
        "SELECT so.id,so.display_artist,so.display_title,so.confidence,so.metadata_source,"
        "so.conflict,") + MetadataResolver::hasTrustedSql(QStringLiteral("so")) + QStringLiteral(","
        "COALESCE(f.rel_path,''),so.label FROM songs so LEFT JOIN sources s ON s.id=so.best_source_id "
        "LEFT JOIN files f ON f.id=s.mp3_file_id WHERE ")
        + reviewCondition(filter) + kReviewScope;
    const QStringList tokens = normalizeForSearch(text).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (qsizetype i = 0; i < tokens.size(); ++i)
        sql += QStringLiteral(" AND so.search_text LIKE ? ESCAPE '\\'");
    sql += QStringLiteral(" ORDER BY f.rel_path,so.id LIMIT ?");
    QSqlQuery query(m_database);
    query.prepare(sql);
    for (const QString& token : tokens)
        query.addBindValue(QStringLiteral("%") + escapedLike(token) + QStringLiteral("%"));
    query.addBindValue(qBound(1, limit, 5000));
    if (!query.exec()) {
        setError(sqlError(query, QStringLiteral("Review list failed")), error);
        return result;
    }
    while (query.next()) {
        result.append({query.value(0).toLongLong(), query.value(1).toString(),
                       query.value(2).toString(), query.value(3).toString(),
                       query.value(4).toString(), query.value(5).toBool(),
                       query.value(6).toBool(), query.value(7).toString(),
                       query.value(8).toString()});
    }
    return result;
}

qint64 Catalogue::reviewCount(ReviewFilter filter, QString* error) const
{
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("SELECT count(*) FROM songs so WHERE ") + reviewCondition(filter)
                    + kReviewScope)
        || !query.next()) {
        setError(sqlError(query, QStringLiteral("Review count failed")), error);
        return 0;
    }
    return query.value(0).toLongLong();
}

std::optional<ReviewSummary> Catalogue::readReviewSummary(const QString& databasePath,
                                                          QString* error)
{
    const QString name = QStringLiteral("fks-review-summary-%1")
                             .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    std::optional<ReviewSummary> result;
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        database.setDatabaseName(databasePath);
        database.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=5000"));
        if (!database.open()) {
            if (error)
                *error = database.lastError().text();
        } else {
            QSqlQuery query(database);
            const QString trusted = MetadataResolver::hasTrustedSql(QStringLiteral("so"));
            if (query.exec(QStringLiteral(
                    "SELECT count(*),"
                    "sum(CASE WHEN so.confidence='high' THEN 1 ELSE 0 END),"
                    "sum(CASE WHEN so.confidence='medium' THEN 1 ELSE 0 END),"
                    "sum(CASE WHEN so.confidence='low' THEN 1 ELSE 0 END),"
                    "sum(CASE WHEN so.confidence='unresolved' THEN 1 ELSE 0 END),"
                    "sum(CASE WHEN so.conflict=1 THEN 1 ELSE 0 END),"
                    "sum(CASE WHEN %1 THEN 1 ELSE 0 END) FROM songs so WHERE 1=1")
                               .arg(trusted) + kReviewScope)
                && query.next()) {
                ReviewSummary summary;
                summary.all = query.value(0).toLongLong();
                summary.high = query.value(1).toLongLong();
                summary.medium = query.value(2).toLongLong();
                summary.low = query.value(3).toLongLong();
                summary.unresolved = query.value(4).toLongLong();
                summary.conflicts = query.value(5).toLongLong();
                summary.manual = query.value(6).toLongLong();
                result = summary;
            } else if (error) {
                *error = query.lastError().text();
            }
            database.close();
        }
    }
    QSqlDatabase::removeDatabase(name);
    return result;
}

QVariantMap Catalogue::reviewDetail(qint64 songId, QString* error) const
{
    QVariantMap result;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT so.display_artist,so.display_title,so.confidence,so.metadata_source,"
        "so.auto_artist,so.auto_title,so.auto_confidence,so.auto_source,so.conflict,"
        "so.manual_artist,so.manual_title,so.evidence_json,so.disc_id,so.track,"
        "so.resolver_version,f.rel_path,f.rel_dir,f.file_name,f.raw_tags_json,f.size,"
        "g.rel_path,s.parsed_json,r.path,so.base_confidence,so.label,so.series,so.label_source,"
        "so.manual_label,so.manual_series,so.manual_disc_id,so.manual_track,so.manual_origin,"
        "so.auto_label,so.auto_series,so.auto_disc_id,so.auto_track "
        "FROM songs so LEFT JOIN sources s ON s.id=so.best_source_id "
        "LEFT JOIN files f ON f.id=s.mp3_file_id LEFT JOIN files g ON g.id=s.graphics_file_id "
        "LEFT JOIN library_roots r ON r.id=s.root_id WHERE so.id=?"));
    query.addBindValue(songId);
    if (!query.exec()) {
        setError(sqlError(query, QStringLiteral("Review detail failed")), error);
        return result;
    }
    if (!query.next()) {
        setError(QStringLiteral("Song was not found"), error);
        return result;
    }
    const QStringList keys = {
        QStringLiteral("artist"), QStringLiteral("title"), QStringLiteral("confidence"),
        QStringLiteral("source"), QStringLiteral("autoArtist"), QStringLiteral("autoTitle"),
        QStringLiteral("autoConfidence"), QStringLiteral("autoSource"), QStringLiteral("conflict"),
        QStringLiteral("manualArtist"), QStringLiteral("manualTitle"), QStringLiteral("evidence"),
        QStringLiteral("discId"), QStringLiteral("track"), QStringLiteral("resolverVersion"),
        QStringLiteral("mp3RelPath"), QStringLiteral("folder"), QStringLiteral("fileName"),
        QStringLiteral("tags"), QStringLiteral("mp3Size"), QStringLiteral("cdgRelPath"),
        QStringLiteral("parsed"), QStringLiteral("rootPath"), QStringLiteral("baseConfidence"),
        QStringLiteral("label"), QStringLiteral("series"), QStringLiteral("labelSource"),
        QStringLiteral("manualLabel"), QStringLiteral("manualSeries"), QStringLiteral("manualDiscId"),
        QStringLiteral("manualTrack"), QStringLiteral("manualOrigin"), QStringLiteral("autoLabel"),
        QStringLiteral("autoSeries"), QStringLiteral("autoDiscId"), QStringLiteral("autoTrack")};
    for (int i = 0; i < keys.size(); ++i)
        result.insert(keys.at(i), query.value(i));
    result.insert(QStringLiteral("songId"), songId);
    result.insert(QStringLiteral("autoDisplayArtist"),
                  MetadataResolver::displayArtistName(query.value(4).toString()));
    result.insert(QStringLiteral("evidence"),
                  QJsonDocument::fromJson(query.value(11).toByteArray()).object().toVariantMap());
    result.insert(QStringLiteral("tags"),
                  QJsonDocument::fromJson(query.value(18).toByteArray()).object().toVariantMap());
    result.insert(QStringLiteral("parsed"),
                  QJsonDocument::fromJson(query.value(21).toByteArray()).object().toVariantMap());
    return result;
}

QList<MetadataOverride> Catalogue::trustedMirror(QString* error) const
{
    QList<MetadataOverride> result;
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral(
            "SELECT so.id,so.manual_artist,so.manual_title,so.manual_label,so.manual_series,"
            "so.manual_disc_id,so.manual_track,COALESCE(so.manual_origin,'manual'),"
            "COALESCE(so.manual_updated_at,0) FROM songs so WHERE ")
            + MetadataResolver::hasTrustedSql(QStringLiteral("so")))) {
        setError(sqlError(query, QStringLiteral("Could not read trusted metadata")), error);
        return result;
    }
    while (query.next()) {
        auto value = metadataOverrideSnapshot(query.value(0).toLongLong(), nullptr);
        if (!value)
            continue;
        auto text = [&](int column) {
            return query.value(column).isNull() ? std::nullopt
                                                : std::optional<QString>(query.value(column).toString());
        };
        value->artist = text(1);
        value->title = text(2);
        value->label = text(3);
        value->series = text(4);
        value->trustedDiscId = text(5);
        value->trustedTrack = query.value(6).isNull() ? std::nullopt
                                                      : std::optional<int>(query.value(6).toInt());
        value->origin = query.value(7).toString();
        value->updatedAt = query.value(8).toLongLong();
        result.append(*value);
    }
    return result;
}

bool Catalogue::hasTrustedMirror(QString* error) const
{
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("SELECT EXISTS(SELECT 1 FROM songs WHERE ")
                    + MetadataResolver::hasTrustedSql() + QStringLiteral(")"))
        || !query.next()) {
        setError(sqlError(query, QStringLiteral("Could not inspect trusted metadata")), error);
        return false;
    }
    return query.value(0).toBool();
}
