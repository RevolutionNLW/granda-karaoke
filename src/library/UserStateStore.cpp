#include "library/UserStateStore.h"

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
#include <QVariant>

namespace {

Q_LOGGING_CATEGORY(lcUserState, "fks.library.user-state")

bool indicatesCorruption(const QString& detail)
{
    const QString folded = detail.toLower();
    return folded.contains(QLatin1String("malformed")) || folded.contains(QLatin1String("not a database"));
}

QString queryError(const QSqlQuery& query, const QString& context)
{
    return QStringLiteral("%1: %2").arg(context, query.lastError().text());
}

PlayHistoryEntry entryFrom(const QSqlQuery& query)
{
    PlayHistoryEntry entry;
    entry.identity = query.value(0).toString();
    entry.playCount = query.value(1).toInt();
    entry.lastPlayedMs = query.value(2).toLongLong();
    entry.rootPath = query.value(3).toString();
    entry.mp3RelPath = query.value(4).toString();
    entry.mp3Size = query.value(5).toLongLong();
    entry.cdgSize = query.value(6).toLongLong();
    return entry;
}

const QString kHistoryColumns = QStringLiteral(
    "identity,play_count,last_played_ms,root_path,mp3_rel_path,mp3_size,cdg_size");

} // namespace

QMutex& UserStateStore::synchronisation()
{
    static QMutex mutex;
    return mutex;
}

UserStateStore::UserStateStore(QString databasePath)
    : m_databasePath(QFileInfo(databasePath).absoluteFilePath())
    , m_connectionName(QStringLiteral("fks-user-state-%1")
                           .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)))
{
}

UserStateStore::~UserStateStore()
{
    close();
}

void UserStateStore::setError(const QString& message, QString* error) const
{
    m_lastError = message;
    if (error)
        *error = message;
}

bool UserStateStore::open(QString* error, const QStringList& libraryRoots, bool recoverCorrupt)
{
    if (isOpen())
        return true;
    QString safetyError;
    if (!Catalogue::storageIsSafe(m_databasePath, {}, libraryRoots, &safetyError)) {
        setError(safetyError, error);
        qCritical(lcUserState).noquote() << safetyError;
        return false;
    }
    const QFileInfo info(m_databasePath);
    if (!QDir().mkpath(info.absolutePath())) {
        setError(QStringLiteral("Could not create user state directory: %1").arg(info.absolutePath()), error);
        return false;
    }
    m_database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_database.setDatabaseName(m_databasePath);
    m_database.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000"));
    QString detail;
    if (m_database.open() && ensureSchema(&detail))
        return true;
    if (detail.isEmpty())
        detail = m_database.lastError().text();
    close();
    if (info.exists() && indicatesCorruption(detail) && recoverCorrupt
        && recoverCorruptDatabase(detail, error))
        return open(error, libraryRoots, false);
    setError(QStringLiteral("Could not open user state: %1").arg(detail), error);
    return false;
}

void UserStateStore::close()
{
    if (!m_database.isValid())
        return;
    m_database.close();
    m_database = QSqlDatabase();
    QSqlDatabase::removeDatabase(m_connectionName);
}

bool UserStateStore::isOpen() const
{
    return m_database.isOpen();
}

bool UserStateStore::ensureSchema(QString* error)
{
    QSqlQuery query(m_database);
    for (const char* pragma : {"PRAGMA journal_mode=WAL", "PRAGMA busy_timeout=5000"}) {
        if (!query.exec(QLatin1String(pragma))) {
            setError(queryError(query, QStringLiteral("User state setup failed")), error);
            return false;
        }
    }
    if (!query.exec(QStringLiteral("PRAGMA user_version")) || !query.next()) {
        setError(queryError(query, QStringLiteral("Could not inspect user state schema")), error);
        return false;
    }
    const int version = query.value(0).toInt();
    query.finish();
    if (version > SchemaVersion) {
        setError(QStringLiteral("User state schema version %1 is newer than supported version %2")
                     .arg(version).arg(SchemaVersion), error);
        return false;
    }
    if (version == SchemaVersion)
        return true;
    const QStringList statements = {
        QStringLiteral("CREATE TABLE IF NOT EXISTS play_history(identity TEXT PRIMARY KEY,"
                       "play_count INTEGER NOT NULL DEFAULT 0,last_played_ms INTEGER,"
                       "root_path TEXT,mp3_rel_path TEXT,mp3_size INTEGER,cdg_size INTEGER)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS preferences(key TEXT PRIMARY KEY,value TEXT)"),
        QStringLiteral("PRAGMA user_version=1"),
    };
    if (!m_database.transaction()) {
        setError(QStringLiteral("Could not begin user state schema: %1").arg(m_database.lastError().text()), error);
        return false;
    }
    for (const QString& statement : statements) {
        if (!query.exec(statement)) {
            m_database.rollback();
            setError(queryError(query, QStringLiteral("Could not create user state schema")), error);
            return false;
        }
    }
    if (!m_database.commit()) {
        setError(QStringLiteral("Could not commit user state schema: %1").arg(m_database.lastError().text()), error);
        return false;
    }
    return true;
}

bool UserStateStore::recoverCorruptDatabase(const QString& detail, QString* error)
{
    const QFileInfo info(m_databasePath);
    const QString moved = QDir(info.absolutePath()).filePath(
        info.completeBaseName() + QStringLiteral(".corrupt-")
        + QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmsszzz"))
        + QStringLiteral(".sqlite"));
    if (!QFile::rename(m_databasePath, moved)) {
        setError(QStringLiteral("User state is damaged (%1) and could not be set aside").arg(detail), error);
        return false;
    }
    for (const QString& suffix : {QStringLiteral("-wal"), QStringLiteral("-shm")}) {
        if (QFileInfo::exists(m_databasePath + suffix))
            QFile::rename(m_databasePath + suffix, moved + suffix);
    }
    qWarning(lcUserState).noquote() << "Set damaged user state aside as" << moved << "because:" << detail;
    return true;
}

bool UserStateStore::recordPlay(const PlayHistoryEntry& where, qint64 playedAtMs,
                                PlayHistoryEntry* updated, QString* error)
{
    if (where.identity.isEmpty()) {
        setError(QStringLiteral("A play needs the song's identity"), error);
        return false;
    }
    const bool completeLocation = !where.rootPath.isEmpty() && !where.mp3RelPath.isEmpty()
        && where.mp3Size > 0 && where.cdgSize > 0;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO play_history(identity,play_count,last_played_ms,root_path,mp3_rel_path,mp3_size,cdg_size) "
        "VALUES(:identity,1,:at,:root,:rel,:mp3size,:cdgsize) ON CONFLICT(identity) DO UPDATE SET "
        "play_count=play_count+1,last_played_ms=max(coalesce(last_played_ms,0),excluded.last_played_ms),"
        "root_path=CASE WHEN :complete THEN excluded.root_path ELSE root_path END,"
        "mp3_rel_path=CASE WHEN :complete THEN excluded.mp3_rel_path ELSE mp3_rel_path END,"
        "mp3_size=CASE WHEN :complete THEN excluded.mp3_size ELSE mp3_size END,"
        "cdg_size=CASE WHEN :complete THEN excluded.cdg_size ELSE cdg_size END"));
    query.bindValue(QStringLiteral(":identity"), where.identity);
    query.bindValue(QStringLiteral(":at"), playedAtMs);
    query.bindValue(QStringLiteral(":root"), where.rootPath);
    query.bindValue(QStringLiteral(":rel"), where.mp3RelPath);
    query.bindValue(QStringLiteral(":mp3size"), where.mp3Size);
    query.bindValue(QStringLiteral(":cdgsize"), where.cdgSize);
    query.bindValue(QStringLiteral(":complete"), completeLocation ? 1 : 0);
    if (!query.exec()) {
        setError(queryError(query, QStringLiteral("Could not record the play")), error);
        return false;
    }
    if (updated) {
        QString readError;
        *updated = playHistoryFor(where.identity, &readError);
        if (!readError.isEmpty()) {
            setError(readError, error);
            return false;
        }
    }
    return true;
}

QList<PlayHistoryEntry> UserStateStore::playHistory(QString* error) const
{
    QList<PlayHistoryEntry> result;
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("SELECT %1 FROM play_history ORDER BY identity").arg(kHistoryColumns))) {
        setError(queryError(query, QStringLiteral("Could not read play history")), error);
        return result;
    }
    while (query.next())
        result.append(entryFrom(query));
    return result;
}

PlayHistoryEntry UserStateStore::playHistoryFor(const QString& identity, QString* error) const
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT %1 FROM play_history WHERE identity=?").arg(kHistoryColumns));
    query.addBindValue(identity);
    if (!query.exec()) {
        setError(queryError(query, QStringLiteral("Could not read play history")), error);
        return {};
    }
    return query.next() ? entryFrom(query) : PlayHistoryEntry{};
}

bool UserStateStore::updateLocation(const PlayHistoryEntry& where, const PlayHistoryEntry& expected,
                                    QString* error)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "UPDATE play_history SET root_path=?,mp3_rel_path=?,mp3_size=?,cdg_size=? "
        "WHERE identity=? AND coalesce(root_path,'')=? AND coalesce(mp3_rel_path,'')=? "
        "AND coalesce(mp3_size,0)=? AND coalesce(cdg_size,0)=?"));
    query.addBindValue(where.rootPath);
    query.addBindValue(where.mp3RelPath);
    query.addBindValue(where.mp3Size);
    query.addBindValue(where.cdgSize);
    query.addBindValue(where.identity);
    query.addBindValue(expected.rootPath);
    query.addBindValue(expected.mp3RelPath);
    query.addBindValue(expected.mp3Size);
    query.addBindValue(expected.cdgSize);
    if (!query.exec()) {
        setError(queryError(query, QStringLiteral("Could not update the play history location")), error);
        return false;
    }
    return true;
}

QString UserStateStore::preference(const QString& key, QString* error) const
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT value FROM preferences WHERE key=?"));
    query.addBindValue(key);
    if (!query.exec()) {
        setError(queryError(query, QStringLiteral("Could not read a preference")), error);
        return {};
    }
    return query.next() ? query.value(0).toString() : QString();
}

bool UserStateStore::setPreferences(const QList<QPair<QString, QString>>& values, QString* error)
{
    if (!m_database.transaction()) {
        setError(QStringLiteral("Could not save preferences: %1").arg(m_database.lastError().text()), error);
        return false;
    }
    for (const auto& [key, value] : values) {
        if (!setPreference(key, value, error)) {
            undoTransaction();
            return false;
        }
    }
    if (!m_database.commit()) {
        setError(QStringLiteral("Could not save preferences: %1").arg(m_database.lastError().text()), error);
        undoTransaction();
        return false;
    }
    return true;
}

void UserStateStore::undoTransaction()
{
    if (m_database.rollback())
        return;
    // Closing undoes the unfinished transaction; later saves then fail
    // openly instead of seeming to succeed inside it.
    qCWarning(lcUserState).noquote() << "Could not undo unsaved preferences; closing"
                                     << m_databasePath << ":" << m_database.lastError().text();
    close();
}

bool UserStateStore::setPreference(const QString& key, const QString& value, QString* error)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("INSERT INTO preferences(key,value) VALUES(?,?) "
                                 "ON CONFLICT(key) DO UPDATE SET value=excluded.value"));
    query.addBindValue(key);
    query.addBindValue(value);
    if (!query.exec()) {
        setError(queryError(query, QStringLiteral("Could not save a preference")), error);
        return false;
    }
    return true;
}
