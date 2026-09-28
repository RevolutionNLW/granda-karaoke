#pragma once

#include <QString>
#include <QStringList>

// An installation check, run as
//   FrankiesKaraokeStudio --self-check <report.txt> [<song.mp3>]
// It opens no library, playlist or settings database and writes only the
// report and GStreamer's plugin list in the program's own data folder.
// It checks that the SQLite driver, the data folder and every audio
// component are present and, in a packaged copy, that the audio components
// come from the package itself. Given a song (MP3 with its CDG beside it) it
// plays it silently with a changed Key and Tempo.
namespace selfcheck {

// Returns the process exit code: 0 when every required part works, 2 when
// the report cannot be written (it is never written inside a music folder
// or over the test song).
int run(const QString& reportPath, const QString& songPath, const QStringList& musicFolders);

} // namespace selfcheck
