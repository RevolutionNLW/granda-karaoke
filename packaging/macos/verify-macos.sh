#!/bin/bash
# Static audit of a packaged FrankiesKaraokeStudio.app, independent of
# package-macos.sh. Prints one line per check and "AUDIT: PASS" or exits 1.
#
# With a disk image as well, the app inside it must be byte-identical to the
# audited app, beside an Applications link and nothing else.
#
# Usage: verify-macos.sh <FrankiesKaraokeStudio.app> [<disk image>]
set -uo pipefail
if [[ $# -lt 1 || $# -gt 2 ]]; then
    echo "usage: $0 <FrankiesKaraokeStudio.app> [<disk image>]" >&2
    exit 2
fi
APP=$(cd "$1" && pwd -P)   # real path: library locations are compared as real paths
DMG=${2:-}
# This script lives in packaging/macos of the source tree the app was built from.
REPO=$(cd "$(dirname "$0")/../.." && pwd)
VERSION=$(sed -n 's/^project(FrankiesKaraokeStudio VERSION \([0-9.]*\).*/\1/p' "$REPO/CMakeLists.txt")
[[ -n "$VERSION" ]] || { echo "cannot read the version from $REPO/CMakeLists.txt" >&2; exit 2; }
CONTENTS="$APP/Contents"
FAILURES=0
pass() { echo "PASS  $*"; }
bad()  { echo "FAIL  $*"; FAILURES=$((FAILURES + 1)); }

macho_files() {
    find "$APP" -type f -print0 | while IFS= read -r -d '' f; do
        if file -b "$f" | grep -q 'Mach-O'; then printf '%s\n' "$f"; fi
    done
}
rpaths_of() { otool -l "$1" | awk '/cmd LC_RPATH/{getline; getline; print $2}'; }

# Expands @executable_path/@loader_path in an rpath for an image at $2 loaded by $3.
expand() {
    local p=$1 image=$2 exe=$3
    p=${p//@loader_path/$(dirname "$image")}
    p=${p//@executable_path/$(dirname "$exe")}
    printf '%s\n' "$p"
}

EXE="$CONTENTS/MacOS/FrankiesKaraokeStudio"
SCANNER="$CONTENTS/MacOS/gst-plugin-scanner"
LINK="$CONTENTS/MacOS/lib/gstreamer-1.0"

# --- Layout ---------------------------------------------------------------------
[[ -x "$EXE" ]] && pass "program present" || bad "program missing"
[[ -x "$SCANNER" ]] && pass "gst-plugin-scanner beside the program" || bad "gst-plugin-scanner missing"
if [[ -d "$LINK" ]]; then
    real=$(cd "$LINK" && pwd -P)
    [[ "$real" == "$CONTENTS/"* ]] && pass "MacOS/lib/gstreamer-1.0 -> ${real#"$APP"/}" \
        || bad "MacOS/lib/gstreamer-1.0 resolves outside the bundle: $real"
else
    bad "MacOS/lib/gstreamer-1.0 missing (program would not see itself as packaged)"
fi
expected="apetag audioconvert audiofx audioparsers audioresample audiotestsrc autodetect coreelements id3demux mpg123 osxaudio pbtypes playback soundtouch typefindfunctions volume"
actual=$(ls "$LINK" 2>/dev/null | sed -n 's/^libgst\(.*\)\.dylib$/\1/p' | sort | xargs)
[[ "$actual" == "$expected" ]] && pass "plugins: $actual" || bad "plugins differ: '$actual'"
for f in QtCore QtGui QtWidgets QtSql; do
    [[ -d "$CONTENTS/Frameworks/$f.framework" ]] || bad "Qt framework $f missing"
done
[[ -f "$CONTENTS/PlugIns/platforms/libqcocoa.dylib" ]] && pass "Qt cocoa platform plugin" || bad "no cocoa plugin"
sql=$(ls "$CONTENTS/PlugIns/sqldrivers" | xargs)
[[ "$sql" == "libqsqlite.dylib" ]] && pass "only the SQLite driver" || bad "sqldrivers: $sql"

# --- Every Mach-O: arm64 only, no outside paths, every library found inside -------
MIN_OS=$(plutil -extract LSMinimumSystemVersion raw "$CONTENTS/Info.plist" 2>/dev/null)
[[ -n "$MIN_OS" ]] || { bad "Info.plist has no LSMinimumSystemVersion"; MIN_OS=0; }
count=0
failures_before=$FAILURES
while IFS= read -r f; do
    count=$((count + 1))
    rel=${f#"$APP"/}
    [[ "$(lipo -archs "$f")" == "arm64" ]] || bad "$rel is $(lipo -archs "$f")"
    if otool -l "$f" | tail -n +2 | grep -E '^ +(name|path) ' | sed -E 's/ \(offset [0-9]+\)$//' | grep -E '^ +(name|path) .*[[:space:]]' >/dev/null; then
        bad "$rel has an install name or rpath containing a space"
    fi
    fmin=$(otool -l "$f" | awk '/LC_BUILD_VERSION/{b=1} b&&/minos/{print $2; exit} /LC_VERSION_MIN_MACOSX/{m=1} m&&/version/{print $2; exit}')
    if [[ -z "$fmin" ]]; then
        bad "$rel declares no minimum macOS"
    elif [[ "$(printf '%s\n' "$fmin" "$MIN_OS" | sort -V | tail -1)" != "$MIN_OS" ]]; then
        bad "$rel needs macOS $fmin, newer than LSMinimumSystemVersion $MIN_OS"
    fi
    if otool -l "$f" | grep -E '^ +(name|path) ' | grep -vE ' (@rpath|@loader_path|@executable_path|/usr/lib/|/System/)' >/dev/null; then
        bad "$rel has an outside load path: $(otool -l "$f" | grep -E '^ +(name|path) ' | grep -vE ' (@rpath|@loader_path|@executable_path|/usr/lib/|/System/)' | head -1)"
    fi
    # Which executable loads it: the scanner loads only GStreamer, the program everything.
    loaders=("$EXE")
    case "$rel" in Contents/PlugIns/gstreamer/*|Contents/Frameworks/lib*) loaders+=("$SCANNER");; esac
    # Plugins are reached through the symbolic link as well as their real folder.
    images=("$f")
    [[ "$rel" == Contents/PlugIns/gstreamer/plugins/* ]] && images+=("$LINK/$(basename "$f")")
    while IFS= read -r dep; do
        case "$dep" in
            /usr/lib/*|/System/*) continue;;
        esac
        for image in "${images[@]}"; do
            for exe in "${loaders[@]}"; do
                found=""
                if [[ "$dep" == @rpath/* ]]; then
                    for rp in $(rpaths_of "$image") $(rpaths_of "$exe"); do
                        cand="$(expand "$rp" "$image" "$exe")/${dep#@rpath/}"
                        if [[ -e "$cand" ]]; then found=$cand; break; fi
                    done
                else
                    cand=$(expand "$dep" "$image" "$exe")
                    [[ -e "$cand" ]] && found=$cand
                fi
                if [[ -z "$found" ]]; then
                    bad "$rel: $dep not found (loaded by $(basename "$exe"))"
                else
                    realfound=$(cd "$(dirname "$found")" && pwd -P)
                    [[ "$realfound" == "$APP/"* ]] || bad "$rel: $dep resolves outside: $found"
                fi
            done
        done
    done < <(otool -L "$f" | tail -n +2 | awk '{print $1}' | grep -vxF "$(otool -D "$f" | tail -n +2)")
done < <(macho_files)
if [[ $FAILURES -eq $failures_before ]]; then
    pass "$count Mach-O files checked (arm64, minimum macOS <= $MIN_OS, load paths, library resolution)"
else
    echo "....  $count Mach-O files checked, with the failures above"
fi

# No absolute path to this Mac's SDKs anywhere in a load command.
if macho_files | while IFS= read -r f; do otool -l "$f" | tail -n +2; done | grep -E '/Users/|homebrew|/usr/local|GStreamer\.framework|/Qt/' >/dev/null; then
    bad "a load command names an SDK or user path"
else
    pass "no load command names /Users, Homebrew, /usr/local, GStreamer.framework or Qt"
fi

# --- Signature --------------------------------------------------------------------------
if codesign --verify --deep --strict "$APP" 2>/dev/null; then pass "codesign --verify --deep --strict"; else bad "codesign verification"; fi
sig=$(codesign -dv "$APP" 2>&1)
grep -q 'Signature=adhoc' <<<"$sig" && pass "ad-hoc signature" || bad "not ad-hoc signed"
grep -q 'flags=.*runtime' <<<"$sig" && bad "hardened runtime set (ad-hoc libraries would be refused)" || pass "no hardened runtime"

# --- Info.plist and icon -------------------------------------------------------------------
P="$CONTENTS/Info.plist"
v() { plutil -extract "$1" raw "$P" 2>/dev/null; }
types_ok=1
for entry in CFBundleIdentifier:string CFBundleExecutable:string CFBundleName:string CFBundlePackageType:string \
             CFBundleShortVersionString:string CFBundleVersion:string CFBundleIconFile:string \
             LSMinimumSystemVersion:string NSPrincipalClass:string NSHighResolutionCapable:bool; do
    t=$(plutil -type "${entry%%:*}" "$P" 2>/dev/null)
    [[ "$t" == "${entry##*:}" ]] || { bad "Info.plist ${entry%%:*} is '${t:-missing}', not ${entry##*:}"; types_ok=0; }
done
[[ $types_ok -eq 1 ]] && pass "Info.plist value types"
[[ "$(v CFBundleExecutable)" == "FrankiesKaraokeStudio" && "$(v CFBundlePackageType)" == "APPL" ]] \
    && pass "executable and package type" || bad "CFBundleExecutable/CFBundlePackageType"
[[ "$(v CFBundleIdentifier)" == "com.granda.FrankiesKaraokeStudio" ]] && pass "bundle id" || bad "bundle id $(v CFBundleIdentifier)"
[[ "$(v CFBundleShortVersionString)" == "$VERSION" && "$(v CFBundleVersion)" == "$VERSION" ]] \
    && pass "version $VERSION (CMakeLists.txt)" \
    || bad "version: short $(v CFBundleShortVersionString), bundle $(v CFBundleVersion); expected $VERSION for both"
[[ "$(v CFBundleName)" == "Frankie's Karaoke Studio" ]] && pass "name Frankie's Karaoke Studio" || bad "name"
[[ "$(v LSMinimumSystemVersion)" == "13.0" ]] && pass "minimum macOS 13.0" || bad "LSMinimumSystemVersion"
[[ "$(v NSHighResolutionCapable)" == "true" ]] && pass "high resolution" || bad "NSHighResolutionCapable"
minos=$(otool -l "$EXE" | awk '/LC_BUILD_VERSION/{f=1} f&&/minos/{print $2; exit}')
[[ "$minos" == "13.0" ]] && pass "program built for macOS $minos" || bad "program minos $minos"
icon="$CONTENTS/Resources/$(v CFBundleIconFile)"
if cmp -s "$icon" "$REPO/packaging/icon/FrankiesKaraokeStudio.icns"; then
    pass "icon is the approved packaging/icon/FrankiesKaraokeStudio.icns ($(shasum -a 256 "$icon" | cut -c1-12))"
else
    bad "icon differs from the approved artwork"
fi

# Qt finds its plugins through qt.conf; it must name only the bundle's own folder.
expected_qtconf=$'[Paths]\nPlugins = PlugIns\nImports = Resources/qml\nQmlImports = Resources/qml'
if [[ "$(cat "$CONTENTS/Resources/qt.conf" 2>/dev/null)" == "$expected_qtconf" ]]; then
    pass "qt.conf points Qt only at Contents/PlugIns"
else
    bad "qt.conf missing or unexpected"
fi

# --- Disk image ------------------------------------------------------------------------------
# Every entry (folder, file, link) with its type and mode; file contents and
# link targets as well. Names are NUL-separated, then hex-encoded one per line.
manifest() {
    (cd "$1" && find . -print0 | sort -z | while IFS= read -r -d '' e; do
        mode=$(stat -f '%HT %p' "$e")
        case "$mode" in
            Directory*) extra="";;
            "Regular File"*) extra=$(shasum -a 256 < "$e" | cut -c1-64);;
            "Symbolic Link"*) extra=$(readlink "$e" | od -An -v -tx1 | tr -d ' \n');;
            *) extra="UNSUPPORTED";;
        esac
        printf '%s | %s | %s\n' "$(printf '%s' "$e" | od -An -v -tx1 | tr -d ' \n')" "$mode" "$extra"
    done)
}
if [[ -n "$DMG" ]]; then
    MNT=$(mktemp -d)
    DEVICE=""
    detach() {
        [[ -n "$DEVICE" ]] || return 0
        hdiutil detach -quiet "$DEVICE" || hdiutil detach -quiet -force "$DEVICE" || return 1
        DEVICE=""
    }
    trap 'detach; rmdir "$MNT" 2>/dev/null' EXIT
    trap 'exit 130' INT TERM HUP
    plist=$(hdiutil attach -plist -nobrowse -readonly -noautoopen -mountpoint "$MNT" "$DMG" 2>/dev/null)
    DEVICE=$(printf '%s' "$plist" | plutil -extract system-entities.0.dev-entry raw - 2>/dev/null)
    if [[ -n "$DEVICE" && -d "$MNT/FrankiesKaraokeStudio.app" ]]; then
        top=$(cd "$MNT" && find . -mindepth 1 -maxdepth 1 ! -name .fseventsd ! -name .Trashes ! -name .DS_Store -print0 \
              | sort -z | tr '\0' '|')
        [[ "$top" == "./Applications|./FrankiesKaraokeStudio.app|" ]] \
            && pass "disk image holds the app and an Applications link" || bad "disk image top level: $top"
        [[ -L "$MNT/Applications" && "$(readlink "$MNT/Applications")" == "/Applications" ]] \
            && pass "Applications link" || bad "Applications link missing or wrong"
        if cmp -s <(manifest "$APP") <(manifest "$MNT/FrankiesKaraokeStudio.app"); then
            pass "app in the disk image is identical to the audited app ($(manifest "$APP" | wc -l | tr -d ' ') entries: types, modes, contents)"
        else
            bad "app in the disk image differs from the audited app"
        fi
        manifest "$APP" | grep -q UNSUPPORTED && bad "the app holds an unsupported kind of file"
        codesign --verify --deep --strict "$MNT/FrankiesKaraokeStudio.app" 2>/dev/null \
            && pass "signature valid inside the disk image" || bad "signature invalid inside the disk image"
    else
        bad "could not attach the disk image"
    fi
    if detach; then rmdir "$MNT" 2>/dev/null; else bad "could not detach the disk image ($MNT)"; fi
fi

if [[ $FAILURES -eq 0 ]]; then echo "AUDIT: PASS"; else echo "AUDIT: FAIL ($FAILURES)"; exit 1; fi
