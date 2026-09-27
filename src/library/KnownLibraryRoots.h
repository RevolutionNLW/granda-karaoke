#pragma once

#include <QStringList>

// Library roots remembered in the application's own settings, outside every
// catalogue. Any database path is checked against them before SQLite opens it,
// so nothing a database contains (or hides in a journal) can decide whether a
// music folder is written to.
namespace KnownLibraryRoots {

QStringList load();
// Adds roots to the remembered list; a root is never forgotten.
void remember(const QStringList& roots);

} // namespace KnownLibraryRoots
