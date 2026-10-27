#!/bin/bash
# The app: build/libdm.so (build.sh APK=1) + apk/AndroidManifest.xml → build/dmc.apk, signed with the debug key.
# Install and first run:
#   adb install -r dm/android/build/dmc.apk
#   adb shell appops set dev.nxandroid.dmc MANAGE_EXTERNAL_STORAGE allow     (the RomFS under /sdcard/nx/dmc/romfs)
#   adb push <rewritten modules>/. /sdcard/Android/data/dev.nxandroid.dmc/files/mods/
#   adb push <vk_sh_build.py output>/. /sdcard/Android/data/dev.nxandroid.dmc/files/vksh/
# App icon: build/icon.png when present (game data, kept out of git): the game's own icon from its control NCA (icon_AmericanEnglish.dat, a 256x256 JPEG;
#   sips -s format png icon_AmericanEnglish.dat --out build/icon.png). build/icon_cropped.png, when present, is used instead (the same art cropped inward,
#   e.g. to Dante, so the door frame the square crop cuts at the left and right is out of the picture). The adaptive icon insets the art 16.7% of the
#   layer's 1.5x bounds, which is the visible tile: full art, only the mask's corners clipped
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
export JAVA_HOME=${JAVA_HOME:-/opt/homebrew/opt/openjdk@17}; export PATH="$JAVA_HOME/bin:$PATH"
SDK=${ANDROID_SDK:-/opt/homebrew/share/android-commandlinetools}; BT=$SDK/build-tools/34.0.0
GAME=${GAME:-0100E8000D5B8000} APK=1 bash "$HERE/build.sh"
W=$HERE/build/apk; rm -rf "$W"; mkdir -p "$W/lib/arm64-v8a"
cp "$HERE/build/libdm.so" "$W/lib/arm64-v8a/"
MAN=$HERE/apk/AndroidManifest.xml; RES=()
R=$W/res; mkdir -p "$R"
[ -d "$HERE/apk/res" ] && cp -R "$HERE/apk/res/." "$R/"   # the app's own resources (the full-screen theme with the camera cutout)
if [ -f "$HERE/build/icon.png" ] || [ -f "$HERE/build/icon_cropped.png" ]; then
    mkdir -p "$R/mipmap-xxxhdpi" "$R/drawable-nodpi" "$R/drawable" "$R/mipmap-anydpi-v26" "$R/values"
    ART=$HERE/build/icon_cropped.png; [ -f "$ART" ] || ART=$HERE/build/icon.png
    cp "$ART" "$R/mipmap-xxxhdpi/ic_launcher.png"; cp "$ART" "$R/drawable-nodpi/ic_art.png"
    printf '<resources><color name="icon_bg">#391219</color></resources>\n' > "$R/values/colors.xml"
    cat > "$R/drawable/ic_foreground.xml" <<'X'
<inset xmlns:android="http://schemas.android.com/apk/res/android"
    android:drawable="@drawable/ic_art" android:inset="16.7%" />
X
    cat > "$R/mipmap-anydpi-v26/ic_launcher.xml" <<'X'
<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">
    <background android:drawable="@color/icon_bg" />
    <foreground android:drawable="@drawable/ic_foreground" />
</adaptive-icon>
X
    sed 's|<application |<application android:icon="@mipmap/ic_launcher" |' "$HERE/apk/AndroidManifest.xml" > "$W/AndroidManifest.xml"; MAN=$W/AndroidManifest.xml
fi
if [ -d "$HERE/apk/res" ] || [ -f "$HERE/build/icon.png" ] || [ -f "$HERE/build/icon_cropped.png" ]; then
    "$BT/aapt2" compile --dir "$R" -o "$W/res.zip"; RES=(-R "$W/res.zip" --auto-add-overlay)
fi
# Optional data bundle (BUNDLE=1 WORK=<dir>): packs <dir>/{mods,vksh,texpatch} as APK assets; activity.c extracts
# them into internal storage on first run, so a personal reinstall is one APK plus the RomFS. Personal installs
# only — the mods are the game's own rewritten code, a bundled APK must never be what a public download serves.
ASSETS=(); OUT=${OUT:-dmc.apk}
if [ "${BUNDLE:-0}" = 1 ] && [ -d "${WORK:-}/mods" ]; then
    A=$W/assets/bundle; mkdir -p "$A/mods"
    cp -R "$WORK/mods/." "$A/mods/"
    [ -d "$WORK/vksh" ] && { mkdir -p "$A/vksh"; find "$WORK/vksh" -maxdepth 1 -type f -exec cp -R {} "$A/vksh/" \; ; }
    [ -d "$WORK/texpatch" ] && { mkdir -p "$A/texpatch"; cp -R "$WORK/texpatch/." "$A/texpatch/"; }
    date +%s > "$A/stamp"; ASSETS=(-A "$W/assets"); OUT=${OUT:-dmc-bundled.apk}
fi
"$BT/aapt2" link -o "$W/base.apk" --manifest "$MAN" -I "$SDK/platforms/android-34/android.jar" ${ASSETS[@]+"${ASSETS[@]}"} "${RES[@]}"
(cd "$W" && zip -q -r base.apk lib)
"$BT/zipalign" -f -p 4 "$W/base.apk" "$W/aligned.apk"
"$BT/apksigner" sign --ks "$HOME/.android/debug.keystore" --ks-pass pass:android --key-pass pass:android --out "$HERE/build/$OUT" "$W/aligned.apk"
echo "built $HERE/build/$OUT"
