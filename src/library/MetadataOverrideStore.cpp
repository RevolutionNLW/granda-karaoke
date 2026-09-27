#include "library/MetadataOverrideStore.h"

#include "library/Catalogue.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QMutex>
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
    return value;
}

const QString overrideColumns = QStringLiteral(
    "root_path,mp3_rel_path,artist,title,created_at,updated_at,"
    "auto_artist,auto_title,disc_id,track,file_name,label,series,set_disc_id,set_track,origin");

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
        return true;
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
            "origin TEXT NOT NULL DEFAULT 'manual',PRIMARY KEY(root_path,mp3_rel_path))"));
    } else {
        // Version 1 held artist/title corrections only.
        statements << QStringLiteral("ALTER TABLE metadata_overrides ADD COLUMN label TEXT")
                   << QStringLiteral("ALTER TABLE metadata_overrides ADD COLUMN series TEXT")
                   << QStringLiteral("ALTER TABLE metadata_overrides ADD COLUMN set_disc_id TEXT")
                   << QStringLiteral("ALTER TABLE metadata_overrides ADD COLUMN set_track INTEGER")
                   << QStringLiteral("ALTER TABLE metadata_overrides ADD COLUMN origin TEXT NOT NULL DEFAULT 'manual'");
    }
    statements.append(QStringLiteral("PRAGMA user_version=2"));
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
    return true;
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
        "auto_artist,auto_title,disc_id,track,file_name,label,series,set_disc_id,set_track,origin) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
        "ON CONFLICT(root_path,mp3_rel_path) DO UPDATE SET artist=excluded.artist,"
        "title=excluded.title,updated_at=excluded.updated_at,auto_artist=excluded.auto_artist,"
        "auto_title=excluded.auto_title,disc_id=excluded.disc_id,track=excluded.track,"
        "file_name=excluded.file_name,label=excluded.label,series=excluded.series,"
        "set_disc_id=excluded.set_disc_id,set_track=excluded.set_track,origin=excluded.origin"));
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
    return result;
}
