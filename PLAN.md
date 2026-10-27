# nx-android: the Switch build of Devil May Cry, run natively on Android

## Why

The Switch release of Devil May Cry (title 0100E8000D5B8000, SDK 6.4.2) is Capcom's own AArch64
port of the PS2 game. Its build path is `BarashiDMC1`. It reads the same PS2 data files as the disc
(`pl00.pld`, `em0e.emd`, `r11d.fsd`, ...) with a `.prd` suffix, and it renders through NVN. Android
phones are AArch64 too, so that code can run on the phone's CPU unchanged, the same way
[nxnative](https://github.com/seedds/nxnative) (MIT) runs Switch games on iPhone and Mac. That
replaces the MIPS-to-C recompile and the PS2 GS/VU emulation of the PS2 port with Capcom's shipped
native code. The Switch version runs at 60 fps on a Tegra X1, which is a weaker CPU than the S10e's.

## How (nxnative's model, retargeted)

| Part | nxnative (Apple) | nx-android |
| --- | --- | --- |
| Module rewriter (`patch_nso.py`) | x18, tpidr_el0, tpidrro_el0, svc and cntpct become host slots and gates; slot block = `tpidrro_el0 + 8*key` (Apple TSD) | same rewrite; slot block pointer kept in bionic's `TLS_SLOT_APP` (`tpidr_el0[2]`), one extra load |
| Host (Horizon stand-in, `dm/host`) | C plus Mach calls | same C; the Mach calls replaced by Linux (futex, clock_gettime, signals) |
| Graphics | NVN recorded, replayed on Metal (`nvn_mtl.m`) | NVN recorded with the same recorder, replayed on Vulkan 1.1 (Mali-G76) |
| Shaders | Maxwell BNSH to SPIR-V (Ryujinx translator) to MSL | Maxwell BNSH to SPIR-V, used directly |
| Textures | BC sampled natively (A17 and newer) | Mali has no BC: decode at upload |
| Frontend | Cocoa / UIKit | NativeActivity, AAudio, game controller and touch input |

Checked on the S10e (`probe/abiprobe.c`): an app can write code into anonymous memory and run it,
and `tpidr_el0[2]` is zero and private to each thread across syscalls. x18 holds bionic's shadow call
stack, so the game's x18 use must be rewritten, as on Apple.

DMC calls 181 of the 512 NVN functions it loads (`loader_census.py`), so the Vulkan replay only needs
that subset.

## Status (2026-10-26)

- Milestone 1 (CPU bring-up) done 2026-10-25.
- Milestone 2 (Vulkan replay) done: `dm/android/nvn_vk.c` replays DMC's NVN subset on the Mali-G76; the 81 shaders in `main` are translated
  by `dm/android/tools/vk_sh_build.py`.
- Milestone 3 (app) done: `dm/android/build_apk.sh` builds a NativeActivity APK (window, controller and touch input, AAudio sound). Movies play with
  picture (MediaCodec H.264; the decoder reports `nv12-colorspace`, without which DMC never binds its NV12 to RGB shader) and sound (MediaCodec AAC,
  `dm/android/aconv_mc.c`).
- Performance: the app runs in the Switch's handheld mode (720p, `DM_OPMODE=1` in env.txt for docked). Through the opening cutscenes and Dante's office
  it holds 59.5 to 60 fps with 0 to 4 dropped frames per 300, including the heaviest scene (58 render passes) with Samsung's thermal policy capping the GPU
  at 377 MHz. What got it there:
  - the replay runs on its own thread (the game's submit thread only queues; it spent 9 to 12 ms a frame replaying before);
  - uniform blocks are copied only as far as each shader reads (ring copies 7 MB to 3 MB a frame);
  - textures the game rewrites every frame (the movie planes) stop being write-protected (page faults were 12% of the CPU in movies);
  - images are created without MUTABLE_FORMAT, so the Mali driver compresses them (about 20% fewer GPU cycles);
  - `-O2`.
  Docked mode (1600x900 scene) still drops to about 48 fps in the heaviest scene: GPU-bound.

- Native screen shape (2026-10-27): the game sizes its window, 3D render target and crop from the display resolution the system reports, so the app now
  reports the panel's own shape (`display_default` in activity.c, from the surface at 720 lines, 1080 with `DM_OPMODE=1`) and the loader rewrites DMC's two
  baked resolution pairs to it (`dm_resolution_pair_cut`), which makes the scene render 1:1 at the panel and is lighter on the GPU. The camera projection is
  the one piece the game keeps at 16:9 whatever the target (measured: sx/sy = 1.7778 for every camera; it is not the f32 16:9 constant in main, not the two
  pairs, and not any other {1600,900} pair in the image), so the replay layer scales the projection's horizontal row to the panel's aspect
  (`proj_aspect_fix` in nvn_vk.c, `DM_ASPECT=off` to leave it). 3D scenes now show more at the sides instead of stretching 19% on a 19.5:9 panel.
  Video (splash screens, the movie plane) is still drawn edge to edge by the game and keeps that stretch; the menu layer is aspect-aware.

Open:
- Objects a command points to (programs, render-target textures) are read when the replay thread reaches it, up to 3 jobs after the submit; fine for DMC
  (it sets them once), see the replay thread comment in nvn_vk.c.
- Window origin lower left (set at boot before the game switches to upper left) is not implemented; harmless so far.
- The movie plane is the last stretched surface. DMC uploads the decoded frame into a texture some way the host does not see (not `nvnTextureWriteTexels`
  with the decoder's buffer, not its frame size, and not native-texture mode), so the replay layer cannot single it out; the full-face quad it draws
  would otherwise be pillarboxed to the video's shape.

## Milestones

1. **CPU bring-up**: the rewritten modules (`main`, `sdk`, `subsdk0`) load in the Android host with a
   null NVN device, reach the game's main loop and read RomFS files from device storage.
2. **Vulkan replay**: DMC's NVN subset on Vulkan, title screen visible.
3. **Gameplay**: input, audio (audren to AAudio), movies (the `.mp4` cutscenes), save data.
4. **Comparison**: frame time and image against the PS2 recompile and ARMSX2 on the same scenes.

## Data

The NSP is 9.9 GB. RomFS is extracted straight to device storage (the Mac has 12 GB free).
