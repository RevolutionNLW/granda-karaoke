#include "SmartAutoplayEnd.h"

#include "AppPreferences.h"
#include "AudibleEnds.h"
#include "playlist/PlaylistPlayback.h"
#include "playlist/PlaylistStore.h"

#include <QFileInfo>
#include <QLoggingCategory>
#include <QTimer>

#include <cstdlib>

namespace {
Q_LOGGING_CATEGORY(lcSmartEnd, "fks.autoplay.end")
}

SmartAutoplayEnd::SmartAutoplayEnd(KaraokePlayer* player, PlaylistPlayback* playback,
                                   PlaylistStore* store, AppPreferences* preferences,
                                   AudibleEnds* ends, QObject* parent)
    : QObject(parent)
    , m_player(player)
    , m_playback(playback)
    , m_store(store)
    , m_preferences(preferences)
    , m_ends(ends)
{
    if (m_player) {
        connect(m_player, &KaraokePlayer::positionChanged, this,
                [this](qint64 positionMs) { onPositionChanged(positionMs); });
        // Each time a song is sung it is asked about, and its file looked
        // at, afresh.
        connect(m_player, &KaraokePlayer::stateChanged, this, [this](KaraokePlayer::State state) {
            if (state != KaraokePlayer::State::Playing && state != KaraokePlayer::State::Paused)
                forgetPlayback();
        });
    }
    if (m_ends) {
        connect(m_ends, &AudibleEnds::fileChecked, this, [this](quint64 token, bool unchanged) {
            if (token == m_checkToken && m_check == Check::Pending) {
                m_check = unchanged ? Check::Unchanged : Check::Changed;
                m_checkAnswered.start();
            }
        });
    }
}

void SmartAutoplayEnd::forgetPlayback()
{
    m_requested.clear();
    m_explained.clear();
    m_checkPath.clear();
    m_check = Check::NotAsked;
    m_checkToken = 0;
}

bool SmartAutoplayEnd::isEnabled() const
{
    return m_preferences && m_preferences->flag(pref::SkipTrailingSilence, true);
}

void SmartAutoplayEnd::onPositionChanged(qint64 positionMs)
{
    if (!m_player || !m_playback || !m_ends || !isEnabled()
        || m_player->state() != KaraokePlayer::State::Playing || !m_playback->context())
        return;
    const QString path = m_player->song().mp3Path;
    if (positionMs >= kAnalyseAfterMs && m_requested != path) {
        m_requested = path;
        m_ends->request(path);
    }
    if (m_checkPath != path) {
        m_checkPath = path;
        m_check = Check::NotAsked;
        m_checkToken = 0;
    }
    // At the end point, whether the file is still the one analysed is looked
    // at in the background; only a fresh answer lets the song end (a pause,
    // or the setting or Autoplay turned off and on, means looking again).
    if (m_check == Check::Unchanged && m_checkAnswered.elapsed() > kCheckFreshMs)
        m_check = Check::NotAsked;
    const std::optional<music::AudibleEnd> end = m_ends->known(path);
    if (end && end->skips() && m_check == Check::NotAsked && positionMs >= end->endAtMs
        && m_store && m_store->isOpen() && m_store->autoplay()) {
        m_check = Check::Pending;
        m_checkToken = m_ends->checkFile(path);
    }
    if (!m_endQueued && dueNow(true)) {
        m_endQueued = true;
        QTimer::singleShot(0, this, &SmartAutoplayEnd::endIfStillDue);
    }
}

void SmartAutoplayEnd::endIfStillDue()
{
    m_endQueued = false;
    if (!dueNow(false))
        return;
    const auto end = m_ends->known(m_player->song().mp3Path);
    qCInfo(lcSmartEnd).noquote()
        << "Smart Autoplay End: moving on at"
        << QString::fromStdString(music::formatTime(m_player->positionMs())) << "of"
        << QFileInfo(m_player->song().mp3Path).fileName() << "("
        << QString::number(double(end->durationMs - m_player->positionMs()) / 1000.0, 'f', 1)
        << "s of silence skipped)";
    m_player->finishEarly();
}

bool SmartAutoplayEnd::dueNow(bool log)
{
    if (!m_player || !m_playback || !m_ends || !isEnabled()
        || m_player->state() != KaraokePlayer::State::Playing || !m_playback->context()
        || !m_store || !m_store->isOpen() || !m_store->autoplay())
        return false;
    const QString path = m_player->song().mp3Path;
    const std::optional<music::AudibleEnd> end = m_ends->known(path);
    const qint64 position = m_player->positionMs();
    if (!end || !end->skips() || position < end->endAtMs)
        return false;
    const auto explain = [this, log, &path](const QString& why) {
        if (log && m_explained != path) {
            m_explained = path;
            qCInfo(lcSmartEnd).noquote() << "Smart Autoplay End:" << QFileInfo(path).fileName()
                                         << "plays to the end:" << why;
        }
        return false;
    };
    // The file playing must be the recording analysed: the same file, and
    // the song not past the length analysed. (The player's own figure for
    // the length is an estimate for some variable-bitrate MP3s; its position
    // is exact, counted from the start as the analysis counts.)
    if (position > end->durationMs + kLengthToleranceMs)
        return explain(QStringLiteral("it has played longer than the analysed length (%1 ms)")
                           .arg(end->durationMs));
    // (Until the look at the file has answered, the song plays on: a drive
    // that does not answer means it plays to its end.)
    if (m_checkPath != path || m_check == Check::NotAsked || m_check == Check::Pending)
        return false;
    if (m_check == Check::Unchanged && m_checkAnswered.elapsed() > kCheckFreshMs) {
        m_check = Check::NotAsked;
        return false;
    }
    if (m_check == Check::Changed)
        return explain(QStringLiteral("the file has changed since it was analysed"));
    // Only ever to move on to the next song.
    const PlaybackContext context = *m_playback->context();
    if (!m_store->itemAfter(context.playlistId, context.itemId))
        return explain(QStringLiteral("no song follows it in the playlist"));
    return true;
}
