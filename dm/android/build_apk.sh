#!/bin/bash
# The app: build/libdm.so (build.sh APK=1) + apk/AndroidManifest.xml → build/dmc.apk, signed with the debug key.
# Install and first run:
#   adb install -r dm/android/build/dmc.apk
#   adb shell appops set dev.nxandroid.dmc MANAGE_EXTERNAL_STORAGE allow     (the RomFS under /sdcard/nx/dmc/romfs)
#   adb push <rewritten modules>/. /sdcard/Android/data/dev.nxandroid.dmc/files/mods/
#   adb push <vk_sh_build.py output>/. /sdcard/Android/data/dev.nxandroid.dmc/files/vksh/
# App icon: build/icon.png when present (game data, kept out of git): the game's own icon from its control NCA (icon_AmericanEnglish.dat, a 256x256 JPEG;
#   sips -s format png icon_AmericanEnglish.dat --out build/icon.png). An adaptive icon: the picture inset to the part every launcher mask shows, on black
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
export JAVA_HOME=${JAVA_HOME:-/opt/homebrew/opt/openjdk@17}; export PATH="$JAVA_HOME/bin:$PATH"
SDK=${ANDROID_SDK:-/opt/homebrew/share/android-commandlinetools}; BT=$SDK/build-tools/34.0.0
GAME=${GAME:-0100E8000D5B8000} APK=1 bash "$HERE/build.sh"
W=$HERE/build/apk; rm -rf "$W"; mkdir -p "$W/lib/arm64-v8a"
cp "$HERE/build/libdm.so" "$W/lib/arm64-v8a/"
MAN=$HERE/apk/AndroidManifest.xml; RES=()
if [ -f "$HERE/build/icon.png" ]; then
    R=$W/res; mkdir -p "$R/mipmap-xxxhdpi" "$R/drawable-nodpi" "$R/mipmap-anydpi-v26"
    cp "$HERE/build/icon.png" "$R/mipmap-xxxhdpi/ic_launcher.png"; cp "$HERE/build/icon.png" "$R/drawable-nodpi/ic_art.png"
    cat > "$R/mipmap-anydpi-v26/ic_launcher.xml" <<'X'
<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">
    <background android:drawable="@android:color/black" />
    <foreground><inset android:drawable="@drawable/ic_art" android:inset="16.7%" /></foreground>
</adaptive-icon>
X
    "$BT/aapt2" compile --dir "$R" -o "$W/res.zip"; RES=(-R "$W/res.zip" --auto-add-overlay)
    sed 's|<application |<application android:icon="@mipmap/ic_launcher" |' "$HERE/apk/AndroidManifest.xml" > "$W/AndroidManifest.xml"; MAN=$W/AndroidManifest.xml
fi
"$BT/aapt2" link -o "$W/base.apk" --manifest "$MAN" -I "$SDK/platforms/android-34/android.jar" "${RES[@]}"
(cd "$W" && zip -q -r base.apk lib)
"$BT/zipalign" -f -p 4 "$W/base.apk" "$W/aligned.apk"
"$BT/apksigner" sign --ks "$HOME/.android/debug.keystore" --ks-pass pass:android --key-pass pass:android --out "$HERE/build/dmc.apk" "$W/aligned.apk"
echo "built $HERE/build/dmc.apk"
