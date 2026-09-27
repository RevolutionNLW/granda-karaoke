#include "KaraokePlayer.h"
#include "LibraryController.h"
#include "Logging.h"
#include "MainWindow.h"
#include "SongSettings.h"
#include "library/KnownLibraryRoots.h"
#include "playlist/PlaylistStore.h"

#include <QApplication>
#include <QDir>
#include <QMessageBox>
#include <QStandardPaths>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("Granda"));
    QApplication::setApplicationName(QStringLiteral("FrankiesKaraokeStudio"));
    QApplication::setApplicationDisplayName(QStringLiteral("Frankie's Karaoke Studio"));
    QApplication::setApplicationVersion(QStringLiteral(FKS_VERSION));

    const QString logFile = logging::install();
    qCInfo(lcApp) << "Frankie's Karaoke Studio" << FKS_VERSION << "starting; Qt" << qVersion()
                  << "log file:" << (logFile.isEmpty() ? QStringLiteral("(none)") : logFile);

    QString error;
    if (!KaraokePlayer::initializeGStreamer(&error)) {
        QMessageBox::critical(nullptr, QStringLiteral("Frankie's Karaoke Studio"),
                              error + QStringLiteral("\n\nThe program will now close."));
        return 1;
    }

    KaraokePlayer player;
    const QString appDataPath = QStandardPaths::writableLocation(
        QStandardPaths::AppDataLocation);
    QDir().mkpath(appDataPath);
    const QString settingsPath = QDir(appDataPath).filePath(
        QStringLiteral("song-settings.json"));
    SongSettingsStore settingsStore(settingsPath);
    const QString cataloguePath = QDir(QStandardPaths::writableLocation(
        QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("library.sqlite"));
    const QString playlistsPath = QDir(appDataPath).filePath(
        QStringLiteral("playlists.sqlite"));
    const QString overridesPath = QDir(appDataPath).filePath(
        QStringLiteral("metadata-overrides.sqlite"));
    // Play history and preferences: user state that outlives the catalogue.
    const QString userStatePath = QDir(appDataPath).filePath(
        QStringLiteral("user-state.sqlite"));
    // Library roots are also remembered in app-owned settings, outside the
    // catalogue: every database path is checked against them before SQLite
    // opens it, so no database inside a music folder is ever opened for writing.
    LibraryController libraryController(
        cataloguePath, {}, overridesPath, nullptr, KnownLibraryRoots::load(), userStatePath);
    auto rememberRoots = [&libraryController] {
        KnownLibraryRoots::remember(libraryController.libraryRoots());
    };
    rememberRoots();
    QObject::connect(&libraryController, &LibraryController::catalogueChanged,
                     &libraryController, rememberRoots);
    QObject::connect(&libraryController, &LibraryController::libraryReady,
                     &libraryController, rememberRoots);
    libraryController.setProtectedStoragePaths(
        {playlistsPath, overridesPath, settingsPath, userStatePath});
    PlaylistStore playlistStore(playlistsPath);
    if (!playlistStore.open(&error, libraryController.libraryRoots()))
        qCWarning(lcApp).noquote() << "Playlists are unavailable:" << error;
    MainWindow window(&player, &settingsStore, &libraryController, &playlistStore);
    window.resize(900, 520);
    window.showFullScreen();
    window.setFocus(Qt::OtherFocusReason);
    libraryController.startConfiguredScan();

    // Optional: a song path on the command line is opened at start-up.
    const QStringList args = QApplication::arguments();
    if (args.size() > 1 && !args.at(1).startsWith(QLatin1Char('-')))
        window.openSong(args.at(1));

    const int result = app.exec();
    qCInfo(lcApp) << "Exiting with code" << result;
    return result;
}
