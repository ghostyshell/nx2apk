#!/bin/bash
# nx2apk: turn a Switch game you own into an Android app that runs its code natively (see README.md).
#   ./nx2apk.sh check                               the tools this needs, and what is missing
#   ./nx2apk.sh extract <game.nsp> <prod.keys>      ExeFS, icon and title id into work/ (add --romfs for the RomFS too, about the game's size)
#   ./nx2apk.sh build                               rewritten modules, translated shaders and the APK, from work/
#   ./nx2apk.sh install                             the APK and its files onto the phone over adb (and the RomFS if work/romfs exists)
# Environment: WORK (default ./work), ANDROID_SDK, ANDROID_NDK, HACTOOL, DOTNET_ROOT, SPIRV_CROSS, PYTHON
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${WORK:-$HERE/work}; mkdir -p "$WORK"; WORK=$(cd "$WORK" && pwd)   # absolute: build runs tools from other folders
SDK=${ANDROID_SDK:-/opt/homebrew/share/android-commandlinetools}
NDK=${ANDROID_NDK:-$(ls -d "$SDK"/ndk/* 2>/dev/null | sort -V | tail -1 || true)}
HACTOOL=${HACTOOL:-$(command -v hactool || true)}
PY=${PYTHON:-python3}
PKG=dev.nxandroid.dmc
APPDIR=/sdcard/Android/data/$PKG/files
ROMFS_DEV=/sdcard/nx/dmc/romfs

say() { printf '\n== %s\n' "$*"; }
die() { printf 'nx2apk: %s\n' "$*" >&2; exit 1; }
hac() { "$HACTOOL" -k "$KEYS" --disablekeywarns "$@" 2>&1 | grep -v -i 'WARN' || true; }   # hactool, its key warnings (which print key values) filtered out

check() {
    local bad=0
    have() { if eval "$2" >/dev/null 2>&1; then printf '  ok       %s\n' "$1"; else printf '  MISSING  %s: %s\n' "$1" "$3"; bad=1; fi; }
    say "Tools"
    have "Python 3.9+" "$PY -c 'import sys; sys.exit(sys.version_info < (3, 9))'" "Python 3.9 or newer (or set PYTHON)"
    have "python lz4 + capstone" "$PY -c 'import lz4.block, capstone'" "$PY -m pip install lz4 capstone"
    have hactool "[ -x '$HACTOOL' ]" "build https://github.com/SciresM/hactool and put it on PATH (or set HACTOOL)"
    have "Android SDK build-tools 34" "[ -x '$SDK/build-tools/34.0.0/aapt2' ]" "sdkmanager 'build-tools;34.0.0' (or set ANDROID_SDK)"
    have "Android platform 34" "[ -f '$SDK/platforms/android-34/android.jar' ]" "sdkmanager 'platforms;android-34'"
    have "Android NDK" "[ -d '$NDK/toolchains' ]" "sdkmanager 'ndk;28.2.13676358' (or set ANDROID_NDK)"
    have "Java 17" "[ -x '${JAVA_HOME:-/opt/homebrew/opt/openjdk@17}/bin/java' ]" "brew install openjdk@17 (or set JAVA_HOME)"
    have ".NET 10 runtime" "[ -d '${DOTNET_ROOT:-/usr/local/share/dotnet}/shared/Microsoft.NETCore.App' ]" "https://dot.net (runtime 10.x); set DOTNET_ROOT to its folder unless it is in /usr/local/share/dotnet"
    have spirv-cross "command -v ${SPIRV_CROSS:-spirv-cross}" "brew install spirv-cross (or set SPIRV_CROSS)"
    have adb "command -v adb || [ -x '$SDK/platform-tools/adb' ]" "sdkmanager platform-tools"
    have "debug keystore" "[ -f '$HOME/.android/debug.keystore' ]" "keytool -genkey -v -keystore ~/.android/debug.keystore -storepass android -alias androiddebugkey -keypass android -keyalg RSA -validity 10000 -dname CN=Android"
    [ $bad = 0 ] && say "Everything is in place" || { say "Install what is missing, then run check again"; return 1; }
}

extract() {
    local nsp=${1:-} romfs=0; KEYS=${2:-}
    [ "${3:-}" = --romfs ] && romfs=1
    [ -f "$nsp" ] && [ -f "$KEYS" ] || die "usage: nx2apk.sh extract <game.nsp> <prod.keys> [--romfs]"
    [ -x "$HACTOOL" ] || die "hactool not found (./nx2apk.sh check)"
    mkdir -p "$WORK"; rm -rf "$WORK/pfs" "$WORK/exefs" "$WORK/control"
    # hactool stops at any key that is not 32 or 64 hex digits (some key dumps have such entries): a filtered copy, deleted on exit. hac() output is never shown,
    # as hactool's errors can print key values
    local keys="$WORK/.prod.keys"; trap 'rm -f "$WORK/.prod.keys"' EXIT INT TERM
    (umask 077; grep -E '^[[:space:]]*[a-z0-9_]+[[:space:]]*=[[:space:]]*([0-9a-fA-F]{32}|[0-9a-fA-F]{64})[[:space:]]*$' "$KEYS" > "$keys") || die "no usable keys in $KEYS"
    KEYS=$keys
    say "Unpacking the NSP"
    hac -t pfs0 --outdir="$WORK/pfs" "$nsp" >/dev/null
    local prog="" ctrl="" f t
    for f in "$WORK"/pfs/*.nca; do   # each NCA's content type from its header
        t=$(hac -t nca "$f" | sed -n 's/^Content Type: *//p' | head -1)
        case "$t" in Program) prog=$(basename "$f" .nca) ;; Control) ctrl=$(basename "$f" .nca) ;; esac
    done
    [ -n "$prog" ] || die "no game code found: check that this is the base game's .nsp (not an update, DLC, .xci or .nsz) and that prod.keys is from your console"
    say "Code (ExeFS)"
    hac -t nca --exefsdir="$WORK/exefs" "$WORK/pfs/$prog.nca" >/dev/null
    [ -f "$WORK/exefs/main" ] || die "could not decrypt the game code. If the game uses title key crypto, put your title.keys in ~/.switch/title.keys and try again"
    hac -t nca "$WORK/pfs/$prog.nca" | sed -n 's/^Title ID: *//p' | head -1 | tr 'a-f' 'A-F' > "$WORK/title_id"
    printf 'title id %s\n' "$(cat "$WORK/title_id")"
    if [ -n "$ctrl" ]; then
        say "Icon"
        hac -t nca --romfsdir="$WORK/control" "$WORK/pfs/$ctrl.nca" >/dev/null
        local icon="$WORK/control/icon_AmericanEnglish.dat"; [ -f "$icon" ] || icon=$(ls "$WORK"/control/icon_*.dat 2>/dev/null | head -1 || true)
        if [ -n "$icon" ] && command -v sips >/dev/null; then sips -s format png "$icon" --out "$WORK/icon.png" >/dev/null && echo "icon.png"
        elif [ -n "$icon" ] && command -v magick >/dev/null; then magick "$icon" "$WORK/icon.png" && echo "icon.png"
        else echo "no icon (needs macOS sips or ImageMagick); the app gets the default one"; fi
    fi
    if [ $romfs = 1 ]; then
        say "Game files (RomFS): this is the size of the game"
        rm -rf "$WORK/romfs"
        hac -t nca --romfsdir="$WORK/romfs" "$WORK/pfs/$prog.nca" >/dev/null
    fi
    rm -rf "$WORK/pfs"   # the game-sized NCAs are not needed again
    say "Extracted into $WORK"
}

build() {
    [ -f "$WORK/exefs/main" ] || die "run extract first"
    local tid; tid=$(cat "$WORK/title_id")
    [ -f "$HERE/dm/games/$tid/game.tsv" ] || die "title $tid has no game profile in dm/games/ (see README: supported games)"
    rm -f "$WORK/game.apk"
    say "Rewriting the game code for Android"
    rm -rf "$WORK/mods"; mkdir -p "$WORK/mods"
    for m in main sdk subsdk0 subsdk1 subsdk2 subsdk3; do
        [ -f "$WORK/exefs/$m" ] || continue
        (cd "$HERE/dm/tools" && DM_TARGET=android "$PY" patch_nso.py "$WORK/exefs/$m" "$WORK/mods" "$m" | tail -1)
    done
    say "Finding the game's shaders"
    rm -rf "$WORK/shpkg" "$WORK/emptyromfs"; mkdir -p "$WORK/shpkg" "$WORK/emptyromfs"
    DM_GAME=$tid "$PY" "$HERE/dm/gfx/tools/shader_split.py" "$WORK/emptyromfs" "$WORK/exefs/main" "$WORK/shpkg" \
        "$WORK/shpkg/shader_map.tsv" "$WORK/shpkg/shader_facts.tsv" "$WORK/shpkg/shader_pairs.tsv" | tail -2
    say "Translating shaders to Vulkan"
    local glslc; glslc=$(ls "$NDK"/shader-tools/*/glslc 2>/dev/null | head -1 || true); [ -x "$glslc" ] || die "glslc not found in the NDK ($NDK)"
    GLSLC=$glslc SPIRV_CROSS=$(command -v "${SPIRV_CROSS:-spirv-cross}") "$PY" "$HERE/dm/android/tools/vk_sh_build.py" "$WORK/shpkg" "$WORK/vksh" | tail -2
    say "Building the APK"
    mkdir -p "$HERE/dm/android/build"
    rm -f "$HERE/dm/android/build/icon.png"; if [ -f "$WORK/icon.png" ]; then cp "$WORK/icon.png" "$HERE/dm/android/build/icon.png"; fi
    GAME=$tid ANDROID_SDK=$SDK bash "$HERE/dm/android/build_apk.sh" | tail -1
    cp "$HERE/dm/android/build/dmc.apk" "$WORK/game.apk"
    say "Built $WORK/game.apk"
}

install() {
    local adb; adb=$(command -v adb || echo "$SDK/platform-tools/adb")
    [ -f "$WORK/game.apk" ] || die "run build first"
    "$adb" get-state >/dev/null 2>&1 || die "no phone found (or more than one: set ANDROID_SERIAL): plug it in with USB debugging on and accept the prompt"
    say "Installing the app"
    "$adb" install -r -g "$WORK/game.apk" | tail -1
    "$adb" shell appops set "$PKG" MANAGE_EXTERNAL_STORAGE allow
    say "Copying the rewritten code and shaders"
    "$adb" shell mkdir -p "$APPDIR/mods" "$APPDIR/vksh"
    "$adb" push "$WORK/mods/." "$APPDIR/mods/" | tail -1
    "$adb" push "$WORK"/vksh/*.spv "$WORK/vksh/vk_shaders.tsv" "$APPDIR/vksh/" | tail -1
    if [ -d "$WORK/romfs" ]; then
        say "Copying the game files (this takes a while)"
        "$adb" shell mkdir -p "$ROMFS_DEV"
        "$adb" push "$WORK/romfs/." "$ROMFS_DEV/" | tail -1
    elif ! "$adb" shell "[ -d $ROMFS_DEV ]"; then
        echo "The game files are not on the phone yet: run extract with --romfs and install again, or copy your RomFS to $ROMFS_DEV yourself"
    fi
    say "Done: open the app on the phone"
}

case "${1:-}" in
    check) check ;;
    extract) shift; extract "$@" ;;
    build) build ;;
    install) install ;;
    *) sed -n '2,7p' "$0" | sed 's/^# \{0,1\}//'; exit 1 ;;
esac
