#pragma once

#include "playlist/PlaylistTypes.h"

#include <QList>
#include <QSqlDatabase>
#include <QString>

#include <optional>

class PlaylistStore {
public:
    static constexpr int SchemaVersion = 1;

    explicit PlaylistStore(QString databasePath);
    ~PlaylistStore();
    PlaylistStore(const PlaylistStore&) = delete;
    PlaylistStore& operator=(const PlaylistStore&) = delete;

    bool open(QString* error = nullptr, const QStringList& libraryRoots = {});
    void close();
    bool isOpen() const;
    QString lastError() const { return m_lastError; }
    QString databasePath() const { return m_databasePath; }

    QList<PlaylistInfo> playlists(QString* error = nullptr) const;
    bool createPlaylist(const QString& name, qint64* id = nullptr,
                        QString* error = nullptr);
    bool renamePlaylist(qint64 id, const QString& name, QString* error = nullptr);
    bool deletePlaylist(qint64 id, QString* error = nullptr);
    std::optional<PlaylistInfo> playlist(qint64 id, QString* error = nullptr) const;

    QList<PlaylistEntry> items(qint64 playlistId, QString* error = nullptr) const;
    std::optional<PlaylistEntry> item(qint64 itemId, QString* error = nullptr) const;
    bool addItem(qint64 playlistId, const SongRef& song, qint64* itemId = nullptr,
                 QString* error = nullptr);
    bool insertItem(qint64 playlistId, int position, const SongRef& song,
                    qint64* itemId = nullptr, QString* error = nullptr);
    bool moveItem(qint64 itemId, int newPosition, QString* error = nullptr);
    bool moveItemUp(qint64 itemId, QString* error = nullptr);
    bool moveItemDown(qint64 itemId, QString* error = nullptr);
    bool removeItem(qint64 itemId, QString* error = nullptr);
    std::optional<PlaylistEntry> itemAfter(qint64 playlistId, qint64 itemId,
                                           QString* error = nullptr) const;

    qint64 lastPlaylistId(QString* error = nullptr) const;
    bool setLastPlaylistId(qint64 id, QString* error = nullptr);
    bool autoplay(QString* error = nullptr) const;
    bool setAutoplay(bool enabled, QString* error = nullptr);
    bool updateSongId(qint64 itemId, qint64 newSongId, QString* error = nullptr);

private:
    friend class PlaylistStoreTestAccess;

    bool ensureOpen(QString* error) const;
    bool ensureSchema(QString* error);
    bool execute(const QString& sql, QString* error = nullptr) const;
    bool recoverCorruptDatabase(const QString& detail, QString* error);
    bool begin(QString* error);
    bool commit(QString* error);
    void rollback();
    bool touchPlaylist(qint64 playlistId, QString* error);
    bool stateValue(const QString& key, QString* value, QString* error) const;
    bool setStateValue(const QString& key, const QString& value, QString* error);
    void setError(const QString& message, QString* error) const;

    QString m_databasePath;
    QString m_connectionName;
    mutable QString m_lastError;
    QSqlDatabase m_database;
};
