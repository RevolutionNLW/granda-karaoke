#pragma once

class LibraryController;

// The end of the program, in the order that keeps every thread away from
// objects that are being destroyed.
namespace shutdown {

// How long closing waits for the library scanner, which normally stops within
// milliseconds of being asked.
inline constexpr int kLibraryStopMs = 5000;

// Call once the event loop has ended, before any application object is
// destroyed. Returns once the library scanner has stopped and its thread has
// ended. If it is stuck (e.g. in a read from a music drive that stopped
// answering), the process ends here with exitCode instead, so nothing the
// scanner may still be using is ever destroyed under it.
void stopLibraryOrExit(LibraryController& library, int exitCode,
                       int timeoutMs = kLibraryStopMs);

} // namespace shutdown
