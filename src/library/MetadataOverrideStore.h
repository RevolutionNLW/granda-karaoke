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
        return artist || title || label || series || trustedDiscId || trustedTrack;
    }
};

class QMutex;

class MetadataOverrideStore {
public:
    // Held while trusted metadata is changed (store then catalogue) and while
    // the store is read and applied to the catalogue, so a background sync
    // can never re-apply a value the user has just changed or cleared.
    static QMutex& synchronisation();

    static constexpr int SchemaVersion = 2;

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

    bool setOverride(const MetadataOverride& value, QString* error = nullptr);
    bool clearOverride(const QString& rootPath, const QString& mp3RelPath,
                       QString* error = nullptr);
    std::optional<MetadataOverride> overrideFor(
        const QString& rootPath, const QString& mp3RelPath,
        QString* error = nullptr) const;
    QList<MetadataOverride> all(QString* error = nullptr) const;

private:
    bool ensureSchema(QString* error);
    bool execute(const QString& sql, QString* error = nullptr) const;
    bool recoverCorruptDatabase(const QString& detail, QString* error);
    void setError(const QString& message, QString* error) const;

    QString m_databasePath;
    QString m_connectionName;
    mutable QString m_lastError;
    QSqlDatabase m_database;
    bool m_recovered = false;
};
