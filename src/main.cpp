#include "AppPreferences.h"
#include "BackgroundWork.h"
#include "KaraokePlayer.h"
#include "LibraryController.h"
#include "Logging.h"
#include "MainWindow.h"
#include "Shutdown.h"
#include "SongSettings.h"
#include "library/KnownLibraryRoots.h"
#include "playlist/PlaylistStore.h"
#include "ui/Controls.h"
#include "ui/Splash.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QMessageBox>
#include <QScreen>
#include <QStandardPaths>

#include <cstdlib>

int main(int argc, char* argv[])
{
    QElapsedTimer launch;
    launch.start();
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("Granda"));
    QApplication::setApplicationName(QStringLiteral("FrankiesKaraokeStudio"));
    QApplication::setApplicationDisplayName(QStringLiteral("Frankie's Karaoke Studio"));
    QApplication::setApplicationVersion(QStringLiteral(FKS_VERSION));
    theme::apply(app);
    QApplication::setWindowIcon(ui::glyphIcon(ui::Glyph::App, theme::color::accent));

    const QString logFile = logging::install();
    qCInfo(lcApp) << "Frankie's Karaoke Studio" << FKS_VERSION << "starting; Qt" << qVersion()
                  << "log file:" << (logFile.isEmpty() ? QStringLiteral("(none)") : logFile);

    QString error;
    if (!KaraokePlayer::initializeGStreamer(&error)) {
        QMessageBox::critical(nullptr, QStringLiteral("Frankie's Karaoke Studio"),
                              error + QStringLiteral("\n\nThe program will now close."));
        return 1;
    }

    int result = 0;
    {
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

        // Settings live beside play history, outside the rebuildable catalogue.
        AppPreferences preferences;
        AppPreferences storedPreferences(
            [&libraryController](const QString& key) { return libraryController.preference(key); },
            [&libraryController](const QString& key, const QString& value) {
                return libraryController.setPreference(key, value);
            });
        storedPreferences.setBatchWriter([&libraryController](const QList<QPair<QString, QString>>& values) {
            return libraryController.setPreferences(values);
        });
        AppPreferences& settings = libraryController.hasPreferences() ? storedPreferences : preferences;
        theme::setScalePercent(settings.number(pref::ScalePercent, 100));
        theme::setCompactRows(settings.flag(pref::CompactRows, false));
        theme::setAlternateRows(settings.flag(pref::AlternateRows, true));
        applyStartupChoices(settings, playlistStore);

        MainWindow window(&player, &settingsStore, &libraryController, &playlistStore, &settings);
        window.setDataLocations({
            {QStringLiteral("Program data folder"), appDataPath},
            {QStringLiteral("Song catalogue (library.sqlite, rebuilt from the music folder)"), cataloguePath},
            {QStringLiteral("Settings and play history (user-state.sqlite)"), userStatePath},
            {QStringLiteral("Playlists (playlists.sqlite)"), playlistsPath},
            {QStringLiteral("Song-name corrections (metadata-overrides.sqlite)"), overridesPath},
            {QStringLiteral("Title-screen cache (enrichment-cache.sqlite)"),
             QFileInfo(cataloguePath).absoluteDir().filePath(QStringLiteral("enrichment-cache.sqlite"))},
            {QStringLiteral("Key and Tempo memory (song-settings.json)"), settingsPath},
            {QStringLiteral("Log file"), logFile},
        });
        // The splash covers the finished window as it first appears, then
        // dissolves into it; the window works underneath all the while.
        if (settings.flag(pref::ShowSplash, true))
            new ui::SplashOverlay(&window, launch.elapsed());
        const QRect screen = window.screen() ? window.screen()->availableGeometry() : QRect();
        window.resize(QSize(theme::px(1280), theme::px(800))
                          .boundedTo(screen.isValid() ? screen.size() : QSize(4000, 4000)));
        const QByteArray geometry = QByteArray::fromBase64(settings.text(pref::WindowGeometry).toLatin1());
        if (settings.flag(pref::StartFullscreen, true)) {
            window.showFullScreen();
        } else {
            if (settings.flag(pref::RememberWindow, true) && !geometry.isEmpty())
                window.restoreGeometry(geometry);
            window.setWindowState(window.windowState() & ~Qt::WindowFullScreen);
            window.show();
        }
        qCInfo(lcApp) << "Main window shown" << launch.elapsed() << "ms after launch";
        libraryController.startConfiguredScan();

        // Optional: a song path on the command line is opened at start-up.
        const QStringList args = QApplication::arguments();
        if (args.size() > 1 && !args.at(1).startsWith(QLatin1Char('-')))
            window.openSong(args.at(1));

        result = app.exec();
        // The library scanner is stopped and its thread ended before anything
        // it could still be using is destroyed below.
        shutdown::stopLibraryOrExit(libraryController, result);
    }  // everything is saved and closed here
    qCInfo(lcApp) << "Exiting with code" << result;
    // Qt must not shut down under a background job still running. One stuck
    // in the system is not waited for forever: the program then ends at once,
    // with everything already saved.
    if (!background::waitForAll(3000)) {
        qCWarning(lcApp) << "Background work still running at exit; closing without waiting";
        std::_Exit(result);
    }
    return result;
}
