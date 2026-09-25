# granda-karaoke

**Frankie's Karaoke Studio**: a simple MP3+CDG karaoke player (C++20, Qt 6, GStreamer).

## Behaviour (Milestone 1, accepted)

- **Open Song** picks an `.mp3` or `.cdg`. Its companion file is found in the same folder.
  A song that can't be opened is reported without disturbing the song already playing.
- **Play** starts the audio, and the lyrics fill the screen. The app is one window that
  goes fullscreen once at startup. Lyrics and controls are pages of that window, so
  switching between them is instant.
- **Escape** (or a click on the lyrics) shows the controls. The music keeps playing.
- **Enter/Return** shows the lyrics again while playing or paused. Playback is unaffected.
- **Pause/Resume** and **Stop** keep the lyrics in sync. The GStreamer audio position is
  the only clock, so the lyrics never run ahead of the music. At the end of a song the
  controls return.
- Buttons never react to the keyboard, so Space or Enter can't press one by accident.
  The display is kept awake while a song plays.
- Each failure shows one plain-English message. Details go to the log.

## Layout

| Path | Purpose |
| --- | --- |
| `src/cdg/CdgDecoder.*` | CD+G decoding (plain C++, no Qt/GStreamer) |
| `src/KaraokePlayer.*` | GStreamer audio playback; drives the CDG decoder from the audio position |
| `src/SongPair.*` | Finds the matching `.mp3`/`.cdg` companion and checks both are readable |
| `src/MainWindow.*` | Single fullscreen window with controls (including Exit) and lyrics pages |
| `src/LyricsView.*` | Child lyrics page with aspect-correct nearest-neighbour painting |
| `src/platform/DisplaySleepBlocker.*` | Prevents idle display sleep while Playing (macOS/Windows) |
| `src/Logging.*` | Log categories; log file in the user's application data folder |
| `cmake/GStreamer.cmake` | All GStreamer SDK discovery (override with `-DGSTREAMER_ROOT=...`) |
| `tests/` | Qt Test suites using synthetic media |

## Build (macOS development)

Requires Qt 6, CMake 3.22 or newer and the official GStreamer SDK (found automatically in
`~/Library/Frameworks` or `/Library/Frameworks`). Configuration fails on purpose if only a
package-manager GStreamer (for example Homebrew) is available, or if the SDK lacks the
SoundTouch `pitch` plugin needed for key changes later.

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=$HOME/Qt/6.11.2/macos
cmake --build build -j8
ctest --test-dir build --output-on-failure
open build/FrankiesKaraokeStudio.app
```

CTest optionally reads a real pair from `~/Frankies-Karaoke-Studio`
(`/Users/joshdeery/Frankies-Karaoke-Studio` on this Mac). The test skips cleanly
when that directory is absent. Media is read in place and never copied into the
repository. Override the CTest path at configuration time:

```bash
cmake -S . -B build -DFKS_TEST_MEDIA_DIR=/path/to/folder/with/pair
```

For a direct run, override the environment variable:

```bash
FKS_TEST_MEDIA_DIR="$HOME/Frankies-Karaoke-Studio" build/tests/tst_karaokeplayer externalMediaSmokeTest
```

Logs: `~/Library/Application Support/Granda/FrankiesKaraokeStudio/frankies-karaoke-studio.log`
