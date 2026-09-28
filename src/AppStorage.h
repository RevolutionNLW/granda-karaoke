#pragma once

#include <QString>

// The one folder the program writes to: its databases, Key/Tempo memory,
// caches, log and audio-plugin registry. It belongs to the program and the
// user (on Windows %LOCALAPPDATA%\Granda\FrankiesKaraokeStudio, on macOS
// ~/Library/Application Support/Granda/FrankiesKaraokeStudio), and is never
// beside the program itself or inside the music.
namespace appstorage {

// Needs the organisation and application names set first. Empty only if the
// system gives no such location.
QString folder();

} // namespace appstorage
