#pragma once

#include "SongSettings.h"

#include <QHash>
#include <QSet>
#include <QString>

#include <optional>

class LibraryController;

// Remembered Key/Tempo by catalogue song, so they can be shown and changed
// before a song is loaded: on library and playlist rows, and by Key/Tempo for
// a selected song. Song settings belong to a song's content identity, which
// takes reading its files. So each remembered song is first found in the
// catalogue by the MP3 it was last remembered for (nothing on the music drive
// is read to show it), and every identity worked out from a song's own files
// this session (loaded, or changed before playing) takes precedence over that
// guess. The identity stays authoritative: a song loaded for singing always
// starts from its own remembered settings.
class RememberedSettingsIndex {
public:
    RememberedSettingsIndex(const ISongSettingsStore* store, const LibraryController* library);

    // The song's remembered Key/Tempo, if it has any.
    std::optional<SongSettings> settingsFor(qint64 songId);
    // The song's files were just found to have this identity, remembered
    // with these settings (none: nothing is remembered for it).
    void learn(qint64 songId, const QString& identity, const std::optional<SongSettings>& settings);
    // Looked up afresh when next asked (the catalogue or the store changed).
    // What was learned goes too: a rebuilt catalogue may give its song
    // numbers to other songs.
    void invalidate();

private:
    void rebuild();

    const ISongSettingsStore* m_store;
    const LibraryController* m_library;
    bool m_valid = false;
    QHash<qint64, SongSettings> m_bySong;
    // The song each identity's last-remembered MP3 is (a guess by path).
    QHash<QString, qint64> m_songByPath;
    // Identities worked out from the songs' own files since the catalogue
    // last changed; exact copies of one song share one.
    QHash<QString, QSet<qint64>> m_learned;
    QHash<qint64, QString> m_learnedIdentity;
};
