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

## Song Library (Milestone 3)

**Find a Song** opens a large-text library screen. Choose the folder containing the
karaoke collection once, then search by any part of a title or singer name. Searches
show only playable MP3+CDG pairs from the active music folder, never start playback
automatically, and keep working from the saved catalogue when the external music drive
is disconnected. The scanner runs in the background and pauses disk access while a
song is playing or paused.

The catalogue is `library.sqlite` in the platform local application-data folder (on
macOS, normally `~/Library/Application Support/Granda/FrankiesKaraokeStudio/`). The
`fks-catalogue` command-line tool can scan and inspect the same catalogue; run it without
arguments for its command list. ZIP and MCG songs are catalogued, but ZIP-only songs and
MCG songs are not playable yet. Playlists, queues and autoplay are not included.

## Song Names (Milestone 5)

The collection's file names and tags are inconsistent, so the app keeps its own clean
interpretation of every song and never changes a song file. Each song has three layers:

1. **Raw data** exactly as found: path, file name, folders, ID3 tags, ZIP members.
2. **Automatic metadata** with a confidence (`high`, `medium`, `low`, `unresolved`),
   where it came from, and the evidence behind it. The sources are the file name and
   the folder/disc naming rules, ID3 tags (supporting only), disc track lists found
   beside the songs (for example MP3+G Toolz "Folder Text Files"), identical copies
   elsewhere in the collection (proven by complete CDG content, never by size), and
   CDG title screens read with local text recognition.
3. **Manual corrections**, which always win and are never overwritten by reprocessing.

Each song also records its karaoke **label** and, where certain, **series** (for example
Sunfly / Most Wanted, Legends, Zoom), separately from artist, title, disc and track, so
versions of the same song from different labels stay distinct (`Johnny Cash | Ring Of Fire |
Sunfly | SF123-04` and `... | Legends | LEG056-09`). A label comes from a disc code that
belongs to one label, or for songs without a disc code from a top-level folder named after
a label; when the two disagree, or the code is unknown, no label is given. Songs are never
merged because their artist and title match.

Unresolved songs stay playable and are shown as, for example, `Disc SGB39 - Track 02`.
Search finds a song by its shown name and by its raw file name, folders, tags, disc and
track (`3902`, `SGB39 02`), so old identifiers keep working.

**Library maintenance** (for whoever looks after the collection, not the singer) opens with
**Ctrl+Shift+M** (**Cmd+Shift+M** on macOS). It lists unresolved, low- or medium-confidence,
conflicting and manually corrected songs, shows the evidence for each, lets you correct
artist and title, and runs **Reprocess Metadata** in the background. Reprocessing only
reads the catalogue, except for two slow steps that read the music drive read-only,
resume where they stopped and pause while a song plays: comparing weakly named songs with
same-size named copies, and (when ticked, macOS only for now) reading title screens.
Title screens usually show the song title but not the singer, so a title is shown only
when it matches a title already known in the collection; other readings stay searchable,
and a single unverified reading is offered as **Detected title** (Use Detected Title copies
it into the Title box; nothing is saved until Save Correction). **Play Preview** plays the
selected song through the normal player, with its lyrics in the review window: a preview
never starts playlist context or Autoplay, never counts as a play and never stores Key/Tempo.
**Revert to Automatic** removes a correction.

Title-screen results are stored by CDG content and can be moved to another computer
(for example the Windows PC, which has no text recognition in this version):

```bash
build/fks-catalogue --db <mac library.sqlite> title-screens-export ~/title-screens.json
fks-catalogue --db <windows library.sqlite> title-screens-import title-screens.json
```

**Trusted metadata.** A correction can set artist, title, label, series, disc and track;
any value left unset stays automatic. The same store records metadata supplied when a song
is added (origin `import`, for a future Add Song workflow: validate the MP3/CDG pair, save
the clean metadata by file, then scan). Reprocessing and resolver upgrades never override a
trusted value, and filename rules never reinterpret it.

**One-time enrichment.** The legacy collection is treated as static. Track lists, duplicate
matching and title-screen text are worked out once and cached by file size/mtime or content,
so routine rescans re-read nothing that has not changed, and title screens are only read
when explicitly requested. Reprocessing reuses those results. Raw title-screen readings are
kept in `enrichment-cache.sqlite` beside the catalogue, so a rebuilt catalogue reconnects to
them by CDG content (during an ordinary scan, without any text recognition).

**Library order and play history.** The library's **Sort** control (Artist, Song, Most
Played, Recently Played, Label) applies to browsing and search results. Plays are counted
once, when a song's audio actually starts (library, playlist, Autoplay, or a song file that
is in the catalogue; never a preview or a resume). Play history is kept per karaoke version
by content identity, like Key/Tempo, in `user-state.sqlite` together with the chosen sort,
so it survives rebuilding the catalogue and moving or renaming song files.

Corrections, playlists and remembered Key/Tempo are not affected by name changes: songs
are identified by file path (playlists) and by content (Key/Tempo). Corrections are
stored in `metadata-overrides.sqlite` beside `playlists.sqlite`, so deleting or rebuilding
`library.sqlite` loses nothing you entered. No database is ever opened inside a music
folder: the music folders are also remembered in the application's settings, and every
database path is checked against them before SQLite opens it.

The first start after upgrading makes a consistent `library.sqlite.pre-v5-<timestamp>.bak`
beside the catalogue and then updates the song names in the background. Milestone 4 builds
refuse a v5 catalogue as newer than supported; restore that backup to go back.

`fks-catalogue --db <path> metadata-stats` reports confidence and source counts, and
`compare --baseline <other.sqlite>` compares two catalogues song by song.

## Song Keys

The library can show each song's **musical key** (C, F#m, Bb ...) in a **Key** column,
worked out from the music itself (never from file names or tags). This is the key of the
karaoke track as recorded, which may differ from the original hit record. It is not the
player bar's Key control, which shifts the song by semitones: the Now Playing line shows
the song's key and the key heard, e.g. `Key C → D` at +2, with "Original key: C /
Current key: D (+2)" in its tooltip, and changes as Key is pressed. The Key column stays
hidden until analysis is on or keys are known, and gives way on a small screen at a large
interface size so Artist and Song keep their room. Keys are always spelt the
familiar way (Db, Eb, F#, Ab, Bb; C#m, Ebm, F#m, G#m, Bbm), never B#, E#, Cb or Fb.

Working keys out means reading every song, so it is **off until turned on** in
Settings > Metadata > Song keys. Once on, it works in short batches between other library
work, stops the moment a song starts and waits 45 seconds after one ends (Autoplay may be
about to start the next), gives way to scans at once, and carries on in later sessions
where it left off; the same page shows progress and an estimate of the time left. On a
2.1 GHz cloud CPU a 4-minute song took about 0.5 s in a release build (decoding the MP3 is
most of it), so 50,000 songs is roughly 7 hours of work plus reading from the drive and the
rests between batches: expect 8 to 15 hours spread over several sessions. Measure on the
real laptop and drive with `fks-song-keys` (below).

**Set Song Key.** A key can also be chosen by hand: select a song in the library and press
**Set Song Key** (in the library's footer, beside Add to Playlist). A small popup lists the
24 keys (major and minor, in the spellings above); one click saves it and closes. It is the
key the backing track is *recorded* in, before any Key +/- adjustment: a song recorded in C
and sung at +2 is set to C, and the Now Playing line then shows `Key C → D`. A key chosen by
hand always wins over the detected one, is never replaced by analysis, and is kept with the
other trusted corrections in `metadata-overrides.sqlite` (field `original_key`; mirrored in
the catalogue), so it survives restarts, rescans and catalogue rebuilds. **Clear Manual Key**
in the same popup goes back to the detected key (or blank). The detected key itself stays in
`enrichment-cache.sqlite`, untouched. The Key cell in the table is display only: clicking
anywhere in a row, the Key cell included, just selects the song.

**How.** The MP3 is decoded read-only with the GStreamer components the player already
uses, to mono at 11 kHz. Spectral peaks between 60 Hz and 2 kHz are gathered into a
12-note profile, corrected for the recording's tuning, with every loud-enough second
counting the same (so silent or spoken intros, fades and a last-chorus key change do not
decide it), and compared with major and minor key templates (Sha'ath's audio-derived
profiles). No extra library or plugin is needed on macOS or Windows.

**Limits.** Automatic key finding is imperfect. On 30 real karaoke tracks checked by hand
(independent published keys plus the backing audio itself), 20 keys were shown and 19 were
exact; one was a fifth away; the rest stayed blank. Most mistakes are the relative major/minor (C for Am), a fifth away (G for C) or
major for minor. So a key is shown only when the evidence
is clear: a strong match, clearly better than any other key (major versus minor included),
consistent through the song, and tuning not near a quarter-tone. Only passages with a
clear note in the 60 Hz to 2 kHz band count, so silence, hiss, drums alone, a DC offset
and a hum below the band are not counted. (A mains buzz with strong harmonics inside the
band is a real series of notes and can still suggest a key; it matters only for a file
with nothing else in it.) Everything else stays
blank, as do songs shorter than 30 s, mostly silent, or unreadable. Songs that modulate
get the key heard longest. The thresholds were set on synthetic music and must be checked
against real songs (see below) before they are trusted.

**Storage.** Results go in `enrichment-cache.sqlite` (table `song_keys`), which outlives
catalogue rebuilds, keyed by the MP3's audio content with its tags ignored: a moved,
renamed or retagged song keeps its key without being read again, and a rebuilt catalogue
only reads 128 KiB of each song to find its key again. Each row keeps the key, status
(`confident`, `uncertain`, `silent`, `too_short`, `not_audio`), a confidence, the analysis
version and the evidence (correlations, runner-up, margins, agreement, tuning and the
song's whole 12-note profile, so the decision can be re-tuned later without decoding the
songs again). Raising
`kSongKeyAnalysisVersion` re-analyses in the background, showing the old key meanwhile.
A damaged file is recorded and not tried again; a file that cannot be read (the drive
went away) is tried again next session. Nothing is ever written to the music folder.

**Checking on real songs.** `fks-song-keys` analyses files read-only and prints each key
with its evidence and timing, and estimates for 1,000 / 10,000 / 50,000 songs:

```bash
build/fks-song-keys --limit 30 "$FKS_TEST_MEDIA_DIR"
build/fks-song-keys --expect keys.tsv --limit 30 "$FKS_TEST_MEDIA_DIR"
```

`keys.tsv` lists songs whose key is known independently, one per line as
`<part of the file name><TAB><key>`; the report then counts exact, relative, fifth and
parallel mistakes, for all songs and for those whose key would be shown.

## Layout

| Path | Purpose |
| --- | --- |
| `src/cdg/CdgDecoder.*` | CD+G decoding (plain C++, no Qt/GStreamer) |
| `src/music/` | Musical keys, transposition and key detection from samples (plain C++) |
| `src/SongKeyAnalyser.*` | Decodes a song read-only with GStreamer for key detection |
| `src/KaraokePlayer.*` | GStreamer audio playback; drives the CDG decoder from the audio position |
| `src/SongPair.*` | Finds the matching `.mp3`/`.cdg` companion and checks both are readable |
| `src/SongSettings.*` | Content identity and atomic per-song Key/Tempo JSON storage |
| `src/library/` | Catalogue, filename/tag/track-list parsing, content identity, metadata resolution and background scanning |
| `src/library/KaraokeLabels.*` | Conservative karaoke label/series identification |
| `src/library/UserStateStore.*` | Play history and preferences (`user-state.sqlite`) |
| `src/library/KnownLibraryRoots.*` | Music folders remembered outside the catalogue, for storage safety |
| `src/cdg/CdgTitleFrames.*` | Finds title-screen frames at the start of a CD+G stream |
| `src/ocr/` | Local title-screen text recognition (Apple Vision on macOS; none elsewhere yet) |
| `src/MetadataReviewDialog.*` | Library maintenance: review, correct and reprocess song names |
| `src/LibraryController.*` | Active music root, GUI catalogue connection and scanner-thread lifecycle |
| `src/LibraryView.*` | Large-text search, results and folder setup page |
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
