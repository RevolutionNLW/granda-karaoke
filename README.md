# granda-karaoke

**Frankie's Karaoke Studio**: a simple MP3+CDG karaoke player (C++20, Qt 6, GStreamer).

## Behaviour (Milestone 2)

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
- **Key** changes the singing key from -6 to +6 while the song keeps playing. **Tempo**
  changes the speed from 70% to 130% without changing the key. Both have large -/+
  buttons and a Reset button. Changes also work while paused and remain in effect when
  the same song is restarted.
- Key and Tempo are remembered automatically for each song. There is no Save button.
  Settings are restored before Play, so the first note uses the remembered values.
  Songs are identified by a SHA-256 fingerprint of the complete CDG and the MP3 audio
  payload (excluding recognised ID3 and APE metadata), not by their file name or path:
  moving, renaming, or retagging a song keeps its settings, while different lyrics do not
  share them.
- Buttons never react to the keyboard, so Space or Enter can't press one by accident.
  The display is kept awake while a song plays.
- Each failure shows one plain-English message. Details go to the log.

Song settings are stored as `song-settings.json` in the platform application-data
folder. On macOS this is normally
`~/Library/Application Support/Granda/FrankiesKaraokeStudio/`. Writes are atomic. If
multiple app instances are open, each write is locked and merged with the current file.
If the file is damaged or belongs to an unsupported format version, it is preserved beside
the new file with a `.corrupt-<timestamp>` suffix.

## Layout

| Path | Purpose |
| --- | --- |
| `src/cdg/CdgDecoder.*` | CD+G decoding (plain C++, no Qt/GStreamer) |
| `src/KaraokePlayer.*` | GStreamer audio playback; drives the CDG decoder from the audio position |
| `src/SongPair.*` | Finds the matching `.mp3`/`.cdg` companion and checks both are readable |
| `src/SongSettings.*` | Content identity and atomic per-song Key/Tempo JSON storage |
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
required playback and Key/Tempo plugins, including SoundTouch `pitch` and audiofx
`scaletempo`.

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=$HOME/Qt/6.11.2/macos
cmake --build build -j8
ctest --test-dir build --output-on-failure
open build/FrankiesKaraokeStudio.app
```

CTest optionally reads a real pair from `~/Music/Frankies-Karaoke-Studio`
(`/Users/joshdeery/Music/Frankies-Karaoke-Studio` on this Mac). The test skips cleanly
when that directory is absent. Media is read in place and never copied into the
repository. Override the CTest path at configuration time:

```bash
cmake -S . -B build -DFKS_TEST_MEDIA_DIR=/path/to/folder/with/pair
```

For a direct run, override the environment variable:

```bash
FKS_TEST_MEDIA_DIR="$HOME/Music/Frankies-Karaoke-Studio" build/tests/tst_karaokeplayer externalMediaSmokeTest
```

Logs: `~/Library/Application Support/Granda/FrankiesKaraokeStudio/frankies-karaoke-studio.log`
