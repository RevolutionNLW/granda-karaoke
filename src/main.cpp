#include "KaraokePlayer.h"
#include "Logging.h"
#include "MainWindow.h"

#include <QApplication>
#include <QMessageBox>

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
    MainWindow window(&player);
    window.resize(900, 520);
    window.showFullScreen();
    window.setFocus(Qt::OtherFocusReason);

    // Optional: a song path on the command line is opened at start-up.
    const QStringList args = QApplication::arguments();
    if (args.size() > 1 && !args.at(1).startsWith(QLatin1Char('-')))
        window.openSong(args.at(1));

    const int result = app.exec();
    qCInfo(lcApp) << "Exiting with code" << result;
    return result;
}
