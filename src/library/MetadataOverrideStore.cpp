#include "library/MetadataOverrideStore.h"

#include "library/Catalogue.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QMutex>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

namespace {

Q_LOGGING_CATEGORY(lcMetadataOverrides, "fks.library.metadata-overrides")

QString queryError(const QSqlQuery& query, const QString& context)
{
    return QStringLiteral("%1: %2").arg(context, query.lastError().text());
}

bool indicatesCorruption(const QString& detail)
{
    const QString folded = detail.toLower();
    return folded.contains(QStringLiteral("malformed"))
        || folded.contains(QStringLiteral("not a database"))
        || folded.contains(QStringLiteral("database disk image is malformed"));
}

QVariant nullableString(const std::optional<QString>& value)
{
    return value ? QVariant(*value) : QVariant();
}

std::optional<QString> optionalString(const QVariant& value)
{
    return value.isNull() ? std::nullopt : std::optional<QString>(value.toString());
}

std::optional<int> optionalInt(const QVariant& value)
{
    return value.isNull() ? std::nullopt : std::optional<int>(value.toInt());
}

QVariant nullableInt(const std::optional<int>& value)
{
    return value ? QVariant(*value) : QVariant();
}

MetadataOverride overrideFromQuery(const QSqlQuery& query)
{
    MetadataOverride value;
    value.rootPath = query.value(0).toString();
    value.mp3RelPath = query.value(1).toString();
    value.artist = optionalString(query.value(2));
    value.title = optionalString(query.value(3));
    value.createdAt = query.value(4).toLongLong();
    value.updatedAt = query.value(5).toLongLong();
    value.autoArtist = query.value(6).toString();
    value.autoTitle = query.value(7).toString();
    value.discId = query.value(8).toString();
    value.track = query.value(9).toInt();
    value.fileName = query.value(10).toString();
    value.label = optionalString(query.value(11));
    value.series = optionalString(query.value(12));
    value.trustedDiscId = optionalString(query.value(13));
    value.trustedTrack = optionalInt(query.value(14));
    value.origin = query.value(15).toString();
    value.originalKey = optionalInt(query.value(16));
    return value;
}

// Everything stored for a song file except its identity (root and path).
const QString valueColumns = QStringLiteral(
    "artist,title,created_at,updated_at,"
    "auto_artist,auto_title,disc_id,track,file_name,label,series,set_disc_id,set_track,origin,"
    "original_key");
const QString overrideColumns = QStringLiteral("root_path,mp3_rel_path,") + valueColumns;

} // namespace

QMutex& MetadataOverrideStore::synchronisation()
{
    static QMutex mutex;
    return mutex;
}

MetadataOverrideStore::MetadataOverrideStore(QString databasePath)
    : m_databasePath(QFileInfo(databasePath).absoluteFilePath())
    , m_connectionName(QStringLiteral("fks-metadata-overrides-%1").arg(
          QUuid::createUuid().toString(QUuid::WithoutBraces)))
{
}

MetadataOverrideStore::~MetadataOverrideStore()
{
    close();
}

void MetadataOverrideStore::setError(const QString& message, QString* error) const
{
    m_lastError = message;
    if (error)
        *error = message;
}

bool MetadataOverrideStore::open(QString* error, const QStringList& libraryRoots,
                                 bool recoverCorrupt)
{
    if (isOpen())
        return true;
    QString safetyError;
    if (!Catalogue::storageIsSafe(m_databasePath, {}, libraryRoots, &safetyError)) {
        setError(safetyError, error);
        qCritical(lcMetadataOverrides).noquote() << safetyError;
        return false;
    }
    const QFileInfo info(m_databasePath);
    if (!QDir().mkpath(info.absolutePath())) {
        setError(QStringLiteral("Could not create metadata override directory: %1")
                     .arg(info.absolutePath()), error);
        return false;
    }
    m_database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_database.setDatabaseName(m_databasePath);
    m_database.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000"));
    if (!m_database.open()) {
        const QString detail = m_database.lastError().text();
        close();
        if (info.exists() && indicatesCorruption(detail)
            && recoverCorrupt && recoverCorruptDatabase(detail, error)) {
            m_recovered = true;
            return open(error, libraryRoots, false);
        }
        setError(QStringLiteral("Could not open metadata overrides: %1").arg(detail), error);
        return false;
    }
    QSqlQuery version(m_database);
    if (!version.exec(QStringLiteral("PRAGMA user_version")) || !version.next()) {
        const QString detail = version.lastError().text();
        version.finish();
        close();
        if (info.exists() && indicatesCorruption(detail)
            && recoverCorrupt && recoverCorruptDatabase(detail, error)) {
            m_recovered = true;
            return open(error, libraryRoots, false);
        }
        setError(QStringLiteral("Could not read metadata override schema: %1").arg(detail), error);
        return false;
    }
    const int schema = version.value(0).toInt();
    version.finish();
    if (schema > SchemaVersion) {
        const QString message = QStringLiteral(
            "Metadata override schema version %1 is newer than supported version %2; refusing to open")
                                    .arg(schema).arg(SchemaVersion);
        close();
        setError(message, error);
        return false;
    }
    if (!ensureSchema(error)) {
        const QString detail = error ? *error : m_lastError;
        close();
        if (info.exists() && indicatesCorruption(detail)
            && recoverCorrupt && recoverCorruptDatabase(detail, error)) {
            m_recovered = true;
            return open(error, libraryRoots, false);
        }
        return false;
    }
    m_lastError.clear();
    if (error)
        error->clear();
    return true;
}

void MetadataOverrideStore::close()
{
    if (!m_database.isValid())
        return;
    m_database.close();
    m_database = QSqlDatabase();
    QSqlDatabase::removeDatabase(m_connectionName);
}

bool MetadataOverrideStore::isOpen() const
{
    return m_database.isOpen();
}

bool MetadataOverrideStore::execute(const QString& sql, QString* error) const
{
    QSqlQuery query(m_database);
    if (query.exec(sql))
        return true;
    setError(queryError(query, QStringLiteral("Metadata override SQL failed")), error);
    return false;
}

bool MetadataOverrideStore::ensureSchema(QString* error)
{
    if (!execute(QStringLiteral("PRAGMA foreign_keys=ON"), error)
        || !execute(QStringLiteral("PRAGMA journal_mode=WAL"), error)
        || !execute(QStringLiteral("PRAGMA busy_timeout=5000"), error))
        return false;
    QSqlQuery version(m_database);
    if (!version.exec(QStringLiteral("PRAGMA user_version")) || !version.next()) {
        setError(queryError(version, QStringLiteral("Could not inspect metadata override schema")), error);
        return false;
    }
    const int current = version.value(0).toInt();
    if (current == SchemaVersion)
        return ensureStateTable(error);
    if (!m_database.transaction()) {
        setError(QStringLiteral("Could not begin metadata override schema transaction: %1")
                     .arg(m_database.lastError().text()), error);
        return false;
    }
    QSqlQuery query(m_database);
    QStringList statements;
    if (current == 0) {
        statements.append(QStringLiteral(
            "CREATE TABLE metadata_overrides(root_path TEXT NOT NULL,mp3_rel_path TEXT NOT NULL,"
            "artist TEXT,title TEXT,created_at INTEGER NOT NULL,updated_at INTEGER NOT NULL,"
            "auto_artist TEXT,auto_title TEXT,disc_id TEXT,track INTEGER NOT NULL DEFAULT 0,"
            "file_name TEXT,label TEXT,series TEXT,set_disc_id TEXT,set_track INTEGER,"
            "origin TEXT NOT NULL DEFAULT 'manual',original_key INTEGER,"
            "PRIMARY KEY(root_path,mp3_rel_path))"));
    } else {
        // Version 1 held artist/title corrections only.
        if (current < 2) {
            statements << QStringLiteral("ALTER TABLE metadata_overrides ADD COLUMN label TEXT")
                       << QStringLiteral("ALTER TABLE metadata_overrides ADD COLUMN series TEXT")
                       << QStringLiteral("ALTER TABLE metadata_overrides ADD COLUMN set_disc_id TEXT")
                       << QStringLiteral("ALTER TABLE metadata_overrides ADD COLUMN set_track INTEGER")
                       << QStringLiteral("ALTER TABLE metadata_overrides ADD COLUMN origin TEXT NOT NULL DEFAULT 'manual'");
        }
        // Version 3 adds the song's original key, as chosen by the user.
        statements << QStringLiteral("ALTER TABLE metadata_overrides ADD COLUMN original_key INTEGER");
    }
    statements.append(QStringLiteral("PRAGMA user_version=%1").arg(SchemaVersion));
    for (const QString& statement : std::as_const(statements)) {
        if (!query.exec(statement)) {
            m_database.rollback();
            setError(queryError(query, QStringLiteral("Could not update metadata override schema")), error);
            return false;
        }
    }
    if (!m_database.commit()) {
        setError(QStringLiteral("Could not commit metadata override schema: %1")
                     .arg(m_database.lastError().text()), error);
        return false;
    }
    return ensureStateTable(error);
}

bool MetadataOverrideStore::ensureStateTable(QString* error)
{
    // Added within schema version 3: an earlier build still opens the store
    // and ignores the table.
    return execute(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS store_state(key TEXT PRIMARY KEY,value TEXT NOT NULL)"), error);
}

bool MetadataOverrideStore::recoverCorruptDatabase(const QString& detail, QString* error)
{
    const QFileInfo info(m_databasePath);
    const QString stamp = QDateTime::currentDateTimeUtc().toString(
        QStringLiteral("yyyyMMdd-HHmmsszzz"));
    const QString moved = QDir(info.absolutePath()).filePath(
        info.completeBaseName() + QStringLiteral(".corrupt-") + stamp
        + QStringLiteral(".sqlite"));
    if (!QFile::rename(m_databasePath, moved)) {
        setError(QStringLiteral("Metadata override database is corrupt (%1) and could not be moved aside")
                     .arg(detail), error);
        return false;
    }
    for (const QString& suffix : {QStringLiteral("-wal"), QStringLiteral("-shm")}) {
        if (QFileInfo::exists(m_databasePath + suffix))
            QFile::rename(m_databasePath + suffix, moved + suffix);
    }
    qWarning(lcMetadataOverrides).noquote()
        << "Moved corrupt metadata override database to" << moved << "because:" << detail;
    return true;
}

bool MetadataOverrideStore::isEstablished(QString* error) const
{
    if (!isOpen()) {
        setError(QStringLiteral("Metadata override store is not open"), error);
        return false;
    }
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral(
            "SELECT EXISTS(SELECT 1 FROM store_state WHERE key='established') "
            "OR EXISTS(SELECT 1 FROM metadata_overrides)"))
        || !query.next()) {
        setError(queryError(query, QStringLiteral("Could not inspect metadata overrides")), error);
        return false;
    }
    return query.value(0).toBool();
}

bool MetadataOverrideStore::establish(const QList<MetadataOverride>& values, QString* error)
{
    if (!isOpen()) {
        setError(QStringLiteral("Metadata override store is not open"), error);
        return false;
    }
    if (!m_database.transaction()) {
        setError(QStringLiteral("Could not begin restoring metadata overrides: %1")
                     .arg(m_database.lastError().text()), error);
        return false;
    }
    auto fail = [&](const QString& message) {
        m_database.rollback();
        setError(message, error);
        return false;
    };
    if (!values.isEmpty()) {
        QSqlQuery rows(m_database);
        if (!rows.exec(QStringLiteral("SELECT EXISTS(SELECT 1 FROM metadata_overrides)"))
            || !rows.next())
            return fail(queryError(rows, QStringLiteral("Could not inspect metadata overrides")));
        if (rows.value(0).toBool())
            return fail(QStringLiteral("Metadata overrides were not restored: the store is not empty"));
    }
    QSet<QString> identities;
    for (const MetadataOverride& value : values) {
        // Two values for one song file would leave only one of them.
        const QString identity = Catalogue::canonicalPath(value.rootPath) + QChar(0x1f)
            + Catalogue::normalizedPlaylistRelativePath(value.mp3RelPath);
        if (identities.contains(identity))
            return fail(QStringLiteral("Metadata overrides were not restored: two corrections "
                                       "for %1 %2").arg(value.rootPath, value.mp3RelPath));
        identities.insert(identity);
        QString message;
        if (!setOverride(value, &message))
            return fail(message);
    }
    QSqlQuery mark(m_database);
    if (!mark.exec(QStringLiteral(
            "INSERT OR IGNORE INTO store_state(key,value) VALUES('established','1')")))
        return fail(queryError(mark, QStringLiteral("Could not mark metadata overrides")));
    if (m_database.commit())
        return true;
    return fail(QStringLiteral("Could not commit restoring metadata overrides: %1")
                    .arg(m_database.lastError().text()));
}

bool MetadataOverrideStore::setOverride(const MetadataOverride& input, QString* error)
{
    if (!isOpen()) {
        setError(QStringLiteral("Metadata override store is not open"), error);
        return false;
    }
    if (!input.hasValues()) {
        setError(QStringLiteral("A metadata override must set at least one value"), error);
        return false;
    }
    if (input.originalKey && (*input.originalKey < 0 || *input.originalKey > 23)) {
        setError(QStringLiteral("Not a song key: %1").arg(*input.originalKey), error);
        return false;
    }
    if (input.origin != QLatin1String("manual") && input.origin != QLatin1String("import")) {
        setError(QStringLiteral("Unknown metadata origin: %1").arg(input.origin), error);
        return false;
    }
    const QString root = Catalogue::canonicalPath(input.rootPath);
    const QString relative = Catalogue::normalizedPlaylistRelativePath(input.mp3RelPath);
    const qint64 now = input.updatedAt > 0 ? input.updatedAt
                                          : QDateTime::currentMSecsSinceEpoch();
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO metadata_overrides(root_path,mp3_rel_path,artist,title,created_at,updated_at,"
        "auto_artist,auto_title,disc_id,track,file_name,label,series,set_disc_id,set_track,origin,"
        "original_key) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
        "ON CONFLICT(root_path,mp3_rel_path) DO UPDATE SET artist=excluded.artist,"
        "title=excluded.title,updated_at=excluded.updated_at,auto_artist=excluded.auto_artist,"
        "auto_title=excluded.auto_title,disc_id=excluded.disc_id,track=excluded.track,"
        "file_name=excluded.file_name,label=excluded.label,series=excluded.series,"
        "set_disc_id=excluded.set_disc_id,set_track=excluded.set_track,origin=excluded.origin,"
        "original_key=excluded.original_key"));
    query.addBindValue(root);
    query.addBindValue(relative);
    query.addBindValue(nullableString(input.artist));
    query.addBindValue(nullableString(input.title));
    query.addBindValue(input.createdAt > 0 ? input.createdAt : now);
    query.addBindValue(now);
    query.addBindValue(input.autoArtist);
    query.addBindValue(input.autoTitle);
    query.addBindValue(input.discId);
    query.addBindValue(input.track);
    query.addBindValue(input.fileName);
    query.addBindValue(nullableString(input.label));
    query.addBindValue(nullableString(input.series));
    query.addBindValue(nullableString(input.trustedDiscId));
    query.addBindValue(nullableInt(input.trustedTrack));
    query.addBindValue(input.origin);
    query.addBindValue(nullableInt(input.originalKey));
    if (query.exec())
        return true;
    setError(queryError(query, QStringLiteral("Could not save metadata override")), error);
    return false;
}

bool MetadataOverrideStore::clearOverride(const QString& rootPath,
                                          const QString& mp3RelPath,
                                          QString* error)
{
    if (!isOpen()) {
        setError(QStringLiteral("Metadata override store is not open"), error);
        return false;
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "DELETE FROM metadata_overrides WHERE root_path=? AND mp3_rel_path=?"));
    query.addBindValue(Catalogue::canonicalPath(rootPath));
    query.addBindValue(Catalogue::normalizedPlaylistRelativePath(mp3RelPath));
    if (query.exec())
        return true;
    setError(queryError(query, QStringLiteral("Could not clear metadata override")), error);
    return false;
}

bool MetadataOverrideStore::copyOverride(const MovedMetadataOverride& move, QString* error)
{
    if (!isOpen()) {
        setError(QStringLiteral("Metadata override store is not open"), error);
        return false;
    }
    // A plain INSERT: an existing row at the new identity makes this fail.
    QSqlQuery insert(m_database);
    insert.prepare(QStringLiteral("INSERT INTO metadata_overrides(root_path,mp3_rel_path,%1) "
                                  "SELECT ?,?,%1 FROM metadata_overrides "
                                  "WHERE root_path=? AND mp3_rel_path=? AND updated_at=?")
                       .arg(valueColumns));
    insert.addBindValue(Catalogue::canonicalPath(move.rootPath));
    insert.addBindValue(Catalogue::normalizedPlaylistRelativePath(move.mp3RelPath));
    insert.addBindValue(Catalogue::canonicalPath(move.stored.rootPath));
    insert.addBindValue(Catalogue::normalizedPlaylistRelativePath(move.stored.mp3RelPath));
    insert.addBindValue(move.stored.updatedAt);
    if (!insert.exec()) {
        setError(queryError(insert, QStringLiteral("Could not copy metadata override")), error);
        return false;
    }
    if (insert.numRowsAffected() == 1)
        return true;
    setError(QStringLiteral("Could not copy metadata override: it changed or was removed"), error);
    return false;
}

bool MetadataOverrideStore::removeOverrides(const QList<MetadataOverride>& rows, QString* error)
{
    if (!isOpen()) {
        setError(QStringLiteral("Metadata override store is not open"), error);
        return false;
    }
    if (!m_database.transaction()) {
        setError(QStringLiteral("Could not begin removing metadata overrides: %1")
                     .arg(m_database.lastError().text()), error);
        return false;
    }
    QSqlQuery remove(m_database);
    remove.prepare(QStringLiteral(
        "DELETE FROM metadata_overrides WHERE root_path=? AND mp3_rel_path=? AND updated_at=?"));
    for (const MetadataOverride& row : rows) {
        remove.bindValue(0, Catalogue::canonicalPath(row.rootPath));
        remove.bindValue(1, Catalogue::normalizedPlaylistRelativePath(row.mp3RelPath));
        remove.bindValue(2, row.updatedAt);
        if (!remove.exec()) {
            const QString message = queryError(remove, QStringLiteral("Could not remove metadata override"));
            m_database.rollback();
            setError(message, error);
            return false;
        }
    }
    if (m_database.commit())
        return true;
    const QString message = QStringLiteral("Could not commit removing metadata overrides: %1")
                                .arg(m_database.lastError().text());
    m_database.rollback();
    setError(message, error);
    return false;
}

bool MetadataOverrideStore::removeCopiesExactly(const QList<MetadataOverride>& copies,
                                                QString* error)
{
    QSqlQuery remove(m_database);
    remove.prepare(QStringLiteral(
        "DELETE FROM metadata_overrides WHERE root_path=? AND mp3_rel_path=? AND updated_at=?"));
    for (const MetadataOverride& copy : copies) {
        remove.bindValue(0, Catalogue::canonicalPath(copy.rootPath));
        remove.bindValue(1, Catalogue::normalizedPlaylistRelativePath(copy.mp3RelPath));
        remove.bindValue(2, copy.updatedAt);
        if (!remove.exec()) {
            setError(queryError(remove, QStringLiteral("Could not remove an older copy")), error);
            return false;
        }
        if (remove.numRowsAffected() != 1) {
            setError(QStringLiteral("Could not remove an older copy: it changed or was removed"), error);
            return false;
        }
    }
    return true;
}

bool MetadataOverrideStore::setOverrideAndRemoveCopies(const MetadataOverride& value,
                                                       const QList<MetadataOverride>& copies,
                                                       QString* error)
{
    if (!isOpen()) {
        setError(QStringLiteral("Metadata override store is not open"), error);
        return false;
    }
    if (copies.isEmpty())
        return setOverride(value, error);
    if (!m_database.transaction()) {
        setError(QStringLiteral("Could not begin saving metadata override: %1")
                     .arg(m_database.lastError().text()), error);
        return false;
    }
    if (!removeCopiesExactly(copies, error) || !setOverride(value, error)) {
        m_database.rollback();
        return false;
    }
    if (m_database.commit())
        return true;
    const QString message = QStringLiteral("Could not commit metadata override: %1")
                                .arg(m_database.lastError().text());
    m_database.rollback();
    setError(message, error);
    return false;
}

bool MetadataOverrideStore::clearOverrideAndCopies(const QString& rootPath,
                                                   const QString& mp3RelPath,
                                                   const QList<MetadataOverride>& copies,
                                                   QString* error)
{
    if (!isOpen()) {
        setError(QStringLiteral("Metadata override store is not open"), error);
        return false;
    }
    if (!m_database.transaction()) {
        setError(QStringLiteral("Could not begin clearing metadata override: %1")
                     .arg(m_database.lastError().text()), error);
        return false;
    }
    if (!removeCopiesExactly(copies, error) || !clearOverride(rootPath, mp3RelPath, error)) {
        m_database.rollback();
        return false;
    }
    if (m_database.commit())
        return true;
    const QString message = QStringLiteral("Could not commit clearing metadata override: %1")
                                .arg(m_database.lastError().text());
    m_database.rollback();
    setError(message, error);
    return false;
}

std::optional<MetadataOverride> MetadataOverrideStore::overrideFor(
    const QString& rootPath, const QString& mp3RelPath, QString* error) const
{
    if (!isOpen()) {
        setError(QStringLiteral("Metadata override store is not open"), error);
        return std::nullopt;
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT %1 FROM metadata_overrides "
                                 "WHERE root_path=? AND mp3_rel_path=?")
                      .arg(overrideColumns));
    query.addBindValue(Catalogue::canonicalPath(rootPath));
    query.addBindValue(Catalogue::normalizedPlaylistRelativePath(mp3RelPath));
    if (!query.exec()) {
        setError(queryError(query, QStringLiteral("Could not read metadata override")), error);
        return std::nullopt;
    }
    return query.next() ? std::optional<MetadataOverride>(overrideFromQuery(query))
                        : std::nullopt;
}

QList<MetadataOverride> MetadataOverrideStore::all(QString* error) const
{
    QList<MetadataOverride> result;
    if (!isOpen()) {
        setError(QStringLiteral("Metadata override store is not open"), error);
        return result;
    }
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("SELECT %1 FROM metadata_overrides "
                                   "ORDER BY root_path,mp3_rel_path")
                        .arg(overrideColumns))) {
        setError(queryError(query, QStringLiteral("Could not list metadata overrides")), error);
        return result;
    }
    while (query.next())
        result.append(overrideFromQuery(query));
    if (query.lastError().isValid()) {
        // Never a partial list: it would be taken for every correction there is.
        setError(queryError(query, QStringLiteral("Could not list metadata overrides")), error);
        return {};
    }
    return result;
}
