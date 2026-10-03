#pragma once

#include <QList>
#include <QSqlDatabase>
#include <QString>

#include <optional>

// Trusted, user-owned metadata for one song file: a correction made while
// reviewing the collection ("manual"), or clean metadata supplied when the
// song was added ("import"). Every value is optional; an unset value keeps the
// automatic one. Automatic reprocessing never overrides a set value.
struct MetadataOverride {
    QString rootPath;
    QString mp3RelPath;
    std::optional<QString> artist;
    std::optional<QString> title;
    std::optional<QString> label;
    std::optional<QString> series;
    std::optional<QString> trustedDiscId;
    std::optional<int> trustedTrack;
    // The key the backing track is recorded in, as chosen by the user
    // (0-23, music::MusicalKey::index). Never the Key +/- transpose, which is
    // kept separately with Key/Tempo; shown in place of a detected key.
    std::optional<int> originalKey;
    QString origin = QStringLiteral("manual");  // "manual" or "import"
    qint64 createdAt = 0;
    qint64 updatedAt = 0;
    // A snapshot of the automatic result when the values were entered, kept
    // for people and for re-finding the song if its music folder moves.
    QString autoArtist;
    QString autoTitle;
    QString discId;
    int track = 0;
    QString fileName;

    bool hasValues() const
    {
        return artist || title || label || series || trustedDiscId || trustedTrack || originalKey;
    }
};

// A stored value for a song that is now in the active music folder at
// another path (the folder moved, e.g. a new drive letter): its store row
// belongs at that song's identity from now on.
struct MovedMetadataOverride {
    MetadataOverride stored;
    QString rootPath;
    QString mp3RelPath;
};

class QMutex;

class MetadataOverrideStore {
public:
    // Held while trusted metadata is changed (store then catalogue) and while
    // the store is read and applied to the catalogue, so a background sync
    // can never re-apply a value the user has just changed or cleared.
    static QMutex& synchronisation();

    static constexpr int SchemaVersion = 3;

    explicit MetadataOverrideStore(QString databasePath);
    ~MetadataOverrideStore();
    MetadataOverrideStore(const MetadataOverrideStore&) = delete;
    MetadataOverrideStore& operator=(const MetadataOverrideStore&) = delete;

    // A damaged store is normally moved aside and replaced by an empty one
    // (the application then re-seeds it from the catalogue's mirror). With
    // recoverCorrupt=false a damaged store is left alone and open() fails.
    bool open(QString* error = nullptr, const QStringList& libraryRoots = {},
              bool recoverCorrupt = true);
    bool recoveredFromCorruption() const { return m_recovered; }
    void close();
    bool isOpen() const;
    QString databasePath() const { return m_databasePath; }
    QString lastError() const { return m_lastError; }

    // An established store holds every correction there is: it has rows, or
    // the application started it (empty, or restored from the catalogue's
    // mirror). A store that is not established was just created - the file
    // was lost, or damaged and set aside - and must never be applied to the
    // catalogue, or its emptiness would erase the corrections mirrored there.
    bool isEstablished(QString* error = nullptr) const;
    // Stores every value and marks the store established, in one transaction:
    // all of them or, on any error, nothing. Values may only be given while
    // the store is empty; with none it just marks the store.
    bool establish(const QList<MetadataOverride>& values, QString* error = nullptr);

    bool setOverride(const MetadataOverride& value, QString* error = nullptr);
    bool clearOverride(const QString& rootPath, const QString& mp3RelPath,
                       QString* error = nullptr);
    // Copies a stored row to the song file it now belongs to, keeping every
    // value and timestamp. Fails, changing nothing, if the row changed since
    // it was read or a row is already stored for the new song file.
    bool copyOverride(const MovedMetadataOverride& move, QString* error = nullptr);
    // Removes rows (moved or replaced ones) in one transaction, each only if
    // it is still exactly as it was read (same last change).
    bool removeOverrides(const QList<MetadataOverride>& rows, QString* error = nullptr);
    // Saves, or clears, a song's row and removes the older copies of it saved
    // at another folder's path, in one transaction, so the song's row is the
    // only one left. Every copy must still be exactly as it was read;
    // otherwise, or on any error, nothing changes.
    bool setOverrideAndRemoveCopies(const MetadataOverride& value,
                                    const QList<MetadataOverride>& copies,
                                    QString* error = nullptr);
    bool clearOverrideAndCopies(const QString& rootPath, const QString& mp3RelPath,
                                const QList<MetadataOverride>& copies,
                                QString* error = nullptr);
    std::optional<MetadataOverride> overrideFor(
        const QString& rootPath, const QString& mp3RelPath,
        QString* error = nullptr) const;
    QList<MetadataOverride> all(QString* error = nullptr) const;

private:
    // Inside an open transaction: removes each copy, which must match exactly one row.
    bool removeCopiesExactly(const QList<MetadataOverride>& copies, QString* error);
    bool ensureSchema(QString* error);
    bool ensureStateTable(QString* error);
    bool execute(const QString& sql, QString* error = nullptr) const;
    bool recoverCorruptDatabase(const QString& detail, QString* error);
    void setError(const QString& message, QString* error) const;

    QString m_databasePath;
    QString m_connectionName;
    mutable QString m_lastError;
    QSqlDatabase m_database;
    bool m_recovered = false;
};
