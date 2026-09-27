#pragma once

#include <QList>
#include <QSqlDatabase>
#include <QString>

// Play history of one karaoke version, keyed by its content identity (the
// same identity as remembered Key/Tempo), so it survives rebuilding the
// catalogue, renaming or moving the files and any change to song names.
// Byte-identical copies share one history; different versions never do.
struct PlayHistoryEntry {
    QString identity;
    int playCount = 0;
    qint64 lastPlayedMs = 0;
    // Where the song was last seen: a hint for re-finding it cheaply. The
    // identity is authoritative; the files are re-checked when this is stale.
    QString rootPath;
    QString mp3RelPath;
    qint64 mp3Size = 0;
    qint64 cdgSize = 0;
};

// User-owned state that must outlive the rebuildable catalogue: play history
// and preferences. Kept in the application's data folder (never under a music
// folder), in its own small database.
class QMutex;

class UserStateStore {
public:
    // Held while play history is recorded, and while a scan reads it to
    // rebuild the catalogue's copy or records where a moved song now is, so
    // neither can overwrite the other's newer values.
    static QMutex& synchronisation();

    static constexpr int SchemaVersion = 1;

    explicit UserStateStore(QString databasePath);
    ~UserStateStore();
    UserStateStore(const UserStateStore&) = delete;
    UserStateStore& operator=(const UserStateStore&) = delete;

    // Refused, untouched, inside any known library root. A damaged store is
    // set aside (kept for inspection) and replaced only when recoverCorrupt.
    bool open(QString* error = nullptr, const QStringList& libraryRoots = {},
              bool recoverCorrupt = true);
    void close();
    bool isOpen() const;
    QString databasePath() const { return m_databasePath; }

    // Counts one play; returns the updated entry. The stored location is
    // replaced only by a complete one (root, path and both sizes).
    bool recordPlay(const PlayHistoryEntry& where, qint64 playedAtMs,
                    PlayHistoryEntry* updated = nullptr, QString* error = nullptr);
    QList<PlayHistoryEntry> playHistory(QString* error = nullptr) const;
    PlayHistoryEntry playHistoryFor(const QString& identity, QString* error = nullptr) const;
    // Records where a song was found again after it moved, only if the stored
    // location is still `expected` (a newer play's location always wins).
    bool updateLocation(const PlayHistoryEntry& where, const PlayHistoryEntry& expected,
                        QString* error = nullptr);

    QString preference(const QString& key, QString* error = nullptr) const;
    bool setPreference(const QString& key, const QString& value, QString* error = nullptr);

private:
    bool ensureSchema(QString* error);
    bool recoverCorruptDatabase(const QString& detail, QString* error);
    void setError(const QString& message, QString* error) const;

    QString m_databasePath;
    QString m_connectionName;
    mutable QString m_lastError;
    QSqlDatabase m_database;
};
