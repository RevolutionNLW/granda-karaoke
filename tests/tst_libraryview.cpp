#include "BusTestPlayer.h"
#include "LibraryController.h"
#include "LibraryView.h"
#include "MainWindow.h"
#include "SongSettings.h"
#include "TestMedia.h"

#include <QCryptographicHash>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

namespace {

void writeFile(const QString& path, const QByteArray& bytes)
{
    QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly), qPrintable(file.errorString()));
    QCOMPARE(file.write(bytes), qint64(bytes.size()));
}

void writeSmallPair(const QString& root, int number, const QString& artist,
                    const QString& title)
{
    const QString base = root + QStringLiteral("/AT%1-01 - %2 - %3")
                                    .arg(number, 3, 10, QLatin1Char('0'))
                                    .arg(artist, title);
    writeFile(base + QStringLiteral(".mp3"), "audio");
    writeFile(base + QStringLiteral(".cdg"), "lyrics");
}

QVariantMap snapshot(const QString& root)
{
    QVariantMap result;
    QDirIterator iterator(root, QDir::AllEntries | QDir::NoDotAndDotDot
                                   | QDir::Hidden | QDir::System,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString path = iterator.next();
        const QFileInfo info(path);
        QVariantMap item;
        item.insert(QStringLiteral("dir"), info.isDir());
        item.insert(QStringLiteral("size"), info.size());
        item.insert(QStringLiteral("mtime"), info.lastModified().toMSecsSinceEpoch());
        if (info.isFile()) {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly))
                qFatal("Could not read synthetic library snapshot");
            item.insert(QStringLiteral("sha256"),
                        QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256));
        }
        result.insert(QDir(root).relativeFilePath(path), item);
    }
    return result;
}

} // namespace

class TestLibraryView : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void setupSearchAndSingUsesExistingOpenFlow();
    void keyboardFilteringSelectionRefreshAndFocus();
    void emptySearchAndResultCap();
    void missingRootMessageAndStayOnLibrary();
    void playbackPausesScannerAndShutdownCancels();
};

void TestLibraryView::initTestCase()
{
    QString error;
    QVERIFY2(KaraokePlayer::initializeGStreamer(&error), qPrintable(error));
}

void TestLibraryView::setupSearchAndSingUsesExistingOpenFlow()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("music"));
    const QString base = root + QStringLiteral("/AT001-01 - Test Singer - Golden Song");
    QVERIFY(QDir().mkpath(root));
    QVERIFY(testmedia::writeMp3(base + QStringLiteral(".mp3"), 1200));
    QVERIFY(testmedia::writeCdg(base + QStringLiteral(".cdg"),
                               testmedia::markerCdg(1200, 300)));
    const QVariantMap before = snapshot(root);

    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")));
    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings, &controller);
    window.setShowErrorDialogs(false);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    QTest::mouseClick(window.findButton(), Qt::LeftButton);
    QVERIFY(window.libraryVisible());
    QCOMPARE(window.libraryView()->statusLabel()->text(), QString());
    window.libraryView()->setFolderChooser(
        [root](QWidget*) { return root; });
    QSignalSpy ready(&controller, &LibraryController::libraryReady);
    QTest::mouseClick(window.libraryView()->chooseFolderButton(), Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() >= 1, 5000);

    window.libraryView()->searchBox()->setText(QStringLiteral("Golden Singer"));
    QTRY_COMPARE_WITH_TIMEOUT(window.libraryView()->songResultCount(), 1, 2000);
    QTest::mouseClick(window.libraryView()->resultsList()->viewport(), Qt::LeftButton,
                      Qt::NoModifier,
                      window.libraryView()->resultsList()->visualItemRect(
                          window.libraryView()->resultsList()->item(0)).center());
    QVERIFY(window.libraryView()->singButton()->isEnabled());
    QTest::mouseClick(window.libraryView()->singButton(), Qt::LeftButton);

    QCOMPARE(player.state(), KaraokePlayer::State::Ready);
    QVERIFY(!window.libraryVisible());
    QVERIFY(window.songText().contains(QStringLiteral("AT001-01")));
    QCOMPARE(snapshot(root), before);
}

void TestLibraryView::keyboardFilteringSelectionRefreshAndFocus()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("music"));
    const QString firstBase = root + QStringLiteral("/AT001-01 - Shared Singer - First Song");
    const QString secondBase = root + QStringLiteral("/AT002-01 - Shared Singer - Second Song");
    QVERIFY(QDir().mkpath(root));
    QVERIFY(testmedia::writeMp3(firstBase + QStringLiteral(".mp3"), 1200));
    QVERIFY(testmedia::writeCdg(firstBase + QStringLiteral(".cdg"),
                               testmedia::markerCdg(1200, 300)));
    QVERIFY(testmedia::writeMp3(secondBase + QStringLiteral(".mp3"), 1200));
    QVERIFY(testmedia::writeCdg(secondBase + QStringLiteral(".cdg"),
                               testmedia::markerCdg(1200, 300)));
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")));
    QSignalSpy ready(&controller, &LibraryController::libraryReady);
    QVERIFY(controller.chooseRoot(root));
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() >= 1, 5000);

    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings, &controller);
    window.setShowErrorDialogs(false);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QTest::mouseClick(window.findButton(), Qt::LeftButton);
    LibraryView* view = window.libraryView();

    for (QPushButton* button : view->findChildren<QPushButton*>())
        QCOMPARE(button->focusPolicy(), Qt::NoFocus);
    QCOMPARE(window.findButton()->focusPolicy(), Qt::NoFocus);

    QTest::keyClicks(view->searchBox(), QStringLiteral("Shared Singer"));
    QTRY_COMPARE_WITH_TIMEOUT(view->songResultCount(), 2, 2000);
    QCOMPARE(view->selectedSongId(), 0LL);
    QTest::keyClick(view->searchBox(), Qt::Key_Down);
    const qint64 first = view->selectedSongId();
    QVERIFY(first != 0);
    QTest::keyClick(view->searchBox(), Qt::Key_Down);
    const qint64 second = view->selectedSongId();
    QVERIFY(second != 0 && second != first);
    QTest::keyClick(view->searchBox(), Qt::Key_Up);
    QCOMPARE(view->selectedSongId(), first);
    QTest::keyClick(view->searchBox(), Qt::Key_Down);
    QCOMPARE(view->selectedSongId(), second);

    const QString typed = view->searchBox()->text();
    QVERIFY(QMetaObject::invokeMethod(&controller, "libraryReady", Qt::DirectConnection));
    QCOMPARE(view->searchBox()->text(), typed);
    QCOMPARE(view->selectedSongId(), second);

    view->resultsList()->clearSelection();
    view->resultsList()->setCurrentRow(-1);
    QTest::keyClick(view->searchBox(), Qt::Key_Return);
    QCOMPARE(player.state(), KaraokePlayer::State::Ready);
    QVERIFY(!window.libraryVisible());

    QTest::mouseClick(window.findButton(), Qt::LeftButton);
    QVERIFY(window.libraryVisible());
    QTest::keyClick(view->searchBox(), Qt::Key_Escape);
    QVERIFY(!window.libraryVisible());
}

void TestLibraryView::emptySearchAndResultCap()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("music"));
    for (int i = 0; i < 205; ++i)
        writeSmallPair(root, i, QStringLiteral("Cap Singer"),
                       QStringLiteral("Common Song %1").arg(i));
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")));
    LibraryView view(&controller);
    view.show();
    QSignalSpy ready(&controller, &LibraryController::libraryReady);
    QVERIFY(controller.chooseRoot(root));
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() >= 1, 7000);

    view.searchBox()->clear();
    view.refreshSearch();
    QCOMPARE(view.songResultCount(), 0);
    QVERIFY(view.hintLabel()->isVisible());
    QVERIFY(!view.resultsList()->isVisible());

    view.searchBox()->setText(QStringLiteral("Common Song"));
    QTRY_COMPARE_WITH_TIMEOUT(view.songResultCount(), 200, 3000);
    QCOMPARE(view.resultsList()->count(), 201);
    QCOMPARE(view.resultsList()->item(200)->text(),
             QStringLiteral("More songs match - type more words"));
}

void TestLibraryView::missingRootMessageAndStayOnLibrary()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("music"));
    const QString disconnected = temporary.filePath(QStringLiteral("music-disconnected"));
    writeSmallPair(root, 1, QStringLiteral("Missing Singer"), QStringLiteral("Missing Song"));
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")));
    QSignalSpy ready(&controller, &LibraryController::libraryReady);
    QVERIFY(controller.chooseRoot(root));
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() >= 1, 5000);

    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    MainWindow window(&player, &settings, &controller);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QTest::mouseClick(window.findButton(), Qt::LeftButton);
    LibraryView* view = window.libraryView();
    view->searchBox()->setText(QStringLiteral("Missing Singer"));
    QTRY_COMPARE_WITH_TIMEOUT(view->songResultCount(), 1, 2000);
    view->resultsList()->setCurrentRow(0);
    QVERIFY(QDir().rename(root, disconnected));
    view->activate();
    QCOMPARE(view->statusLabel()->text(), QStringLiteral("Music drive not connected"));
    QTest::keyClick(view->searchBox(), Qt::Key_Return);
    QVERIFY(window.libraryVisible());
    QCOMPARE(view->messageLabel()->text(),
             QStringLiteral("This song's music drive is not connected."));

    QVERIFY(QDir().rename(disconnected, root));
    const QStringList mp3s = QDir(root).entryList({QStringLiteral("*.mp3")}, QDir::Files);
    QCOMPARE(mp3s.size(), 1);
    QVERIFY(QFile::remove(QDir(root).filePath(mp3s.first())));
    QTest::keyClick(view->searchBox(), Qt::Key_Return);
    QVERIFY(window.libraryVisible());
    QCOMPARE(view->messageLabel()->text(),
             QStringLiteral("This song can't be found. The library will be checked again."));
}

void TestLibraryView::playbackPausesScannerAndShutdownCancels()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("music"));
    const QString playbackBase = temporary.filePath(QStringLiteral("playback/Long Song"));
    QVERIFY(QDir().mkpath(QFileInfo(playbackBase).absolutePath()));
    QVERIFY(testmedia::writeMp3(playbackBase + QStringLiteral(".mp3"), 4000));
    QVERIFY(testmedia::writeCdg(playbackBase + QStringLiteral(".cdg"),
                               testmedia::markerCdg(4000, 300)));
    for (int i = 0; i < 400; ++i)
        writeSmallPair(root, i, QStringLiteral("Slow Singer"),
                       QStringLiteral("Slow Song %1").arg(i));

    auto controller = std::make_unique<LibraryController>(
        temporary.filePath(QStringLiteral("app/library.sqlite")));
    BusTestPlayer player;
    SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
    auto window = std::make_unique<MainWindow>(&player, &settings, controller.get());
    QVERIFY(window->openSong(playbackBase + QStringLiteral(".mp3")));
    player.play();
    QCOMPARE(player.state(), KaraokePlayer::State::Playing);
    QVERIFY(controller->scannerPaused());
    QSignalSpy progress(controller.get(), &LibraryController::progressChanged);
    QVERIFY(controller->chooseRoot(root));
    QTest::qWait(150);
    QCOMPARE(progress.count(), 0);
    player.pause();
    QCOMPARE(player.state(), KaraokePlayer::State::Paused);
    QVERIFY(controller->scannerPaused());
    player.stop();
    QCOMPARE(player.state(), KaraokePlayer::State::Stopped);
    QVERIFY(!controller->scannerPaused());
    QTRY_VERIFY_WITH_TIMEOUT(progress.count() > 0, 3000);

    controller->setPlaybackActive(true);
    window.reset();
    QElapsedTimer timer;
    timer.start();
    controller.reset();
    QVERIFY2(timer.elapsed() < 5000,
             qPrintable(QStringLiteral("Shutdown took %1 ms").arg(timer.elapsed())));
}

QTEST_MAIN(TestLibraryView)
#include "tst_libraryview.moc"
