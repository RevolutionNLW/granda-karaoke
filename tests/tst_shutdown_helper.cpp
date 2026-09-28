// Runs the program's closing sequence in a process of its own, for
// tst_shutdown: "busy" closes while the library scanner is inside the
// resolver; "stuck" closes while the scanner is stuck where it cannot notice
// a stop, which must end the process at once without destroying anything.

#include "LibraryController.h"
#include "Shutdown.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QThread>

#include <atomic>
#include <cstdio>

namespace {

bool waitUntil(const std::function<bool()>& condition, int timeoutMs)
{
    QDeadlineTimer deadline(timeoutMs);
    while (!condition()) {
        if (deadline.hasExpired())
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(5);
    }
    return true;
}

// The resolver's first step marks the reprocess pending (tst_shutdown's
// fixture starts with it off).
bool resolverStarted(const QString& path)
{
    QString value;
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("watch"));
        database.setDatabaseName(path);
        if (database.open()) {
            QSqlQuery query(database);
            if (query.exec(QStringLiteral("SELECT value FROM catalogue_meta WHERE key='reprocess_pending'"))
                && query.next())
                value = query.value(0).toString();
        }
    }
    QSqlDatabase::removeDatabase(QStringLiteral("watch"));
    return value == QLatin1String("1");
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    const QString mode = args.value(1);
    if (mode == QLatin1String("busy") && args.size() == 3) {
        LibraryController library(args.at(2));
        if (!waitUntil([&args] { return resolverStarted(args.at(2)); }, 20000))
            return 3;
        QThread::msleep(150);  // deep in its in-memory stages, which take seconds
        QElapsedTimer timer;
        timer.start();
        shutdown::stopLibraryOrExit(library, 42);
        std::printf("STOPPED in %lld ms, running=%d\n", timer.elapsed(), int(library.scannerRunning()));
        std::fflush(stdout);
        return library.scannerRunning() ? 4 : 0;
    }
    if (mode == QLatin1String("stuck") && args.size() == 4) {
        LibraryController library(args.at(2), {}, {}, nullptr, {}, args.at(3));
        if (!library.isAvailable() || !library.setPreference(QStringLiteral("test.durable"), QStringLiteral("saved")))
            return 3;
        std::atomic_bool started = false;
        library.runOnScannerThreadForTesting([&started] {
            started = true;
            for (;;)  // stuck, deaf to any stop request
                QThread::msleep(20);
        });
        if (!waitUntil([&started] { return started.load(); }, 5000))
            return 3;
        std::printf("STUCK\n");
        std::fflush(stdout);
        shutdown::stopLibraryOrExit(library, 42, 400);
        std::printf("RETURNED\n");  // must never happen
        std::fflush(stdout);
        return 1;
    }
    return 2;
}
