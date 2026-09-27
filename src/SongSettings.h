#pragma once

#include "SongPair.h"

#include <QHash>
#include <QSet>
#include <QString>

inline constexpr int kMinKey = -6;
inline constexpr int kMaxKey = 6;
inline constexpr int kKeyStep = 1;
inline constexpr int kMinTempo = 70;
inline constexpr int kMaxTempo = 130;
inline constexpr int kTempoStep = 2;

struct SongSettings {
    int keySemitones = 0;
    int tempoPercent = 100;

    SongSettings clamped() const;
};

// A path-independent fingerprint of the CDG content and MP3 audio payload.
// Returns an empty string if either file cannot be read completely.
QString songIdentity(const SongPair& pair);

class ISongSettingsStore {
public:
    virtual ~ISongSettingsStore() = default;

    virtual SongSettings settingsFor(const QString& id) const = 0;
    virtual bool store(const QString& id, const SongSettings& settings,
                       const SongPair& pair) = 0;
    // Whether this song has remembered Key/Tempo (a store that cannot tell
    // says yes, so its answer is always used).
    virtual bool hasSettingsFor(const QString& id) const
    {
        Q_UNUSED(id)
        return true;
    }
    // Forgets every song's Key/Tempo, after keeping a copy of the old file
    // (its path in backupPath). Not every store can.
    virtual bool forgetAll(QString* backupPath = nullptr, QString* error = nullptr)
    {
        Q_UNUSED(backupPath)
        if (error)
            *error = QStringLiteral("Song settings cannot be reset here.");
        return false;
    }
};

class SongSettingsStore final : public ISongSettingsStore {
public:
    explicit SongSettingsStore(const QString& path);

    SongSettings settingsFor(const QString& id) const override;
    bool store(const QString& id, const SongSettings& settings,
               const SongPair& pair) override;
    bool hasSettingsFor(const QString& id) const override { return m_entries.contains(id); }
    bool forgetAll(QString* backupPath = nullptr, QString* error = nullptr) override;

    QString path() const { return m_path; }
    // True when an existing file could not be read or safely preserved, or was
    // written by a newer version: changes are then kept in memory only, so that
    // file is never overwritten with a partial set of songs.
    bool isReadOnly() const { return m_readOnly; }

private:
    struct Entry {
        SongSettings settings;
        QString lastPath;
        QString displayName;
        QString updated;
    };

    enum class LoadResult { Missing, Valid, Corrupt, Unreadable, NewerVersion };

    void load();
    LoadResult readFile(QHash<QString, Entry>& entries, int& version) const;
    bool preserveCorruptFile();
    bool save() const;

    QString m_path;
    QHash<QString, Entry> m_entries;
    bool m_readOnly = false;
    mutable bool m_readOnlyWarningLogged = false;
    bool m_lockFailureWarningLogged = false;
    // Songs changed this session but not yet written (e.g. the lock was busy).
    QSet<QString> m_unsaved;
};
