# macOS release package

Builds the self-contained, Apple Silicon `FrankiesKaraokeStudio.app` and a disk image
from a Release build. The package carries its own Qt and GStreamer, so it runs on a Mac
with neither installed. No product code is involved: these scripts only package and
check what CMake built.

| File | Purpose |
| --- | --- |
| `package-macos.sh` | Makes the `.app` and the `.dmg` from a Release build |
| `verify-macos.sh` | Independent static audit of the `.app` (and optionally the `.dmg`) |

## What is produced

| Property | Value |
| --- | --- |
| Architecture | arm64 only (Apple Silicon) |
| Minimum macOS | 13.0 (Qt 6.11's minimum) |
| Qt | 6.11.2 (`~/Qt/6.11.2/macos`, official installer) |
| GStreamer | 1.28.7, official framework (`~/Library/Frameworks/GStreamer.framework`) |
| Signature | ad hoc, no hardened runtime |
| Notarised | no (no Developer ID) |

Inside the bundle:

```text
Contents/MacOS/FrankiesKaraokeStudio       the program
Contents/MacOS/gst-plugin-scanner          GStreamer's plugin scanner
Contents/MacOS/lib/gstreamer-1.0           -> ../../PlugIns/gstreamer/plugins
Contents/PlugIns/gstreamer/plugins/        the 16 karaoke GStreamer plugins
Contents/Frameworks/                       Qt frameworks, GStreamer and GLib libraries
Contents/PlugIns/                          Qt plugins (cocoa, styles, sqlite, images)
```

The program counts as packaged when `lib/gstreamer-1.0` exists beside it
(`KaraokePlayer::initializeGStreamer`, `SelfCheck`), and then loads only those plugins.
`codesign` treats a folder with a dot in its name inside a code folder as a bundle, so
`gstreamer-1.0` is a symbolic link to a dot-free folder of the same depth.

## Prerequisites

- An Apple Silicon Mac with Xcode or the Command Line Tools (`otool`, `lipo`,
  `install_name_tool`, `codesign`, `hdiutil`), CMake 3.22 or newer.
- Qt 6.11.2 for macOS from the official Qt installer.
- The official GStreamer 1.28.7 framework (runtime and development packages). A
  Homebrew GStreamer is refused: it lacks the SoundTouch `pitch` element.

## Build

From the repository root of a clean checkout that contains these scripts. The release
tags themselves predate them: check out the release's packaging branch, which is the tag
plus packaging files only (`release/v0.1.0-packaging` for `v0.1.0` at `b32edf2`,
`release/v0.1.1-packaging` for `v0.1.1` at `caae61b`).

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 \
    -DCMAKE_PREFIX_PATH=$HOME/Qt/6.11.2/macos \
    -DGSTREAMER_ROOT=$HOME/Library/Frameworks/GStreamer.framework/Versions/1.0 \
    -DFKS_WARNINGS_AS_ERRORS=ON
cmake --build build-release -j8
ctest --test-dir build-release --output-on-failure
```

The build must finish with zero warnings and every test must pass.

## Package

```bash
packaging/macos/package-macos.sh build-release $HOME/Qt/6.11.2/macos \
    $HOME/Library/Frameworks/GStreamer.framework/Versions/1.0 ~/fks-macos-package
```

This produces, in the output folder (here `~/fks-macos-package`):

- `FrankiesKaraokeStudio.app`
- `FrankiesKaraokeStudio-<version>-macOS-arm64.dmg` (the app and an Applications link)

Keep the output folder outside the source tree. The script refuses an output folder
that overlaps its inputs, is a symbolic link, or holds anything other than a previous
package. It builds everything in a temporary folder and replaces the previous package
only when the new one is complete.

## Verify

```bash
packaging/macos/verify-macos.sh ~/fks-macos-package/FrankiesKaraokeStudio.app \
    ~/fks-macos-package/FrankiesKaraokeStudio-<version>-macOS-arm64.dmg
```

It must end with `AUDIT: PASS`. It checks:

- the layout and plugin set;
- every Mach-O file is arm64, needs at most the minimum macOS, and loads only libraries
  inside the bundle or the system's;
- no SDK, Homebrew or user path remains;
- the ad-hoc signature and Info.plist (values and types);
- `qt.conf` and that the icon is `packaging/icon/FrankiesKaraokeStudio.icns`;
- that the app inside the disk image is identical to the audited app.

Then run the program's own installation check with a synthetic song
(`build-release/tests/fks-make-test-songs <folder>` writes some):

```bash
~/fks-macos-package/FrankiesKaraokeStudio.app/Contents/MacOS/FrankiesKaraokeStudio \
    --self-check ~/fks-self-check.txt "<folder>/FKT01-01 - Test Singer - Clicks In Time.mp3"
```

The report must say `Packaged copy: yes`, name only files inside the app for every audio
component, and end with `RESULT: PASS`. To prove nothing is loaded from the SDKs, run
the same command under `sandbox-exec` with a profile that denies reading
`~/Library/Frameworks`, `~/Qt`, `/opt/homebrew` and `/usr/local`.

## Install

Open the disk image and drag the app onto Applications, or copy it with
`ditto`. A package made on the same Mac opens normally.

## Known limitations

- **Not notarised, no Developer ID.** The ad-hoc signature is enough on the Mac it was
  made on. A copy that arrives through a browser, mail, AirDrop or cloud storage is
  quarantined, and Gatekeeper blocks the first launch. Allow it once with Control-click
  > Open, or System Settings > Privacy & Security > Open Anyway.
- **Apple Silicon only.** An Intel build needs `CMAKE_OSX_ARCHITECTURES` and the
  scripts' `ARCH` changed (Qt and GStreamer are universal).
- **Environment variables.** The program ignores external GStreamer plugin paths. When
  started from a Terminal that sets Qt or GIO plugin variables (`QT_PLUGIN_PATH`,
  `GIO_EXTRA_MODULES`), those are still honoured. Starting it from Finder or the Dock
  is unaffected.
- **Program data folder.** The package uses the same data folder as development builds
  (`~/Library/Application Support/Granda/FrankiesKaraokeStudio`).
