#include "RememberedSettings.h"

#include "LibraryController.h"

#include <QStringList>

RememberedSettingsIndex::RememberedSettingsIndex(const ISongSettingsStore* store,
                                                 const LibraryController* library)
    : m_store(store)
    , m_library(library)
{
}

std::optional<SongSettings> RememberedSettingsIndex::settingsFor(qint64 songId)
{
    if (songId <= 0)
        return std::nullopt;
    if (!m_valid)
        rebuild();
    const auto it = m_bySong.constFind(songId);
    return it != m_bySong.cend() ? std::optional<SongSettings>(*it) : std::nullopt;
}

void RememberedSettingsIndex::invalidate()
{
    m_valid = false;
    m_learned.clear();
    m_learnedIdentity.clear();
}

void RememberedSettingsIndex::learn(qint64 songId, const QString& identity,
                                    const std::optional<SongSettings>& settings)
{
    if (songId <= 0 || identity.isEmpty())
        return;
    // What the files say beats where they were last seen.
    const QString previous = m_learnedIdentity.value(songId);
    if (!previous.isEmpty() && previous != identity)
        m_learned[previous].remove(songId);
    m_learned[identity].insert(songId);
    m_learnedIdentity.insert(songId, identity);
    if (!m_valid)
        return;  // the next lookup rebuilds, with this
    for (const qint64 song : std::as_const(m_learned[identity])) {
        if (settings)
            m_bySong.insert(song, settings->clamped());
        else
            m_bySong.remove(song);
    }
    if (settings) {
        // The identity is now remembered at this song's MP3: a song guessed
        // from its old place no longer has it (unless its own files do).
        const qint64 guessed = m_songByPath.value(identity);
        if (guessed != 0 && guessed != songId && !m_learnedIdentity.contains(guessed))
            m_bySong.remove(guessed);
        m_songByPath.insert(identity, songId);
    }
}

void RememberedSettingsIndex::rebuild()
{
    m_bySong.clear();
    m_songByPath.clear();
    m_valid = true;
    if (!m_store || !m_library)
        return;
    const QList<RememberedSongSettings> songs = m_store->rememberedSongs();
    QStringList paths;
    paths.reserve(songs.size());
    for (const RememberedSongSettings& song : songs) {
        if (!song.lastPath.isEmpty())
            paths.append(song.lastPath);
    }
    const QHash<QString, qint64> songIds = m_library->songIdsForRememberedFiles(paths);
    QHash<qint64, QString> updated;
    QHash<qint64, QString> identityOf;
    for (const RememberedSongSettings& song : songs) {
        const qint64 songId = songIds.value(song.lastPath);
        if (songId <= 0 || m_learnedIdentity.contains(songId))
            continue;
        // Two identities last seen at the same file (its content changed):
        // the one remembered most recently is the file's own.
        if (const auto other = updated.constFind(songId); other != updated.cend()) {
            if (*other >= song.updated)
                continue;
            m_songByPath.remove(identityOf.value(songId));
        }
        updated.insert(songId, song.updated);
        identityOf.insert(songId, song.identity);
        m_bySong.insert(songId, song.settings.clamped());
        m_songByPath.insert(song.identity, songId);
    }
    // Songs whose own files were read this session have their own settings.
    for (auto it = m_learned.cbegin(); it != m_learned.cend(); ++it) {
        if (it.value().isEmpty() || !m_store->hasSettingsFor(it.key()))
            continue;
        const SongSettings settings = m_store->settingsFor(it.key()).clamped();
        for (const qint64 songId : it.value())
            m_bySong.insert(songId, settings);
    }
}
