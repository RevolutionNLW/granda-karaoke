#include "BusTestPlayer.h"
#include "LibraryController.h"
#include "LibraryResultsModel.h"
#include "LibraryView.h"
#include "MainWindow.h"
#include "SongSettings.h"
#include "TestMedia.h"
#include "library/Catalogue.h"

#include <QCryptographicHash>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMimeData>
#include <QPushButton>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

#include <memory>

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
    void emptySearchBrowsesOrderedCatalogueAndKeepsSelection();
    void emptySearchAndResultCap();
    void rootChangeRefreshesBrowseAndSelection();
    void scannerFailureInvalidatesBrowseCache();
    void browseAndModelScale();
    void missingRootMessageAndStayOnLibrary();
    void protectedStorageInsideChosenRootIsRefused();
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
                      window.libraryView()->resultsList()->visualRect(
                          window.libraryView()->resultsList()->model()->index(0, 0)).center());
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
    view->resultsList()->setCurrentIndex({});
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
    QCOMPARE(view.songResultCount(), 205);
    QVERIFY(view.hintLabel()->isVisible());
    QVERIFY(view.resultsList()->isVisible());

    view.searchBox()->setText(QStringLiteral("Common Song"));
    QTRY_COMPARE_WITH_TIMEOUT(view.songResultCount(), 200, 3000);
    QCOMPARE(view.resultsList()->model()->rowCount(), 201);
    QCOMPARE(view.resultsList()->model()->index(200, 0).data(Qt::DisplayRole).toString(),
             QStringLiteral("More songs match - type more words"));
}

void TestLibraryView::emptySearchBrowsesOrderedCatalogueAndKeepsSelection()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("music"));
    writeSmallPair(root, 1, QStringLiteral("Zulu Singer"),
                   QStringLiteral("Last Named Song"));
    writeSmallPair(root, 2, QStringLiteral("alpha Singer"),
                   QStringLiteral("First Named Song"));
    const QString unresolved = root + QStringLiteral("/SGB39-02");
    writeFile(unresolved + QStringLiteral(".mp3"), "audio");
    writeFile(unresolved + QStringLiteral(".cdg"), "lyrics");

    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")));
    LibraryView view(&controller);
    view.setPlaylistAvailable(true);
    view.show();
    QSignalSpy ready(&controller, &LibraryController::libraryReady);
    QVERIFY(controller.chooseRoot(root));
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() >= 1, 5000);

    view.searchBox()->clear();
    view.refreshSearch();
    QCOMPARE(view.songResultCount(), 3);
    QCOMPARE(view.resultsList()->model()->rowCount(), 3);
    const QAbstractItemModel* model = view.resultsList()->model();
    QCOMPARE(model->index(0, 0).data(LibraryResultsModel::ArtistRole).toString(),
             QStringLiteral("alpha Singer"));
    QCOMPARE(model->index(1, 0).data(LibraryResultsModel::ArtistRole).toString(),
             QStringLiteral("Zulu Singer"));
    QCOMPARE(model->index(2, 0).data(LibraryResultsModel::ArtistRole).toString(),
             QString());
    QCOMPARE(model->index(2, 0).data(Qt::DisplayRole).toString(),
             QStringLiteral("Disc SGB39 - Track 02"));

    view.searchBox()->setText(QStringLiteral("First Named"));
    QTRY_COMPARE_WITH_TIMEOUT(view.songResultCount(), 1, 2000);
    const QModelIndex selected = view.resultsList()->model()->index(0, 0);
    view.resultsList()->setCurrentIndex(selected);
    const qint64 keepSongId = view.selectedSongId();
    QVERIFY(keepSongId != 0);

    QElapsedTimer clearing;
    clearing.start();
    view.searchBox()->setText(QStringLiteral("   "));
    const qint64 clearMs = clearing.elapsed();
    QCOMPARE(view.songResultCount(), 3);
    QCOMPARE(view.selectedSongId(), keepSongId);
    QVERIFY2(clearMs < 200,
             qPrintable(QStringLiteral("Cached clear took %1 ms").arg(clearMs)));

    QSignalSpy sing(&view, &LibraryView::singRequested);
    QSignalSpy add(&view, &LibraryView::addRequested);
    QTest::keyClick(view.searchBox(), Qt::Key_Return);
    QCOMPARE(sing.count(), 1);
    QCOMPARE(sing.first().first().toLongLong(), keepSongId);
    QTest::mouseClick(view.addToPlaylistButton(), Qt::LeftButton);
    QCOMPARE(add.count(), 1);
    QCOMPARE(add.first().first().toLongLong(), keepSongId);

    std::unique_ptr<QMimeData> mimeData(
        view.resultsList()->model()->mimeData({view.resultsList()->currentIndex()}));
    QVERIFY(mimeData);
    QVERIFY(mimeData->hasFormat(QStringLiteral("application/x-fks-song-id")));
    QVERIFY(!mimeData->hasFormat(
        QStringLiteral("application/x-qabstractitemmodeldatalist")));
    QCOMPARE(mimeData->data(QStringLiteral("application/x-fks-song-id")).toLongLong(),
             keepSongId);
}

void TestLibraryView::rootChangeRefreshesBrowseAndSelection()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString rootA = temporary.filePath(QStringLiteral("music-a"));
    const QString rootB = temporary.filePath(QStringLiteral("music-b"));
    writeSmallPair(rootA, 1, QStringLiteral("Root A Singer"),
                   QStringLiteral("Root A Song"));
    writeSmallPair(rootB, 2, QStringLiteral("Root B Singer"),
                   QStringLiteral("Root B Song"));

    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")));
    LibraryView view(&controller);
    view.setPlaylistAvailable(true);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    QSignalSpy finished(&controller, &LibraryController::scanFinished);
    QVERIFY(controller.chooseRoot(rootA));
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 5000);

    view.searchBox()->clear();
    view.refreshSearch();
    QCOMPARE(view.songResultCount(), 1);
    const QModelIndex rootAIndex = view.resultsList()->model()->index(0, 0);
    QCOMPARE(rootAIndex.data(Qt::DisplayRole).toString(), QStringLiteral("Root A Song"));
    view.resultsList()->setCurrentIndex(rootAIndex);
    const qint64 rootASongId = view.selectedSongId();
    QVERIFY(rootASongId != 0);
    QVERIFY(view.singButton()->isEnabled());
    QVERIFY(view.addToPlaylistButton()->isEnabled());

    finished.clear();
    QVERIFY(controller.chooseRoot(rootB));
    QCOMPARE(view.songResultCount(), 0);
    QCOMPARE(view.resultsList()->model()->rowCount(), 0);
    QCOMPARE(view.selectedSongId(), 0LL);
    QVERIFY(!view.singButton()->isEnabled());
    QVERIFY(!view.addToPlaylistButton()->isEnabled());

    QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(view.songResultCount(), 1, 2000);
    const QModelIndex rootBIndex = view.resultsList()->model()->index(0, 0);
    QCOMPARE(rootBIndex.data(Qt::DisplayRole).toString(), QStringLiteral("Root B Song"));
    QVERIFY(rootBIndex.data(LibraryResultsModel::SongIdRole).toLongLong() != rootASongId);
    QCOMPARE(view.selectedSongId(), 0LL);
}

void TestLibraryView::scannerFailureInvalidatesBrowseCache()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("music"));
    const QString databasePath = temporary.filePath(QStringLiteral("app/library.sqlite"));
    writeSmallPair(root, 1, QStringLiteral("Cached Singer"),
                   QStringLiteral("Cached Song"));

    LibraryController controller(databasePath);
    LibraryView view(&controller);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    QSignalSpy finished(&controller, &LibraryController::scanFinished);
    QVERIFY(controller.chooseRoot(root));
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 5000);
    view.searchBox()->clear();
    view.refreshSearch();
    QCOMPARE(view.songResultCount(), 1);

    const QString connection = QStringLiteral("library-failure-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                          connection);
        database.setDatabaseName(databasePath);
        QVERIFY(database.open());
        QSqlQuery update(database);
        QVERIFY2(update.exec(QStringLiteral(
                     "UPDATE sources SET playable=0,unplayable_reason='forced-test'")),
                 qPrintable(update.lastError().text()));
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);

    QSignalSpy catalogueChanges(&controller, &LibraryController::catalogueChanged);
    QVERIFY(QMetaObject::invokeMethod(
        &controller, "onFailed", Qt::DirectConnection,
        Q_ARG(QString, QStringLiteral("Synthetic scanner failure"))));
    QCOMPARE(catalogueChanges.count(), 1);
    QCOMPARE(view.songResultCount(), 0);
    QCOMPARE(view.resultsList()->model()->rowCount(), 0);
}

void TestLibraryView::browseAndModelScale()
{
    constexpr int SongCount = 50000;
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("music"));
    const QString databasePath = temporary.filePath(QStringLiteral("app/library.sqlite"));
    QVERIFY(QDir().mkpath(root));

    qint64 rootId = 0;
    {
        Catalogue schema(databasePath);
        QString error;
        QVERIFY2(schema.open(&error), qPrintable(error));
        QVERIFY2(schema.addRoot(root, &rootId, &error), qPrintable(error));
        QVERIFY2(schema.setActiveRoot(rootId, &error), qPrintable(error));
    }

    const QString connection = QStringLiteral("library-scale-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                          connection);
        database.setDatabaseName(databasePath);
        QVERIFY(database.open());
        QVERIFY(database.transaction());
        QSqlQuery song(database);
        QSqlQuery file(database);
        QSqlQuery source(database);
        song.prepare(QStringLiteral(
            "INSERT INTO songs(id,display_title,display_artist,disc_id,track,search_text,"
            "confidence,playable) VALUES(?,?,?,?,?,?,?,1)"));
        file.prepare(QStringLiteral(
            "INSERT INTO files(id,root_id,rel_path,rel_dir,file_name,ext,kind,size,mtime_ms,"
            "present) VALUES(?,?,?,?,?,?,?,?,?,1)"));
        source.prepare(QStringLiteral(
            "INSERT INTO sources(id,song_id,root_id,kind,mp3_file_id,playable,parsed_json) "
            "VALUES(?,?,?,'loose_cdg',?,1,'{}')"));
        for (int i = 1; i <= SongCount; ++i) {
            const QString number = QString::number(i).rightJustified(5, QLatin1Char('0'));
            song.bindValue(0, i);
            song.bindValue(1, QStringLiteral("Scale Title %1").arg(number));
            song.bindValue(2, QStringLiteral("Scale Artist %1").arg(i % 100));
            song.bindValue(3, QStringLiteral("SC%1").arg(i % 1000));
            song.bindValue(4, i % 100);
            song.bindValue(5, QStringLiteral("scale artist title %1").arg(number));
            song.bindValue(6, QStringLiteral("high"));
            QVERIFY2(song.exec(), qPrintable(song.lastError().text()));

            const QString fileName = QStringLiteral("song-%1.mp3").arg(number);
            file.bindValue(0, i);
            file.bindValue(1, rootId);
            file.bindValue(2, fileName);
            file.bindValue(3, QStringLiteral(""));
            file.bindValue(4, fileName);
            file.bindValue(5, QStringLiteral("mp3"));
            file.bindValue(6, QStringLiteral("audio"));
            file.bindValue(7, 1);
            file.bindValue(8, 1);
            QVERIFY2(file.exec(), qPrintable(file.lastError().text()));

            source.bindValue(0, i);
            source.bindValue(1, i);
            source.bindValue(2, rootId);
            source.bindValue(3, i);
            QVERIFY2(source.exec(), qPrintable(source.lastError().text()));
        }
        QVERIFY(database.commit());
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);

    Catalogue catalogue(databasePath);
    QString error;
    QVERIFY2(catalogue.open(&error, {root}), qPrintable(error));
    QElapsedTimer browseTimer;
    browseTimer.start();
    const QList<CatalogueSearchRow> rows = catalogue.browseActive(&error);
    const qint64 browseMs = browseTimer.elapsed();
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(rows.size(), SongCount);
    QVERIFY2(browseMs < 3000,
             qPrintable(QStringLiteral("browseActive took %1 ms").arg(browseMs)));

    LibraryResultsModel model;
    QListView list;
    list.setUniformItemSizes(true);
    list.setModel(&model);
    QElapsedTimer modelTimer;
    modelTimer.start();
    model.setRows(rows);
    const qint64 modelMs = modelTimer.elapsed();
    QCOMPARE(model.rowCount(), SongCount);
    QVERIFY2(modelMs < 2000,
             qPrintable(QStringLiteral("Model population took %1 ms").arg(modelMs)));
    qInfo().noquote() << QStringLiteral(
        "SCALE_TIMING browseActive_rows=%1 browse_ms=%2 model_ms=%3")
        .arg(rows.size()).arg(browseMs).arg(modelMs);
    catalogue.close();

    LibraryController controller(databasePath);
    QVERIFY(controller.isAvailable());
    LibraryView view(&controller);
    view.resize(1000, 700);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    view.searchBox()->setText(QStringLiteral("title 25000"));
    view.refreshSearch();
    QCOMPARE(view.songResultCount(), 1);
    view.resultsList()->setCurrentIndex(view.resultsList()->model()->index(0, 0));
    const qint64 keepSongId = view.selectedSongId();
    QVERIFY(keepSongId != 0);

    QVERIFY(QMetaObject::invokeMethod(
        &controller, "onFinished", Qt::DirectConnection,
        Q_ARG(QVariantMap, QVariantMap())));
    QCOMPARE(view.songResultCount(), 1);
    QCOMPARE(view.selectedSongId(), keepSongId);

    QElapsedTimer integratedTimer;
    integratedTimer.start();
    view.searchBox()->clear();
    while (view.songResultCount() != SongCount && integratedTimer.elapsed() < 2000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
    const qint64 integratedMs = integratedTimer.elapsed();
    QCOMPARE(view.songResultCount(), SongCount);
    QCOMPARE(view.selectedSongId(), keepSongId);
    QVERIFY2(integratedMs < 2000,
             qPrintable(QStringLiteral(
                 "Integrated cold-cache browse took %1 ms").arg(integratedMs)));
    qInfo().noquote() << QStringLiteral(
        "SCALE_TIMING integrated_cold_cache_rows=%1 integrated_ms=%2 selected_song_id=%3")
        .arg(view.songResultCount()).arg(integratedMs).arg(keepSongId);
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
    view->resultsList()->setCurrentIndex(view->resultsList()->model()->index(0, 0));
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

void TestLibraryView::protectedStorageInsideChosenRootIsRefused()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("music"));
    QVERIFY(QDir().mkpath(root));
    LibraryController controller(temporary.filePath(QStringLiteral("app/library.sqlite")));
    controller.setProtectedStoragePaths({
        QDir(root).filePath(QStringLiteral("app/playlists.sqlite"))});
    QSignalSpy scans(&controller, &LibraryController::scanRequested);
    QString error;
    QVERIFY(!controller.chooseRoot(root, &error));
    QVERIFY(error.contains(QStringLiteral("inside library root")));
    QCOMPARE(scans.count(), 0);
    QVERIFY(!controller.hasActiveRoot());
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
