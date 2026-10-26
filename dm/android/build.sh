#!/bin/bash
# Builds the host for Android (arm64) with the NDK: dm/android/build/dmhost, a command-line executable for bring-up over adb shell.
# Usage: GAME=<title ID> dm/android/build.sh     (DMC: GAME=0100E8000D5B8000)
#        APK=1 builds build/libdm.so (the NativeActivity app, activity.c) instead; build_apk.sh packs it
set -euo pipefail
: "${GAME:?set GAME to a game profile title ID}"
HERE=$(cd "$(dirname "$0")" && pwd); DM=$(dirname "$HERE")
NDK=${NDK:-/opt/homebrew/share/android-commandlinetools/ndk/28.2.13676358}
CC="$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android29-clang"
OUT=$HERE/build; mkdir -p "$OUT/obj" "$OUT/vpx"
python3 "$DM/games/game.py" header "$GAME" "$DM/host/gen/$GAME/game_profile.h"
for s in host/entry.S gfx/nvn/nvn_entry.S gfx/nvn/nvn_thunks.S; do python3 "$HERE/elfasm.py" "$DM/$s" "$HERE/gen/$(basename "$s")"; done
touch -r "$HERE/elfasm.py" "$HERE/gen" 2>/dev/null || true

CFLAGS=(-O2 -g -fvisibility=hidden -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable -march=armv8.2-a -fPIC
        -I"$HERE/shim" -include "$HERE/shim/darwin_compat.h" -I"$DM/host" -I"$DM/host/gen/$GAME"
        -I"$DM/gfx/nvn/include" -I"$DM/third_party/vulkan-headers/include" -I"$DM/third_party/stb_vorbis" -I"$DM/third_party/bcdec" -I"$DM/third_party/libvpx" -I"$DM/third_party/libvpx/config")
# The Apple-only units are replaced: front_mac.m, haptics_apple.m, memstat.m (Cocoa / Core Haptics / Mach), probe.c (Mach watchpoints), hle_movie_avc.c (VideoToolbox),
# nvn_mtl.m and the Metal texture helpers (the graphics backend)
SRC=()
for f in "$DM"/host/main.c "$DM"/host/loader.c "$DM"/host/hle.c "$DM"/host/crash.c "$DM"/host/thread.c "$DM"/host/hle_*.c; do
    case "$(basename "$f")" in hle_movie_avc.c) ;; *) SRC+=("$f") ;; esac
done
SRC+=("$DM"/gfx/hle_nv.c "$DM"/gfx/hle_vi.c "$DM"/gfx/nvn/hle_nvn.c "$DM"/gfx/nvn/nvn_tex.c "$DM"/gfx/nvn/nvn_dirty.c "$DM"/gfx/tex/nvn_deswizzle.c)
SRC+=("$HERE"/compat.c "$HERE"/movie_avc_mc.c "$HERE"/aconv_mc.c "$HERE"/texpatch.c "$HERE"/front_android.c "$HERE"/nvn_${NVN_BACKEND:-vk}.c "$HERE"/host_android.c)
SRC+=("$HERE"/gen/entry.S "$HERE"/gen/nvn_entry.S "$HERE"/gen/nvn_thunks.S)
[ "${APK:-0}" = 1 ] && SRC+=("$HERE"/activity.c)

OBJS=()
build() {   # compile one file when it is newer than its object
    local f=$1 o=$2; shift 2
    if [ ! -f "$o" ] || [ "$f" -nt "$o" ] || [ "$HERE/shim/darwin_compat.h" -nt "$o" ] || [ "$DM/host/plat.h" -nt "$o" ]; then
        "$CC" "$@" -c "$f" -o "$o" || { echo "FAILED: $f"; return 1; }
    fi
}
fail=0
for f in "${SRC[@]}"; do
    o="$OUT/obj/$(echo "${f#$DM/}" | tr / _).o"; OBJS+=("$o")
    case "$f" in
        *.S) build "$f" "$o" -march=armv8.2-a -fPIC || fail=1 ;;
        *) build "$f" "$o" "${CFLAGS[@]}" || fail=1 ;;
    esac
done
# libvpx (VP9 movies, hle_movie.c): its own static library, built once
VOBJS=()
while IFS= read -r f; do
    o="$OUT/vpx/$(echo "${f#$DM/third_party/libvpx/}" | tr / _).o"; VOBJS+=("$o")
    build "$f" "$o" -O2 -march=armv8.2-a -fPIC -I"$DM/third_party/libvpx" -I"$DM/third_party/libvpx/config" -w || fail=1
done < <(find "$DM/third_party/libvpx" -name '*.c' | sort)
[ $fail = 0 ] || exit 1
LIBS=(-llog -laaudio -lmediandk -lvulkan -landroid -lm -ldl -Wl,--no-undefined)
if [ "${APK:-0}" = 1 ]; then
    "$CC" -shared -Wl,-soname,libdm.so -o "$OUT/libdm.so" "${OBJS[@]}" "${VOBJS[@]}" "${LIBS[@]}" -Wl,-z,max-page-size=16384
    echo "built $OUT/libdm.so"
else
    "$CC" -pie -o "$OUT/dmhost" "${OBJS[@]}" "${VOBJS[@]}" "${LIBS[@]}"
    echo "built $OUT/dmhost"
fi
