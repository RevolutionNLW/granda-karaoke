#include "playlist/PlaylistPlayback.h"
#include "playlist/PlaylistStore.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

namespace {

SongRef song(qint64 id, const QString& title)
{
    return {id, title, {}, {}, 0, QStringLiteral("/music"),
            title + QStringLiteral(".mp3")};
}

PlaylistEntry requested(const QSignalSpy& spy, int index = 0)
{
    return qvariant_cast<PlaylistEntry>(spy.at(index).at(0));
}

} // namespace

class TestPlaylistPlayback : public QObject {
    Q_OBJECT

private slots:
    void autoplayOffAndNonFinishedNeverAdvance();
    void finalOrderAndMovingPlayingItemDetermineSuccessor();
    void missingLastAndDeletedOriginsDoNotAdvance();
    void oneEosOneQueuedRequestAndContextClearing();
    void stateChangesAndExplicitCancellationCancelQueuedRequest();
};

void TestPlaylistPlayback::autoplayOffAndNonFinishedNeverAdvance()
{
    QTemporaryDir temporary;
    PlaylistStore store(temporary.filePath(QStringLiteral("playlists.sqlite")));
    QVERIFY(store.open());
    qint64 playlist = 0;
    qint64 a = 0;
    QVERIFY(store.createPlaylist(QStringLiteral("One"), &playlist));
    QVERIFY(store.addItem(playlist, song(1, QStringLiteral("A")), &a));
    QVERIFY(store.addItem(playlist, song(2, QStringLiteral("B"))));
    PlaylistPlayback playback(&store);
    QSignalSpy requests(&playback, &PlaylistPlayback::autoplayRequested);
    playback.startedFromPlaylist(playlist, a);

    playback.onPlayerStateChanged(KaraokePlayer::State::Stopped);
    playback.onPlayerStateChanged(KaraokePlayer::State::Error);
    playback.onPlayerStateChanged(KaraokePlayer::State::Empty);
    playback.onPlayerStateChanged(KaraokePlayer::State::Finished);
    QCoreApplication::processEvents();
    QCOMPARE(requests.count(), 0);

    QVERIFY(store.setAutoplay(true));
    playback.onPlayerStateChanged(KaraokePlayer::State::Playing);
    playback.onPlayerStateChanged(KaraokePlayer::State::Stopped);
    playback.onPlayerStateChanged(KaraokePlayer::State::Error);
    playback.onPlayerStateChanged(KaraokePlayer::State::Empty);
    QCoreApplication::processEvents();
    QCOMPARE(requests.count(), 0);
}

void TestPlaylistPlayback::finalOrderAndMovingPlayingItemDetermineSuccessor()
{
    QTemporaryDir temporary;
    PlaylistStore store(temporary.filePath(QStringLiteral("playlists.sqlite")));
    QVERIFY(store.open());
    QVERIFY(store.setAutoplay(true));
    qint64 playlist = 0;
    QVERIFY(store.createPlaylist(QStringLiteral("One"), &playlist));
    qint64 a = 0;
    qint64 b = 0;
    qint64 c = 0;
    qint64 d = 0;
    QVERIFY(store.addItem(playlist, song(1, QStringLiteral("A")), &a));
    QVERIFY(store.addItem(playlist, song(2, QStringLiteral("B")), &b));
    QVERIFY(store.addItem(playlist, song(3, QStringLiteral("C")), &c));
    QVERIFY(store.addItem(playlist, song(4, QStringLiteral("D")), &d));
    PlaylistPlayback playback(&store);
    QSignalSpy requests(&playback, &PlaylistPlayback::autoplayRequested);

    playback.startedFromPlaylist(playlist, b);
    QVERIFY(store.moveItem(d, 2)); // A, B, D, C
    playback.onPlayerStateChanged(KaraokePlayer::State::Playing);
    playback.onPlayerStateChanged(KaraokePlayer::State::Finished);
    QTRY_COMPARE(requests.count(), 1);
    QCOMPARE(requested(requests).itemId, d);

    requests.clear();
    playback.onPlayerStateChanged(KaraokePlayer::State::Playing);
    QVERIFY(store.moveItem(b, 2)); // A, D, B, C: B's successor is now C
    playback.onPlayerStateChanged(KaraokePlayer::State::Finished);
    QTRY_COMPARE(requests.count(), 1);
    QCOMPARE(requested(requests).itemId, c);

    // Browsing another playlist has no API on PlaylistPlayback and cannot alter context.
    qint64 browsed = 0;
    QVERIFY(store.createPlaylist(QStringLiteral("Browsed"), &browsed));
    QCOMPARE(playback.context()->playlistId, playlist);
    QCOMPARE(playback.context()->itemId, b);
}

void TestPlaylistPlayback::missingLastAndDeletedOriginsDoNotAdvance()
{
    QTemporaryDir temporary;
    PlaylistStore store(temporary.filePath(QStringLiteral("playlists.sqlite")));
    QVERIFY(store.open());
    QVERIFY(store.setAutoplay(true));
    qint64 playlist = 0;
    qint64 first = 0;
    qint64 last = 0;
    QVERIFY(store.createPlaylist(QStringLiteral("One"), &playlist));
    QVERIFY(store.addItem(playlist, song(1, QStringLiteral("A")), &first));
    QVERIFY(store.addItem(playlist, song(2, QStringLiteral("B")), &last));
    PlaylistPlayback playback(&store);
    QSignalSpy requests(&playback, &PlaylistPlayback::autoplayRequested);

    playback.startedFromPlaylist(playlist, last);
    playback.onPlayerStateChanged(KaraokePlayer::State::Playing);
    playback.onPlayerStateChanged(KaraokePlayer::State::Finished);
    QCoreApplication::processEvents();
    QCOMPARE(requests.count(), 0);

    playback.startedFromPlaylist(playlist, first);
    QVERIFY(store.removeItem(first));
    playback.onPlayerStateChanged(KaraokePlayer::State::Playing);
    playback.onPlayerStateChanged(KaraokePlayer::State::Finished);
    QCoreApplication::processEvents();
    QCOMPARE(requests.count(), 0);

    playback.startedFromPlaylist(playlist, last);
    QVERIFY(store.deletePlaylist(playlist));
    playback.onPlayerStateChanged(KaraokePlayer::State::Playing);
    playback.onPlayerStateChanged(KaraokePlayer::State::Finished);
    QCoreApplication::processEvents();
    QCOMPARE(requests.count(), 0);
}

void TestPlaylistPlayback::oneEosOneQueuedRequestAndContextClearing()
{
    QTemporaryDir temporary;
    PlaylistStore store(temporary.filePath(QStringLiteral("playlists.sqlite")));
    QVERIFY(store.open());
    QVERIFY(store.setAutoplay(true));
    qint64 playlist = 0;
    qint64 first = 0;
    QVERIFY(store.createPlaylist(QStringLiteral("One"), &playlist));
    QVERIFY(store.addItem(playlist, song(1, QStringLiteral("A")), &first));
    QVERIFY(store.addItem(playlist, song(2, QStringLiteral("B"))));
    PlaylistPlayback playback(&store);
    QSignalSpy context(&playback, &PlaylistPlayback::contextChanged);
    QSignalSpy requests(&playback, &PlaylistPlayback::autoplayRequested);
    playback.startedFromPlaylist(playlist, first);
    QCOMPARE(context.count(), 1);
    playback.onPlayerStateChanged(KaraokePlayer::State::Playing);
    playback.onPlayerStateChanged(KaraokePlayer::State::Finished);
    playback.onPlayerStateChanged(KaraokePlayer::State::Finished);
    QCOMPARE(requests.count(), 0); // Always queued.
    QTRY_COMPARE(requests.count(), 1);

    playback.onPlayerStateChanged(KaraokePlayer::State::Playing);
    playback.onPlayerStateChanged(KaraokePlayer::State::Finished);
    playback.clear(); // A non-playlist load before the queued request cancels it.
    QCoreApplication::processEvents();
    QCOMPARE(requests.count(), 1);
    QVERIFY(!playback.context());
    QCOMPARE(context.count(), 2);

    playback.onPlayerStateChanged(KaraokePlayer::State::Playing);
    playback.onPlayerStateChanged(KaraokePlayer::State::Finished);
    QCoreApplication::processEvents();
    QCOMPARE(requests.count(), 1);
}

void TestPlaylistPlayback::stateChangesAndExplicitCancellationCancelQueuedRequest()
{
    QTemporaryDir temporary;
    PlaylistStore store(temporary.filePath(QStringLiteral("playlists.sqlite")));
    QVERIFY(store.open());
    QVERIFY(store.setAutoplay(true));
    qint64 playlist = 0;
    qint64 first = 0;
    QVERIFY(store.createPlaylist(QStringLiteral("One"), &playlist));
    QVERIFY(store.addItem(playlist, song(1, QStringLiteral("A")), &first));
    QVERIFY(store.addItem(playlist, song(2, QStringLiteral("B"))));
    PlaylistPlayback playback(&store);
    QSignalSpy requests(&playback, &PlaylistPlayback::autoplayRequested);
    playback.startedFromPlaylist(playlist, first);

    const QList states = {KaraokePlayer::State::Stopped,
                          KaraokePlayer::State::Error,
                          KaraokePlayer::State::Playing};
    for (const KaraokePlayer::State state : states) {
        playback.onPlayerStateChanged(KaraokePlayer::State::Playing);
        playback.onPlayerStateChanged(KaraokePlayer::State::Finished);
        playback.onPlayerStateChanged(state);
        QCoreApplication::processEvents();
        QCOMPARE(requests.count(), 0);
    }

    playback.onPlayerStateChanged(KaraokePlayer::State::Playing);
    playback.onPlayerStateChanged(KaraokePlayer::State::Finished);
    playback.cancelPendingAutoplay();
    QCoreApplication::processEvents();
    QCOMPARE(requests.count(), 0);
    QVERIFY(playback.context());
    QCOMPARE(playback.context()->playlistId, playlist);
    QCOMPARE(playback.context()->itemId, first);
}

QTEST_GUILESS_MAIN(TestPlaylistPlayback)
#include "tst_playlistplayback.moc"
