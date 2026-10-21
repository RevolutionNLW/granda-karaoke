#!/bin/bash
# Builds the self-contained macOS (Apple Silicon) package of Frankie's Karaoke
# Studio from an already-built Release bundle:
#
#   <Output>/FrankiesKaraokeStudio.app
#     Contents/MacOS/FrankiesKaraokeStudio       the program
#     Contents/MacOS/gst-plugin-scanner          GStreamer's plugin scanner
#     Contents/MacOS/lib/gstreamer-1.0           -> ../../PlugIns/gstreamer/plugins
#     Contents/PlugIns/gstreamer/plugins/*.dylib the karaoke audio plugins
#     Contents/Frameworks/                       Qt frameworks + GStreamer/GLib libraries
#     Contents/PlugIns/                          Qt plugins (cocoa, styles, sqlite, ...)
#   <Output>/FrankiesKaraokeStudio-<version>-macOS-arm64.dmg
#
# The program treats itself as packaged when <program folder>/lib/gstreamer-1.0
# exists (KaraokePlayer::initializeGStreamer, SelfCheck), and then uses only
# those plugins and the scanner beside the program. On macOS the program folder
# is Contents/MacOS, so that folder must exist there. No product code is changed.
# codesign takes a folder with a dot in its name inside a code folder for a
# bundle, so "gstreamer-1.0" is a symbolic link to a dot-free folder of the
# same depth (@loader_path/../../../Frameworks fits both paths).
#
# Ad-hoc signed, no hardened runtime: for the owner's own Mac, not for
# distribution (no Developer ID, not notarised).
#
# Usage: package-macos.sh <BuildDir> <QtPrefix> <GStreamerRoot> <Output>
set -euo pipefail

if [[ $# -ne 4 ]]; then
    echo "usage: $0 <BuildDir> <QtPrefix> <GStreamerRoot> <Output>" >&2
    exit 2
fi
BUILD_DIR=$(cd "$1" && pwd)
QT_PREFIX=$(cd "$2" && pwd)
GST_ROOT=$(cd "$3" && pwd)
OUTPUT=$4
ARCH=arm64

fail() { echo "ERROR: $*" >&2; exit 1; }

SOURCE_APP="$BUILD_DIR/FrankiesKaraokeStudio.app"
[[ -x "$SOURCE_APP/Contents/MacOS/FrankiesKaraokeStudio" ]] || fail "no built program in $SOURCE_APP"
[[ "$(lipo -archs "$SOURCE_APP/Contents/MacOS/FrankiesKaraokeStudio")" == "$ARCH" ]] \
    || fail "the built program is not $ARCH-only"
grep -q '^CMAKE_BUILD_TYPE:STRING=Release$' "$BUILD_DIR/CMakeCache.txt" || fail "not a Release build"
[[ -x "$QT_PREFIX/bin/macdeployqt6" ]] || fail "macdeployqt6 not found in $QT_PREFIX/bin"
[[ -f "$GST_ROOT/lib/libgstreamer-1.0.0.dylib" ]] || fail "not a GStreamer SDK root: $GST_ROOT"
case "$GST_ROOT" in
    /opt/homebrew*|/usr/local*) fail "Homebrew GStreamer is not supported (no SoundTouch pitch)";;
esac

VERSION=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$SOURCE_APP/Contents/Info.plist")
[[ -n "$VERSION" ]] || fail "the built bundle has no version"
# Recorded for the package's provenance; README.md names the versions in use.
echo "Program $VERSION; Qt $("$QT_PREFIX/bin/qtpaths6" --qt-query QT_VERSION 2>/dev/null || echo '?');" \
     "GStreamer $(sed -n 's/^Version: //p' "$GST_ROOT/lib/pkgconfig/gstreamer-1.0.pc" 2>/dev/null || echo '?')"

# The output folder holds only this script's two artefacts. Everything is
# built in a temporary folder beside it and published only when complete, so a
# failure never leaves a half-made package or removes the previous one.
while [[ "$OUTPUT" == */ && "$OUTPUT" != / ]]; do OUTPUT=${OUTPUT%/}; done
case "$(basename "$OUTPUT")" in .|..|/) fail "name the output folder itself, not '$OUTPUT'";; esac
[[ -L "$OUTPUT" ]] && fail "the output folder must not be a symbolic link: $OUTPUT"
[[ -d "$(dirname "$OUTPUT")" ]] || fail "the folder for $OUTPUT does not exist"
OUTPUT="$(cd "$(dirname "$OUTPUT")" && pwd -P)/$(basename "$OUTPUT")"
DMG_NAME="FrankiesKaraokeStudio-$VERSION-macOS-$ARCH.dmg"
inside() { [[ "$1/" == "$2/"* ]]; }   # $1 is $2 or inside it
case "$OUTPUT" in
    /|"$(cd ~ && pwd -P)") fail "refusing to use $OUTPUT as the output folder";;
esac
for input in "$BUILD_DIR" "$QT_PREFIX" "$GST_ROOT" "$(cd "$(dirname "$0")" && pwd -P)"; do
    input=$(cd "$input" && pwd -P)
    if inside "$OUTPUT" "$input" || inside "$input" "$OUTPUT"; then
        fail "the output folder $OUTPUT overlaps the input $input"
    fi
done
if [[ -d "$OUTPUT" ]]; then
    ENTRIES=$(mktemp)
    find "$OUTPUT" -mindepth 1 -maxdepth 1 -print0 > "$ENTRIES" || fail "cannot list $OUTPUT"
    while IFS= read -r -d '' entry; do
        name=$(basename "$entry")
        if [[ "$name" == FrankiesKaraokeStudio.app && ( -L "$entry" || ! -d "$entry" ) ]] \
            || [[ "$name" == "$DMG_NAME" && ( -L "$entry" || ! -f "$entry" ) ]]; then
            rm -f "$ENTRIES"; fail "$entry is not a plain previous package"
        fi
        case "$name" in
            FrankiesKaraokeStudio.app|"$DMG_NAME"|.DS_Store) ;;
            *) rm -f "$ENTRIES"; fail "the output folder $OUTPUT holds something else: $(basename "$entry")";;
        esac
    done < "$ENTRIES"
    rm -f "$ENTRIES"
elif [[ -e "$OUTPUT" ]]; then
    fail "$OUTPUT exists and is not a folder"
fi
mkdir -p "$OUTPUT"
WORK=$(mktemp -d "$(dirname "$OUTPUT")/.fks-package.XXXXXX")
NEW_APP="$OUTPUT/.FrankiesKaraokeStudio.app.new"
NEW_DMG="$OUTPUT/.$DMG_NAME.new"
OLD_APP="$OUTPUT/.FrankiesKaraokeStudio.app.old"
[[ -e "$OLD_APP" ]] && fail "$OLD_APP is left from an interrupted run: check it, then remove it"
trap 'rm -rf "$WORK" "$NEW_APP" "$NEW_DMG"' EXIT
APP="$WORK/FrankiesKaraokeStudio.app"
CONTENTS="$APP/Contents"
MACOS="$CONTENTS/MacOS"
FRAMEWORKS="$CONTENTS/Frameworks"
EXE="$MACOS/FrankiesKaraokeStudio"

ditto --norsrc --noextattr "$SOURCE_APP" "$APP"

# --- Qt -----------------------------------------------------------------------
# macdeployqt copies the Qt frameworks and plugins (platforms/libqcocoa, styles,
# imageformats, sqldrivers). Signing is done once, below, after every change.
"$QT_PREFIX/bin/macdeployqt6" "$APP" -verbose=1 -no-strip -no-codesign -libpath="$QT_PREFIX/lib"
[[ -f "$CONTENTS/PlugIns/platforms/libqcocoa.dylib" ]] || fail "Qt deployment is missing platforms/libqcocoa"
[[ -f "$CONTENTS/PlugIns/sqldrivers/libqsqlite.dylib" ]] || fail "Qt deployment is missing sqldrivers/libqsqlite"
for framework in QtCore QtGui QtWidgets QtSql; do
    [[ -d "$FRAMEWORKS/$framework.framework" ]] || fail "Qt deployment is missing $framework"
done
# Only the SQLite driver is used.
find "$CONTENTS/PlugIns/sqldrivers" -name '*.dylib' ! -name 'libqsqlite.dylib' -delete

# --- GStreamer -------------------------------------------------------------------
# Only the plugins a karaoke song needs (see KaraokePlayer::requiredElements),
# the same set as the Windows package with the macOS sound output (osxaudio).
PLUGINS=(
    coreelements playback typefindfunctions pbtypes
    audioconvert audioresample volume audiotestsrc autodetect
    osxaudio
    soundtouch audiofx
    mpg123 audioparsers id3demux apetag
)
PLUGIN_DIR="$CONTENTS/PlugIns/gstreamer/plugins"
mkdir -p "$PLUGIN_DIR" "$MACOS/lib"
ln -s ../../PlugIns/gstreamer/plugins "$MACOS/lib/gstreamer-1.0"
for plugin in "${PLUGINS[@]}"; do
    file="$GST_ROOT/lib/gstreamer-1.0/libgst$plugin.dylib"
    [[ -f "$file" ]] || fail "GStreamer plugin missing from the SDK: $plugin"
    cp -L "$file" "$PLUGIN_DIR/"
done
SCANNER_SOURCE="$GST_ROOT/libexec/gstreamer-1.0/gst-plugin-scanner"
[[ -f "$SCANNER_SOURCE" ]] || fail "gst-plugin-scanner not found"
cp -L "$SCANNER_SOURCE" "$MACOS/gst-plugin-scanner"

# Every Mach-O file in the bundle.
macho_files() {
    find "$APP" -type f -print0 | while IFS= read -r -d '' f; do
        if file -b "$f" | grep -q 'Mach-O'; then printf '%s\n' "$f"; fi
    done
}

# Dependencies of a Mach-O file (its own install name excluded).
dependencies() {
    otool -L -arch "$ARCH" "$1" | tail -n +2 | awk '{print $1}' | {
        local id
        id=$(otool -D -arch "$ARCH" "$1" | tail -n +2)
        grep -vxF "${id:-<none>}" || true
    }
}

# Every @rpath library any packaged file needs, found in the GStreamer SDK, is
# copied into Contents/Frameworks, repeatedly until nothing new is needed.
# Qt frameworks were deployed by macdeployqt and must already be present.
while :; do
    added=0
    while IFS= read -r f; do
        while IFS= read -r dep; do
            case "$dep" in
                /usr/lib/*|/System/*) ;;
                @rpath/*.framework/*)
                    name=${dep#@rpath/}
                    [[ -e "$FRAMEWORKS/$name" ]] || fail "$(basename "$f") needs $dep, not deployed"
                    ;;
                @rpath/*)
                    name=${dep#@rpath/}
                    if [[ ! -e "$FRAMEWORKS/$name" ]]; then
                        [[ -f "$GST_ROOT/lib/$name" ]] || fail "$(basename "$f") needs $dep, not in the GStreamer SDK"
                        cp -L "$GST_ROOT/lib/$name" "$FRAMEWORKS/$name"
                        chmod u+w "$FRAMEWORKS/$name"
                        echo "  + Frameworks/$name (for $(basename "$f"))"
                        added=1
                    fi
                    ;;
                @executable_path/*|@loader_path/*) ;;   # checked by the audit below
                *) fail "$(basename "$f") links to $dep outside the bundle";;
            esac
        done < <(dependencies "$f")
    done < <(macho_files)
    [[ $added -eq 0 ]] && break
done

# --- Apple Silicon only, and only in-bundle library search paths ----------------
while IFS= read -r f; do
    chmod u+w "$f"
    archs=$(lipo -archs "$f")
    if [[ "$archs" != "$ARCH" ]]; then
        [[ " $archs " == *" $ARCH "* ]] || fail "$f has no $ARCH code ($archs)"
        lipo -thin "$ARCH" "$f" -output "$f.thin"
        mv "$f.thin" "$f"
    fi
done < <(macho_files)

# Replaces every LC_RPATH of a file with exactly the given one.
set_rpath() {
    local f=$1 wanted=$2 existing
    while IFS= read -r existing; do
        install_name_tool -delete_rpath "$existing" "$f" 2>/dev/null
    done < <(otool -l "$f" | awk '/cmd LC_RPATH/{getline; getline; print $2}')
    install_name_tool -add_rpath "$wanted" "$f" 2>/dev/null
}

set_rpath "$EXE" "@executable_path/../Frameworks"
set_rpath "$MACOS/gst-plugin-scanner" "@executable_path/../Frameworks"
for f in "$PLUGIN_DIR"/*.dylib; do
    set_rpath "$f" "@loader_path/../../../Frameworks"
done
for f in "$FRAMEWORKS"/*.dylib; do
    set_rpath "$f" "@loader_path"
done

# --- Info.plist -------------------------------------------------------------------
PLIST="$CONTENTS/Info.plist"
plist_set() {   # key -type value
    plutil -replace "$1" "$2" "$3" "$PLIST"
}
plist_set CFBundleVersion -string "$VERSION"
plist_set CFBundleDevelopmentRegion -string en
plist_set NSHumanReadableCopyright -string "Frankie's Karaoke Studio $VERSION"
plist_set LSMinimumSystemVersion -string 13.0
plist_set LSApplicationCategoryType -string public.app-category.music
plist_set NSPrincipalClass -string NSApplication
plist_set NSHighResolutionCapable -bool YES
for empty in CFBundleGetInfoString CFBundleLongVersionString; do
    plutil -remove "$empty" "$PLIST" 2>/dev/null || true
done
plutil -lint "$PLIST" >/dev/null
[[ "$(plutil -extract NSHumanReadableCopyright raw "$PLIST")" == "Frankie's Karaoke Studio $VERSION" ]] \
    || fail "Info.plist was not updated"

# --- Ad-hoc signature, innermost code first ------------------------------------------
xattr -cr "$APP"
sign() { codesign --force --sign - --timestamp=none "$@"; }
while IFS= read -r f; do
    case "$f" in
        "$EXE") ;;                                   # signed with the bundle
        *.framework/*) ;;                            # signed as frameworks
        *) sign "$f";;
    esac
done < <(macho_files)
for framework in "$FRAMEWORKS"/*.framework; do
    sign "$framework"
done
sign "$APP"
codesign --verify --deep --strict "$APP"

# --- Disk image ----------------------------------------------------------------------
DMG="$WORK/$DMG_NAME"
STAGE="$WORK/dmg-contents"
mkdir "$STAGE"
ditto "$APP" "$STAGE/FrankiesKaraokeStudio.app"
ln -s /Applications "$STAGE/Applications"
hdiutil create -quiet -volname "Frankie's Karaoke Studio $VERSION" -srcfolder "$STAGE" \
    -fs HFS+ -format UDZO -ov "$DMG"
hdiutil verify -quiet "$DMG"
rm -rf "$STAGE"

# --- Publish -------------------------------------------------------------------------
# Both new artefacts are first copied into the output folder under temporary
# names (the slow, fallible step), then renamed into place within that folder.
rm -rf "$NEW_APP" "$NEW_DMG"
ditto "$APP" "$NEW_APP"
cp -p "$DMG" "$NEW_DMG"
cmp -s "$DMG" "$NEW_DMG" || fail "the disk image did not copy into $OUTPUT intact"
codesign --verify --deep --strict "$NEW_APP" || fail "the app did not copy into $OUTPUT intact"
# The previous app is kept aside until both new artefacts are in place; if this
# step is interrupted it stays as .FrankiesKaraokeStudio.app.old.
[[ -d "$OUTPUT/FrankiesKaraokeStudio.app" ]] && mv "$OUTPUT/FrankiesKaraokeStudio.app" "$OLD_APP"
mv "$NEW_APP" "$OUTPUT/FrankiesKaraokeStudio.app"
mv -f "$NEW_DMG" "$OUTPUT/$DMG_NAME"
[[ -f "$OUTPUT/$DMG_NAME" && -d "$OUTPUT/FrankiesKaraokeStudio.app" ]] || fail "publishing into $OUTPUT failed"
rm -rf "$OLD_APP"
echo "Packaged $OUTPUT/FrankiesKaraokeStudio.app"
echo "Disk image $OUTPUT/$DMG_NAME"
