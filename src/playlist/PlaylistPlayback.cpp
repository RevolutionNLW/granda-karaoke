#include "playlist/PlaylistPlayback.h"

#include "playlist/PlaylistStore.h"

#include <QTimer>

PlaylistPlayback::PlaylistPlayback(PlaylistStore* store, QObject* parent)
    : QObject(parent)
    , m_store(store)
{
    qRegisterMetaType<PlaylistEntry>();
}

void PlaylistPlayback::startedFromPlaylist(qint64 playlistId, qint64 itemId)
{
    const PlaybackContext next{playlistId, itemId};
    if (m_context && *m_context == next)
        return;
    m_context = next;
    ++m_contextGeneration;
    emit contextChanged();
}

void PlaylistPlayback::clear()
{
    if (!m_context)
        return;
    m_context.reset();
    ++m_contextGeneration;
    emit contextChanged();
}

void PlaylistPlayback::cancelPendingAutoplay()
{
    ++m_finishedGeneration;
}

void PlaylistPlayback::onPlayerStateChanged(KaraokePlayer::State state)
{
    const bool enteredFinished = state == KaraokePlayer::State::Finished
        && m_previousState != KaraokePlayer::State::Finished;
    const bool stateChanged = state != m_previousState;
    if (state != KaraokePlayer::State::Finished
        && m_previousState == KaraokePlayer::State::Finished)
        cancelPendingAutoplay();
    m_previousState = state;
    if (stateChanged)
        emit playbackStateChanged();
    if (!enteredFinished || !m_context || !m_store || !m_store->isOpen()
        || !m_store->autoplay())
        return;

    const PlaybackContext finishedContext = *m_context;
    const quint64 contextGeneration = m_contextGeneration;
    const quint64 requestGeneration = ++m_finishedGeneration;
    QTimer::singleShot(0, this, [this, finishedContext, contextGeneration,
                                 requestGeneration] {
        if (requestGeneration != m_finishedGeneration
            || contextGeneration != m_contextGeneration
            || !m_context || *m_context != finishedContext
            || m_previousState != KaraokePlayer::State::Finished
            || !m_store || !m_store->isOpen() || !m_store->autoplay())
            return;
        const auto next = m_store->itemAfter(finishedContext.playlistId,
                                              finishedContext.itemId);
        if (next)
            emit autoplayRequested(*next);
    });
}
