# nx2apk

Turn a Nintendo Switch game you own into an Android app that runs the game's own code natively on your phone.

Switch games are compiled for 64-bit ARM, the same kind of processor that is in almost every Android phone. nx2apk
does not emulate the Switch. It rewrites the few instructions that only make sense on a Switch, loads the game's
code straight into an Android app, and swaps the console's services for Android ones:

| On the Switch | On your phone |
| --- | --- |
| The game's ARM64 code | The same code, running directly on the phone's CPU |
| NVN graphics | Vulkan (shaders translated ahead of time) |
| Audio renderer | AAudio |
| Video and AAC movie audio | Android's hardware decoders (MediaCodec) |
| Joy-Con | Touch screen or any Bluetooth / USB controller |

The result is an ordinary APK with the game's icon and name.

## Status

This is an early, working project. **One game runs end to end today**:

| Game | Title ID | State on a Galaxy S10e |
| --- | --- | --- |
| Devil May Cry (2019 Switch release) | `0100E8000D5B8000` | Tested through the opening cutscenes and the first area: 60 fps in the Switch's handheld mode (720p), movies with sound, touch and controller input. Saving and later levels are not tested yet |

Other games are not supported yet. Each new game needs a profile in `dm/games/` and usually some work in the
Vulkan layer for graphics features that game uses (see [Adding a game](#adding-a-game)).

## Before you start

- **You need your own game and keys.** Dump the game (an `.nsp`) and your console's `prod.keys` from your own
  Switch. This repository contains no game files, keys or Nintendo code, and the instructions here do not help
  you get them any other way.
- nx2apk is not affiliated with or endorsed by Nintendo or any game publisher.

## What you need

**A Mac with Apple Silicon** (the shader translator in this repository is a macOS ARM64 build). About as much free
disk space as the game, twice that if you extract the game files on the Mac.

**An Android phone** with a 64-bit ARM CPU, Android 11 or newer and Vulkan 1.1 (most phones from 2018 on), with
enough free storage for the game. Developed and tested on a Samsung Galaxy S10e.

Tools on the Mac, with [Homebrew](https://brew.sh):

```sh
brew install --cask android-commandlinetools
brew install openjdk@17 spirv-cross python
export JAVA_HOME=/opt/homebrew/opt/openjdk@17   # sdkmanager and the APK signer need Java 17
sdkmanager "platform-tools" "build-tools;34.0.0" "platforms;android-34" "ndk;28.2.13676358"
python3 -m venv ~/.nx2apk-venv && ~/.nx2apk-venv/bin/pip install lz4 capstone
export PYTHON=~/.nx2apk-venv/bin/python3
```

APKs are signed with Android's standard debug key. If you have never used Android Studio, create it once:

```sh
mkdir -p ~/.android && keytool -genkey -v -keystore ~/.android/debug.keystore -storepass android \
  -alias androiddebugkey -keypass android -keyalg RSA -validity 10000 -dname CN=Android
```

Two more:

- **hactool**, to unpack your NSP: `git clone https://github.com/SciresM/hactool && cd hactool && cp config.mk.template config.mk && make`,
  then put `hactool` on your `PATH` (or set `HACTOOL=/path/to/hactool`).
- **The .NET 10 runtime**, for the shader translator: install it from [dot.net](https://dotnet.microsoft.com/download),
  or run `curl -sSL https://dot.net/v1/dotnet-install.sh | bash -s -- --runtime dotnet --channel 10.0`
  and set `DOTNET_ROOT=$HOME/.dotnet`.

Then check everything is found (keep `JAVA_HOME`, `PYTHON` and `DOTNET_ROOT` set in the same terminal, or add them
to your shell profile):

```sh
./nx2apk.sh check
```

It lists anything missing and how to install it.

## Convert a game

On the phone, turn on **Developer options** and **USB debugging**, plug it in and accept the prompt.

```sh
# 1. Unpack your game: its code, icon and title ID go into ./work
#    Add --romfs to also extract the game files (needed once, about the size of the game)
./nx2apk.sh extract "Devil May Cry.nsp" prod.keys --romfs

# 2. Rewrite the code for Android, translate the shaders, build the APK (about 30 seconds)
./nx2apk.sh build

# 3. Install the app and copy everything it needs to the phone
./nx2apk.sh install
```

Open **Devil May Cry** on the phone.

`install` puts the files here on the phone:

| What | Where |
| --- | --- |
| The rewritten game code and shaders | `Android/data/dev.nxandroid.dmc/files/` |
| The game files (RomFS) | `/sdcard/nx/dmc/romfs` |
| Saves | the app's private storage (removed if you uninstall the app) |
| A log for troubleshooting | `Android/data/dev.nxandroid.dmc/files/dm.log` |

Your keys never go to the phone, and `extract` deletes its temporary copy of them when it finishes.

## Playing

**Touch.** The left half of the screen is a stick that appears where you put your thumb. The right half has the
four face buttons around a centre point, the top corners are L and R, and the top middle is + (pause).

**Controller.** Any Bluetooth or USB controller Android recognises works. Buttons are mapped **by position**, so
they sit where a Switch controller has them: on an Xbox controller the bottom button (A) is the Switch's bottom
button (B). If you would rather press the button whose letter the game shows, map them **by letter** instead (see
below). In both layouts the bumpers are L / R, the triggers ZL / ZR, Menu is +, View or Back is -, and the sticks
and D-pad work as expected.

**Options.** Create a text file `Android/data/dev.nxandroid.dmc/files/env.txt` on the phone with one setting per
line, then restart the app:

| Setting | Effect |
| --- | --- |
| `DM_PAD_LAYOUT=label` | Controller buttons by letter (Xbox A is Switch A) instead of by position |
| `DM_OPMODE=1` | The Switch's docked mode: sharper (1600x900) but too heavy to hold 60 fps on most phones in busy scenes |

For example, from the Mac: `adb shell 'echo DM_PAD_LAYOUT=label > /sdcard/Android/data/dev.nxandroid.dmc/files/env.txt'`

## Troubleshooting

| Problem | What to do |
| --- | --- |
| `extract` says it could not decrypt the code | The game uses title-key encryption: put your `title.keys` in `~/.switch/title.keys` and run it again |
| `build` says the title has no game profile | That game is not supported yet |
| The app opens and closes, or shows a black screen | Check `dm.log` (path above). If it cannot find the game files, run `extract` with `--romfs` and `install` again |
| The app cannot read the game files | Run `adb shell appops set dev.nxandroid.dmc MANAGE_EXTERNAL_STORAGE allow` (install does this for you) |
| Stutters after long play | Phones slow their GPU when warm. Stay in handheld mode (the default) |

## How it works

1. **Rewrite** (`dm/tools/patch_nso.py`): finds the instructions in the game's modules (`main`, `sdk`, `subsdk*`)
   that touch Switch-only registers or call the Horizon kernel, and redirects each to the host.
2. **Host** (`dm/host`, `dm/android`): a small stand-in for the Switch's operating system services (threads, memory,
   files, time, input, audio, saves). It loads the rewritten modules and starts the game.
3. **Graphics** (`dm/gfx/nvn/hle_nvn.c`, `dm/android/nvn_vk.c`): the game's NVN graphics calls are recorded and
   replayed on Vulkan on a separate thread. Textures are converted as they are uploaded (Mali GPUs have no BC
   texture support).
4. **Shaders** (`dm/gfx/tools/shader_split.py`, `dm/android/tools/vk_sh_build.py`): the game's compiled Switch
   shaders are found in its code, translated to SPIR-V with Ryujinx's shader translator, then to Vulkan GLSL, and
   compiled for the phone.
5. **App** (`dm/android/activity.c`, `dm/android/build_apk.sh`): a NativeActivity APK with no Java code, which hands
   the window, touch and controller input to the host.

`PLAN.md` has the full status, measurements and open issues.

## Adding a game

A new game needs a profile folder `dm/games/<title id>/` (see `dm/games/README.md`) and then testing: run it, read
`dm.log`, and fill in what the game uses that the Android graphics layer does not support yet. `nvn_vk.c` covers
the part of NVN Devil May Cry uses; most other games will need more of it. Contributions are welcome.

## Built with AI

This project was written with substantial help from an AI coding assistant (Claude, by Anthropic, used through
Claude Code). The AI wrote most of the Android port, the Vulkan graphics layer, the tools and this documentation,
under the direction of a person who tested each step on a real phone. Treat it like any young open-source
project: read the code before you rely on it, and please report problems.

## Credits and licenses

nx2apk is a fork of [nxnative](https://github.com/seedds/nxnative) by seedds, which runs Switch games natively on
Apple devices, retargeted to Android. nxnative's MIT license is in `LICENSE.nxnative`; nx2apk's own changes are MIT
too (`LICENSE`).

Bundled third-party code, each with its license in its folder (`dm/third_party/README.md` lists them all):
the Ryujinx shader translator (MIT) and the .NET libraries shipped with its prebuilt build in
`dm/gfx/tools/dmsh_xlate_bin/` (MsgPack, Humanizer, System.CodeDom, under their own licenses, as in upstream nxnative), libvpx (BSD 3-Clause), stb_vorbis (MIT or public domain), bcdec (MIT or
Unlicense), Vulkan-Headers (Apache-2.0 or MIT), NVIDIA FLIP (BSD 3-Clause), deko3d's texture layout code (zlib) and
parts of MacNativeGames (MIT).
