#include "Shutdown.h"

#include "LibraryController.h"
#include "Logging.h"

#include <cstdlib>

namespace shutdown {

void stopLibraryOrExit(LibraryController& library, int exitCode, int timeoutMs)
{
    if (library.stopScanner(timeoutMs))
        return;
    // Everything the user owns is already saved: settings, play history,
    // playlists, name corrections and Key/Tempo are written as they change,
    // and the log line by line. Only the scanner's unfinished batch in the
    // rebuildable catalogue is left; SQLite discards it on the next start and
    // the next scan does the work again.
    qCCritical(lcApp) << "Library scanner did not stop within" << timeoutMs
                      << "ms; closing without waiting for it";
    std::_Exit(exitCode);
}

} // namespace shutdown
