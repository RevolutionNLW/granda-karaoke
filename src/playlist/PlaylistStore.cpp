#include "playlist/PlaylistStore.h"

#include "library/Catalogue.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

#include <limits>

namespace {

Q_LOGGING_CATEGORY(lcPlaylistStore, "fks.playlist.store")

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

PlaylistEntry entryFromQuery(const QSqlQuery& query)
{
    return {query.value(0).toLongLong(), query.value(1).toLongLong(),
            query.value(2).toInt(), query.value(3).toLongLong(),
            query.value(4).toString(), query.value(5).toString(),
            query.value(6).toString(), query.value(7).toInt(),
            query.value(8).toString(), query.value(9).toString()};
}

const QString entryColumns = QStringLiteral(
    "id,playlist_id,position,song_id,title,artist,disc_id,track,root_path,mp3_rel_path");

} // namespace

PlaylistStore::PlaylistStore(QString databasePath)
    : m_databasePath(QFileInfo(databasePath).absoluteFilePath())
    , m_connectionName(QStringLiteral("fks-playlists-%1").arg(
          QUuid::createUuid().toString(QUuid::WithoutBraces)))
{
}

PlaylistStore::~PlaylistStore()
{
    close();
}

void PlaylistStore::setError(const QString& message, QString* error) const
{
    m_lastError = message;
    if (error)
        *error = message;
}

bool PlaylistStore::open(QString* error, const QStringList& libraryRoots)
{
    if (isOpen())
        return true;
    QString safetyError;
    if (!Catalogue::storageIsSafe(m_databasePath, {}, libraryRoots, &safetyError)) {
        setError(safetyError, error);
        qCritical(lcPlaylistStore).noquote() << safetyError;
        return false;
    }
    const QFileInfo dbInfo(m_databasePath);
    if (!QDir().mkpath(dbInfo.absolutePath())) {
        setError(QStringLiteral("Could not create playlist database directory: %1")
                     .arg(dbInfo.absolutePath()), error);
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
        setError(QStringLiteral("Could not open playlists: %1").arg(detail), error);
        return false;
    }

    QSqlQuery versionQuery(m_database);
    if (!versionQuery.exec(QStringLiteral("PRAGMA user_version")) || !versionQuery.next()) {
        const QString detail = versionQuery.lastError().text();
        versionQuery.finish();
        close();
        if (dbInfo.exists() && indicatesCorruption(detail)
            && recoverCorruptDatabase(detail, error))
            return open(error, libraryRoots);
        setError(QStringLiteral("Could not read playlist schema version: %1").arg(detail), error);
        return false;
    }
    const int version = versionQuery.value(0).toInt();
    versionQuery.finish();
    if (version > SchemaVersion) {
        const QString message = QStringLiteral(
            "Playlist schema version %1 is newer than supported version %2; refusing to open")
                                    .arg(version).arg(SchemaVersion);
        close();
        setError(message, error);
        return false;
    }
    if (!ensureSchema(error)) {
        const QString detail = error ? *error : m_lastError;
        close();
        if (dbInfo.exists() && indicatesCorruption(detail)
            && recoverCorruptDatabase(detail, error))
            return open(error, libraryRoots);
        return false;
    }
    m_lastError.clear();
    if (error)
        error->clear();
    return true;
}

void PlaylistStore::close()
{
    if (!m_database.isValid())
        return;
    m_database.close();
    m_database = QSqlDatabase();
    QSqlDatabase::removeDatabase(m_connectionName);
}

bool PlaylistStore::isOpen() const
{
    return m_database.isOpen();
}

bool PlaylistStore::ensureOpen(QString* error) const
{
    if (isOpen())
        return true;
    setError(QStringLiteral("Playlist store is not open"), error);
    return false;
}

bool PlaylistStore::execute(const QString& sql, QString* error) const
{
    QSqlQuery query(m_database);
    if (query.exec(sql))
        return true;
    setError(queryError(query, QStringLiteral("Playlist SQL statement failed")), error);
    return false;
}

bool PlaylistStore::ensureSchema(QString* error)
{
    if (!execute(QStringLiteral("PRAGMA foreign_keys=ON"), error)
        || !execute(QStringLiteral("PRAGMA journal_mode=WAL"), error)
        || !execute(QStringLiteral("PRAGMA busy_timeout=5000"), error))
        return false;

    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("PRAGMA user_version")) || !query.next()) {
        setError(queryError(query, QStringLiteral("Could not inspect playlist schema")), error);
        return false;
    }
    const int version = query.value(0).toInt();
    if (version == SchemaVersion)
        return true;
    if (!begin(error))
        return false;
    const QStringList statements = {
        QStringLiteral("CREATE TABLE playlists(id INTEGER PRIMARY KEY, name TEXT NOT NULL, created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL)"),
        QStringLiteral("CREATE TABLE playlist_items(id INTEGER PRIMARY KEY AUTOINCREMENT, playlist_id INTEGER NOT NULL REFERENCES playlists(id) ON DELETE CASCADE, position INTEGER NOT NULL, song_id INTEGER NOT NULL, title TEXT, artist TEXT, disc_id TEXT, track INTEGER NOT NULL DEFAULT 0, root_path TEXT, mp3_rel_path TEXT, added_at INTEGER NOT NULL)"),
        QStringLiteral("CREATE INDEX idx_playlist_items_order ON playlist_items(playlist_id, position)"),
        QStringLiteral("CREATE TABLE app_state(key TEXT PRIMARY KEY, value TEXT)"),
        QStringLiteral("PRAGMA user_version=1")
    };
    for (const QString& statement : statements) {
        if (!query.exec(statement)) {
            rollback();
            setError(queryError(query, QStringLiteral("Could not create playlist schema")), error);
            return false;
        }
    }
    return commit(error);
}

bool PlaylistStore::recoverCorruptDatabase(const QString& detail, QString* error)
{
    const QFileInfo info(m_databasePath);
    const QString stamp = QDateTime::currentDateTimeUtc().toString(
        QStringLiteral("yyyyMMdd-HHmmsszzz"));
    const QString moved = QDir(info.absolutePath()).filePath(
        info.completeBaseName() + QStringLiteral(".corrupt-") + stamp
        + QStringLiteral(".sqlite"));
    if (!QFile::rename(m_databasePath, moved)) {
        setError(QStringLiteral("Playlist database is corrupt (%1) and could not be moved aside")
                     .arg(detail), error);
        return false;
    }
    for (const QString& suffix : {QStringLiteral("-wal"), QStringLiteral("-shm")}) {
        if (QFileInfo::exists(m_databasePath + suffix))
            QFile::rename(m_databasePath + suffix, moved + suffix);
    }
    qWarning(lcPlaylistStore).noquote() << "Moved corrupt playlist database to"
                                        << moved << "because:" << detail;
    return true;
}

bool PlaylistStore::begin(QString* error)
{
    if (m_database.transaction())
        return true;
    setError(QStringLiteral("Could not begin playlist transaction: %1")
                 .arg(m_database.lastError().text()), error);
    return false;
}

bool PlaylistStore::commit(QString* error)
{
    if (m_database.commit())
        return true;
    setError(QStringLiteral("Could not commit playlist transaction: %1")
                 .arg(m_database.lastError().text()), error);
    return false;
}

void PlaylistStore::rollback()
{
    m_database.rollback();
}

QList<PlaylistInfo> PlaylistStore::playlists(QString* error) const
{
    QList<PlaylistInfo> result;
    if (!ensureOpen(error))
        return result;
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral(
            "SELECT p.id,p.name,p.created_at,p.updated_at,count(i.id) FROM playlists p "
            "LEFT JOIN playlist_items i ON i.playlist_id=p.id GROUP BY p.id "
            "ORDER BY p.created_at,p.id"))) {
        setError(queryError(query, QStringLiteral("Could not list playlists")), error);
        return result;
    }
    while (query.next())
        result.append({query.value(0).toLongLong(), query.value(1).toString(),
                       query.value(2).toLongLong(), query.value(3).toLongLong(),
                       query.value(4).toInt()});
    return result;
}

bool PlaylistStore::createPlaylist(const QString& name, qint64* id, QString* error)
{
    if (!ensureOpen(error))
        return false;
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) {
        setError(QStringLiteral("Playlist name must not be empty"), error);
        return false;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO playlists(name,created_at,updated_at) VALUES(?,?,?)"));
    query.addBindValue(trimmed);
    query.addBindValue(now);
    query.addBindValue(now);
    if (!query.exec()) {
        setError(queryError(query, QStringLiteral("Could not create playlist")), error);
        return false;
    }
    if (id)
        *id = query.lastInsertId().toLongLong();
    return true;
}

bool PlaylistStore::renamePlaylist(qint64 id, const QString& name, QString* error)
{
    if (!ensureOpen(error))
        return false;
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) {
        setError(QStringLiteral("Playlist name must not be empty"), error);
        return false;
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "UPDATE playlists SET name=?,updated_at=MAX(updated_at+1,?) WHERE id=?"));
    query.addBindValue(trimmed);
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    query.addBindValue(id);
    if (!query.exec()) {
        setError(queryError(query, QStringLiteral("Could not rename playlist")), error);
        return false;
    }
    if (query.numRowsAffected() == 1)
        return true;
    setError(QStringLiteral("Playlist was not found"), error);
    return false;
}

bool PlaylistStore::deletePlaylist(qint64 id, QString* error)
{
    if (!ensureOpen(error))
        return false;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("DELETE FROM playlists WHERE id=?"));
    query.addBindValue(id);
    if (!query.exec()) {
        setError(queryError(query, QStringLiteral("Could not delete playlist")), error);
        return false;
    }
    if (query.numRowsAffected() == 1)
        return true;
    setError(QStringLiteral("Playlist was not found"), error);
    return false;
}

std::optional<PlaylistInfo> PlaylistStore::playlist(qint64 id, QString* error) const
{
    if (!ensureOpen(error))
        return std::nullopt;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT p.id,p.name,p.created_at,p.updated_at,count(i.id) FROM playlists p "
        "LEFT JOIN playlist_items i ON i.playlist_id=p.id WHERE p.id=? GROUP BY p.id"));
    query.addBindValue(id);
    if (!query.exec()) {
        setError(queryError(query, QStringLiteral("Could not read playlist")), error);
        return std::nullopt;
    }
    if (!query.next())
        return std::nullopt;
    return PlaylistInfo{query.value(0).toLongLong(), query.value(1).toString(),
                        query.value(2).toLongLong(), query.value(3).toLongLong(),
                        query.value(4).toInt()};
}

QList<PlaylistEntry> PlaylistStore::items(qint64 playlistId, QString* error) const
{
    QList<PlaylistEntry> result;
    if (!ensureOpen(error))
        return result;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT %1 FROM playlist_items WHERE playlist_id=? "
                                 "ORDER BY position,id").arg(entryColumns));
    query.addBindValue(playlistId);
    if (!query.exec()) {
        setError(queryError(query, QStringLiteral("Could not list playlist items")), error);
        return result;
    }
    while (query.next())
        result.append(entryFromQuery(query));
    return result;
}

std::optional<PlaylistEntry> PlaylistStore::item(qint64 itemId, QString* error) const
{
    if (!ensureOpen(error))
        return std::nullopt;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT %1 FROM playlist_items WHERE id=?")
                      .arg(entryColumns));
    query.addBindValue(itemId);
    if (!query.exec()) {
        setError(queryError(query, QStringLiteral("Could not read playlist item")), error);
        return std::nullopt;
    }
    return query.next() ? std::optional<PlaylistEntry>(entryFromQuery(query))
                        : std::nullopt;
}

bool PlaylistStore::touchPlaylist(qint64 playlistId, QString* error)
{
    QSqlQuery touch(m_database);
    touch.prepare(QStringLiteral(
        "UPDATE playlists SET updated_at=MAX(updated_at+1,?) WHERE id=?"));
    touch.addBindValue(QDateTime::currentMSecsSinceEpoch());
    touch.addBindValue(playlistId);
    if (!touch.exec()) {
        setError(queryError(touch, QStringLiteral("Could not update playlist")), error);
        return false;
    }
    if (touch.numRowsAffected() == 1)
        return true;
    setError(QStringLiteral("Playlist was not found"), error);
    return false;
}

bool PlaylistStore::addItem(qint64 playlistId, const SongRef& song, qint64* itemId,
                            QString* error)
{
    return insertItem(playlistId, std::numeric_limits<int>::max(), song, itemId, error);
}

bool PlaylistStore::insertItem(qint64 playlistId, int position, const SongRef& song,
                               qint64* itemId, QString* error)
{
    if (!ensureOpen(error) || !begin(error))
        return false;
    QSqlQuery count(m_database);
    count.prepare(QStringLiteral("SELECT count(*) FROM playlist_items WHERE playlist_id=?"));
    count.addBindValue(playlistId);
    if (!count.exec() || !count.next()) {
        rollback();
        setError(queryError(count, QStringLiteral("Could not count playlist items")), error);
        return false;
    }
    const int at = qBound(0, position, count.value(0).toInt());
    QSqlQuery shift(m_database);
    shift.prepare(QStringLiteral(
        "UPDATE playlist_items SET position=position+1 WHERE playlist_id=? AND position>=?"));
    shift.addBindValue(playlistId);
    shift.addBindValue(at);
    if (!shift.exec()) {
        rollback();
        setError(queryError(shift, QStringLiteral("Could not make room in playlist")), error);
        return false;
    }
    QSqlQuery insert(m_database);
    insert.prepare(QStringLiteral(
        "INSERT INTO playlist_items(playlist_id,position,song_id,title,artist,disc_id,track,"
        "root_path,mp3_rel_path,added_at) VALUES(?,?,?,?,?,?,?,?,?,?)"));
    insert.addBindValue(playlistId);
    insert.addBindValue(at);
    insert.addBindValue(song.songId);
    insert.addBindValue(song.title);
    insert.addBindValue(song.artist);
    insert.addBindValue(song.discId);
    insert.addBindValue(song.track);
    insert.addBindValue(song.rootPath);
    insert.addBindValue(song.mp3RelPath);
    insert.addBindValue(QDateTime::currentMSecsSinceEpoch());
    if (!insert.exec() || !touchPlaylist(playlistId, error)) {
        if (insert.lastError().isValid())
            setError(queryError(insert, QStringLiteral("Could not add playlist item")), error);
        rollback();
        return false;
    }
    const qint64 newId = insert.lastInsertId().toLongLong();
    if (!commit(error)) {
        rollback();
        return false;
    }
    if (itemId)
        *itemId = newId;
    return true;
}

bool PlaylistStore::moveItem(qint64 itemId, int newPosition, QString* error)
{
    if (!ensureOpen(error) || !begin(error))
        return false;
    const auto current = item(itemId, error);
    if (!current) {
        rollback();
        if (!error || error->isEmpty())
            setError(QStringLiteral("Playlist item was not found"), error);
        return false;
    }
    QSqlQuery count(m_database);
    count.prepare(QStringLiteral("SELECT count(*) FROM playlist_items WHERE playlist_id=?"));
    count.addBindValue(current->playlistId);
    if (!count.exec() || !count.next()) {
        rollback();
        setError(queryError(count, QStringLiteral("Could not count playlist items")), error);
        return false;
    }
    const int target = qBound(0, newPosition, qMax(0, count.value(0).toInt() - 1));
    if (target == current->position) {
        rollback();
        return false;
    }
    QSqlQuery shift(m_database);
    if (target < current->position) {
        shift.prepare(QStringLiteral(
            "UPDATE playlist_items SET position=position+1 WHERE playlist_id=? "
            "AND position>=? AND position<?"));
        shift.addBindValue(current->playlistId);
        shift.addBindValue(target);
        shift.addBindValue(current->position);
    } else {
        shift.prepare(QStringLiteral(
            "UPDATE playlist_items SET position=position-1 WHERE playlist_id=? "
            "AND position>? AND position<=?"));
        shift.addBindValue(current->playlistId);
        shift.addBindValue(current->position);
        shift.addBindValue(target);
    }
    QSqlQuery move(m_database);
    move.prepare(QStringLiteral("UPDATE playlist_items SET position=? WHERE id=?"));
    move.addBindValue(target);
    move.addBindValue(itemId);
    if (!shift.exec() || !move.exec() || !touchPlaylist(current->playlistId, error)) {
        if (shift.lastError().isValid())
            setError(queryError(shift, QStringLiteral("Could not reorder playlist")), error);
        else if (move.lastError().isValid())
            setError(queryError(move, QStringLiteral("Could not move playlist item")), error);
        rollback();
        return false;
    }
    if (!commit(error)) {
        rollback();
        return false;
    }
    return true;
}

bool PlaylistStore::moveItemUp(qint64 itemId, QString* error)
{
    const auto current = item(itemId, error);
    return current && current->position > 0
        ? moveItem(itemId, current->position - 1, error) : false;
}

bool PlaylistStore::moveItemDown(qint64 itemId, QString* error)
{
    const auto current = item(itemId, error);
    return current ? moveItem(itemId, current->position + 1, error) : false;
}

bool PlaylistStore::removeItem(qint64 itemId, QString* error)
{
    if (!ensureOpen(error) || !begin(error))
        return false;
    const auto current = item(itemId, error);
    if (!current) {
        rollback();
        if (!error || error->isEmpty())
            setError(QStringLiteral("Playlist item was not found"), error);
        return false;
    }
    QSqlQuery remove(m_database);
    remove.prepare(QStringLiteral("DELETE FROM playlist_items WHERE id=?"));
    remove.addBindValue(itemId);
    QSqlQuery compact(m_database);
    compact.prepare(QStringLiteral(
        "UPDATE playlist_items SET position=position-1 WHERE playlist_id=? AND position>?"));
    compact.addBindValue(current->playlistId);
    compact.addBindValue(current->position);
    if (!remove.exec() || !compact.exec() || !touchPlaylist(current->playlistId, error)) {
        if (remove.lastError().isValid())
            setError(queryError(remove, QStringLiteral("Could not remove playlist item")), error);
        else if (compact.lastError().isValid())
            setError(queryError(compact, QStringLiteral("Could not compact playlist")), error);
        rollback();
        return false;
    }
    if (!commit(error)) {
        rollback();
        return false;
    }
    return true;
}

std::optional<PlaylistEntry> PlaylistStore::itemAfter(qint64 playlistId, qint64 itemId,
                                                       QString* error) const
{
    if (!ensureOpen(error))
        return std::nullopt;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT %1 FROM playlist_items WHERE playlist_id=? AND position=("
        "SELECT position+1 FROM playlist_items WHERE id=? AND playlist_id=?) "
        "ORDER BY id LIMIT 1").arg(entryColumns));
    query.addBindValue(playlistId);
    query.addBindValue(itemId);
    query.addBindValue(playlistId);
    if (!query.exec()) {
        setError(queryError(query, QStringLiteral("Could not find next playlist item")), error);
        return std::nullopt;
    }
    return query.next() ? std::optional<PlaylistEntry>(entryFromQuery(query))
                        : std::nullopt;
}

bool PlaylistStore::stateValue(const QString& key, QString* value, QString* error) const
{
    if (!ensureOpen(error))
        return false;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT value FROM app_state WHERE key=?"));
    query.addBindValue(key);
    if (!query.exec()) {
        setError(queryError(query, QStringLiteral("Could not read playlist preference")), error);
        return false;
    }
    *value = query.next() ? query.value(0).toString() : QString();
    return true;
}

bool PlaylistStore::setStateValue(const QString& key, const QString& value, QString* error)
{
    if (!ensureOpen(error))
        return false;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO app_state(key,value) VALUES(?,?) "
        "ON CONFLICT(key) DO UPDATE SET value=excluded.value"));
    query.addBindValue(key);
    query.addBindValue(value);
    if (query.exec())
        return true;
    setError(queryError(query, QStringLiteral("Could not save playlist preference")), error);
    return false;
}

qint64 PlaylistStore::lastPlaylistId(QString* error) const
{
    QString value;
    return stateValue(QStringLiteral("last_playlist_id"), &value, error)
        ? value.toLongLong() : 0;
}

bool PlaylistStore::setLastPlaylistId(qint64 id, QString* error)
{
    return setStateValue(QStringLiteral("last_playlist_id"), QString::number(id), error);
}

bool PlaylistStore::autoplay(QString* error) const
{
    QString value;
    return stateValue(QStringLiteral("autoplay"), &value, error) && value == QLatin1String("1");
}

bool PlaylistStore::setAutoplay(bool enabled, QString* error)
{
    return setStateValue(QStringLiteral("autoplay"), enabled ? QStringLiteral("1")
                                                             : QStringLiteral("0"), error);
}

bool PlaylistStore::updateSongId(qint64 itemId, qint64 newSongId, QString* error)
{
    if (!ensureOpen(error) || !begin(error))
        return false;
    const auto current = item(itemId, error);
    if (!current) {
        rollback();
        if (!error || error->isEmpty())
            setError(QStringLiteral("Playlist item was not found"), error);
        return false;
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("UPDATE playlist_items SET song_id=? WHERE id=?"));
    query.addBindValue(newSongId);
    query.addBindValue(itemId);
    if (!query.exec() || !touchPlaylist(current->playlistId, error)) {
        if (query.lastError().isValid())
            setError(queryError(query, QStringLiteral("Could not relink playlist item")), error);
        rollback();
        return false;
    }
    if (!commit(error)) {
        rollback();
        return false;
    }
    if (query.numRowsAffected() == 1)
        return true;
    setError(QStringLiteral("Playlist item was not found"), error);
    return false;
}
