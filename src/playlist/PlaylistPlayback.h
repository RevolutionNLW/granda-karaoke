#pragma once

#include "KaraokePlayer.h"
#include "playlist/PlaylistTypes.h"

#include <QObject>

#include <optional>

class PlaylistStore;

struct PlaybackContext {
    qint64 playlistId = 0;
    qint64 itemId = 0;

    bool operator==(const PlaybackContext&) const = default;
};

class PlaylistPlayback : public QObject {
    Q_OBJECT

public:
    explicit PlaylistPlayback(PlaylistStore* store, QObject* parent = nullptr);

    std::optional<PlaybackContext> context() const { return m_context; }
    KaraokePlayer::State playerState() const { return m_previousState; }
    void startedFromPlaylist(qint64 playlistId, qint64 itemId);
    void clear();
    void cancelPendingAutoplay();

public slots:
    void onPlayerStateChanged(KaraokePlayer::State state);

signals:
    void contextChanged();
    void playbackStateChanged();
    void autoplayRequested(PlaylistEntry next);

private:
    PlaylistStore* m_store;
    std::optional<PlaybackContext> m_context;
    KaraokePlayer::State m_previousState = KaraokePlayer::State::Empty;
    quint64 m_contextGeneration = 0;
    quint64 m_finishedGeneration = 0;
};
