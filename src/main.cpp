#include "AppPreferences.h"
#include "AppStorage.h"
#include "BackgroundWork.h"
#include "KaraokePlayer.h"
#include "LibraryController.h"
#include "Logging.h"
#include "MainWindow.h"
#include "SelfCheck.h"
#include "Shutdown.h"
#include "SongSettings.h"
#include "library/Catalogue.h"
#include "library/KnownLibraryRoots.h"
#include "playlist/PlaylistStore.h"
#include "ui/Controls.h"
#include "ui/Splash.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLockFile>
#include <QMessageBox>
#include <QScreen>
#include <QSysInfo>

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

    // Everything the program writes lives in its own folder (see AppStorage).
    // Nothing is written anywhere until it is certain that folder is not
    // inside a music folder (only a system that moved it there could do that).
    const QString appDataPath = appstorage::folder();
    for (const QString& root : KnownLibraryRoots::load()) {
        if (!appDataPath.isEmpty() && Catalogue::pathIsInsideOrEqual(appDataPath, root)) {
            QMessageBox::critical(
                nullptr, QStringLiteral("Frankie's Karaoke Studio"),
                QStringLiteral("Frankie's Karaoke Studio cannot start: its own data folder\n\n%1\n\n"
                               "is inside the music folder\n\n%2\n\nNothing has been changed.")
                    .arg(QDir::toNativeSeparators(appDataPath), QDir::toNativeSeparators(root)));
            return 1;
        }
    }

    // Installation check: FrankiesKaraokeStudio --self-check <report> [<song.mp3>]
    const QStringList launchArguments = QApplication::arguments();
    if (const qsizetype check = launchArguments.indexOf(QStringLiteral("--self-check")); check >= 0)
        return selfcheck::run(launchArguments.value(check + 1), launchArguments.value(check + 2));

    QDir().mkpath(appDataPath);

    // One copy at a time. Windows does not stop a second copy when the icon
    // is opened twice, and two copies would share the same databases and log.
    // The lock is held by this process only; a crashed copy's lock is taken
    // over (its process no longer exists), never after a time limit.
    QLockFile instanceLock(QDir(appDataPath).filePath(QStringLiteral("running.lock")));
    instanceLock.setStaleLockTime(0);
    if (!appDataPath.isEmpty() && !instanceLock.tryLock(0)
        && instanceLock.error() == QLockFile::LockFailedError) {
        QMessageBox::information(nullptr, QStringLiteral("Frankie's Karaoke Studio"),
                                 QStringLiteral("Frankie's Karaoke Studio is already open."));
        return 0;
    }

    const QString logFile = logging::install();
    qCInfo(lcApp) << "Frankie's Karaoke Studio" << FKS_VERSION << "starting; Qt" << qVersion()
                  << "log file:" << (logFile.isEmpty() ? QStringLiteral("(none)") : logFile);
    qCInfo(lcApp).noquote() << "System:" << QSysInfo::prettyProductName()
                            << "kernel" << QSysInfo::kernelVersion()
                            << QSysInfo::currentCpuArchitecture()
                            << "| platform" << QGuiApplication::platformName();
    qCInfo(lcApp).noquote() << "Program folder:" << QCoreApplication::applicationDirPath();
    qCInfo(lcApp).noquote() << "Program data folder:" << appDataPath;
    if (const QScreen* primary = QGuiApplication::primaryScreen()) {
        qCInfo(lcApp) << "Primary screen" << primary->size() << "available" << primary->availableGeometry()
                      << "device pixel ratio" << primary->devicePixelRatio()
                      << "logical DPI" << primary->logicalDotsPerInch()
                      << "screens" << QGuiApplication::screens().size();
    }

    QString error;
    if (!KaraokePlayer::initializeGStreamer(&error)) {
        QMessageBox::critical(nullptr, QStringLiteral("Frankie's Karaoke Studio"),
                              error + QStringLiteral("\n\nThe program will now close."));
        return 1;
    }

    int result = 0;
    {
        KaraokePlayer player;
        const QString settingsPath = QDir(appDataPath).filePath(
            QStringLiteral("song-settings.json"));
        SongSettingsStore settingsStore(settingsPath);
        const QString cataloguePath = QDir(appDataPath).filePath(QStringLiteral("library.sqlite"));
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
        {
            const CatalogueRoot root = libraryController.activeRoot();
            qCInfo(lcApp).noquote() << "Catalogue:" << cataloguePath
                                    << (libraryController.isAvailable() ? QString() : QStringLiteral("(unavailable)"));
            qCInfo(lcApp).noquote() << "Music folder:"
                                    << (root.path.isEmpty() ? QStringLiteral("(none chosen)") : root.path)
                                    << (root.path.isEmpty() ? QString()
                                        : libraryController.isRootConnected() ? QStringLiteral("(connected)")
                                                                              : QStringLiteral("(not connected)"));
            const QString output = settings.text(pref::AudioOutput);
            qCInfo(lcApp).noquote() << "Sound output:"
                                    << (output.isEmpty() ? QStringLiteral("system default") : output)
                                    << "| volume" << settings.number(pref::Volume, 100) << "%"
                                    << "| UI scale" << theme::scalePercent() << "%";
        }
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
        // For automated checks: --music-folder <folder> chooses the music
        // folder exactly as Settings does, with the same safety checks.
        if (const qsizetype option = launchArguments.indexOf(QStringLiteral("--music-folder")); option >= 0) {
            QString refused;
            if (!libraryController.chooseRoot(launchArguments.value(option + 1), &refused))
                qCWarning(lcApp).noquote() << "Music folder refused:" << refused;
        }
        libraryController.startConfiguredScan();

        // Optional: a song path on the command line is opened at start-up.
        if (launchArguments.size() > 1 && !launchArguments.at(1).startsWith(QLatin1Char('-')))
            window.openSong(launchArguments.at(1));

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
