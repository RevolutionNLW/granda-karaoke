#pragma once

#include "KaraokePlayer.h"

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QString>

class AppPreferences;
class AudibleEnds;
class PlaylistPlayback;
class PlaylistStore;

// "Skip trailing silence during Autoplay" (Settings > Playback, on unless
// turned off). While a playlist song plays with Autoplay on and another song
// follows it in the playlist, the song ends a second after its music does
// (see music/AudibleEnd.h) instead of at the end of the file, and Autoplay
// moves on exactly as at a natural end. Everything else (a song sung from
// the library or opened from a file, a preview, Autoplay off, the last song
// of a playlist, a song not analysed or whose length is not as analysed)
// plays to the real end of the file.
class SmartAutoplayEnd : public QObject {
    Q_OBJECT

public:
    // A song is analysed once it has played this long (a song skipped
    // straight away is never read twice).
    static constexpr qint64 kAnalyseAfterMs = 2000;
    // How far past the analysed length the player may be (its own count of
    // the length is only an estimate for some MP3s, so it is not used).
    static constexpr qint64 kLengthToleranceMs = 1500;
    // How long a look at the file (that it is the one analysed) stays good.
    static constexpr qint64 kCheckFreshMs = 1000;

    SmartAutoplayEnd(KaraokePlayer* player, PlaylistPlayback* playback, PlaylistStore* store,
                     AppPreferences* preferences, AudibleEnds* ends, QObject* parent = nullptr);

    bool isEnabled() const;

private:
    void onPositionChanged(qint64 positionMs);
    // Ends the song if everything above still holds (run after the
    // player's tick, never inside it).
    void endIfStillDue();
    bool dueNow(bool log);
    void forgetPlayback();

    QPointer<KaraokePlayer> m_player;
    QPointer<PlaylistPlayback> m_playback;
    PlaylistStore* m_store;
    QPointer<AppPreferences> m_preferences;
    QPointer<AudibleEnds> m_ends;
    QString m_requested;   // the file last asked about
    QString m_explained;   // the file whose "plays to the end" was last logged
    bool m_endQueued = false;
    // Whether the file is still the one analysed: looked at once a playback.
    enum class Check { NotAsked, Pending, Unchanged, Changed };
    QString m_checkPath;
    Check m_check = Check::NotAsked;
    quint64 m_checkToken = 0;
    QElapsedTimer m_checkAnswered;
};
