# Linux and Android

Where the port stands, and what is known not to be proven yet. How to build: [BUILDING.md](../BUILDING.md).

## How it is put together

* **SDK**: `tools/setup_sdk.sh` makes `third_party/rexglue-sdk` from ReXGlue v0.10.0, SDL 3.4.14 and
  `sdk-patches/linux-android`. Those patches are for Linux and Android only: Windows builds keep the SDK BUILDING.md
  describes. They hold three things:
  * the ARM64, Android and Vulkan work of [Skate1Native](https://github.com/Kurtisone/Skate1Native), merged with
    `sdk-patches/0001` to `0021`;
  * the renderer fixes that existed only on D3D12, ported to the SPIR-V translator (below);
  * the Android app folder's name as a build setting, and `generated/rexglue.cmake` keeping the underscores of a
    project's name (the games' names have one; Skate's has none).
* **Renderer**: the SDK's Vulkan backend, `render_target_path_vulkan` as in each game's `port_defaults.cpp`.
* **Linux**: the launcher and the three games in one folder, built in a container so that it runs on the Steam Deck.
* **Android**: one APK per game. The launcher stays a desktop program.

## D3D12 fixes on Vulkan

| Patch | Fix | On Vulkan |
| --- | --- | --- |
| 0001, 0009 | Integer number format fetches, rounded only without an exponent bias | `spirv_translator_fetch.cpp` |
| 0002 | Unnormalized coordinates at the fetched mip level | `spirv_translator_fetch.cpp` |
| 0002, 0005, 0011 | Integer destinations and the full -32 to 32 range in 16 and 32 bpp resolves | XeSL sources compiled to SPIR-V, interface checked equal to the stock shaders |
| 0003 | Memory export at an even resolution scale | `spirv_translator.cpp` |
| 0004 | getCompTexLOD reports the console's LOD | `spirv_translator_fetch.cpp` |
| 0011 | k_16_16 render targets keep values below -1 | host render targets and fragment shader interlock |
| 0013 | `ps_position_guest_pixels` | `spirv_translator.cpp` |

Two additions, both from [snufflyyy/trials-hd-recompiled](https://github.com/snufflyyy/trials-hd-recompiled) (MIT):

* the SPIR-V translator read the texture exponent bias from fetch constant word 4; it is in word 3;
* Trials HD sets the integer flag on its 8 bit font textures, which must stay normalized, so
  `texture_fetch_integer_8bpc` is off for that game (the D3D12 path scales them).

That project's Vulkan fast paths for Trials HD (`vulkan_trials_fast_path`) are not in: they are a large performance
change that conflicts with the Android work. Look there first if Trials HD is too slow.

## What has been checked

* The SDK builds for Linux x86-64 and Android arm64.
* **Trials Fusion** (the only package tried so far) goes through `build-linux.sh` from its package to a Linux binary, and
  runs on an AMD GPU: title screen, menus and the first track, riding included. Textures, shadows and the HUD are there.
  The integer fetch and resolve ports are needed and work: with either one switched off, surfaces lose their textures.
* Skate runs on the same SDK with the same picture as before.
* All three games' own code compiles for both platforms.
* The launcher runs on Linux. Trials Fusion builds into a signed arm64 APK.
* That APK runs on an AYN Odin 2 Portal (Adreno 740) with the system's Qualcomm driver: picture and sound, 84 FPS on the
  title screen.

## What has not

* Trials HD and Trials Evolution have not been built.
* On Android, tracks since the uploads were fixed: the frame rate in play, and whether memory now holds.
* Starting a game from the launcher, and installing a package with it. Sound (the test machine has no audio).
* Scores above 720p: all of the above is at 720p.

## Known bug: Trials Fusion's picture is green

In tracks everything leans green (beige turns olive, the sky cyan); menus are fine. Same picture with `fbo` and `fsi`.

The last step of the frame looks the colour up in a 32x32x16 table indexed by chromaticity and luminance. The game draws
that table every so often from a 16x16x16 colour grade (pixel shader `3EBCA95DEB27B0CD`; the final pass is
`BD6D432E414F4E49`). Bypassing the read of the grade in that shader makes the picture nearly neutral, so the fault is in
what that pass feeds the grade, or in the grade's result. The grade's blue is steep (about 36 at Z 4, 155 at Z 8, 255 at Z
12), so inputs a little too low drop the blue and look like this.

Ruled out, each by a run: the 8 bit integer scale, integer fetches, the exponent bias word, the stock resolve shaders,
`readback_resolve = full`, the 3D-as-2D wrappers, the 3D texture's load and sampling (fixed lookups at Z 0 and Z 8 give the
table's colours), the pixel position and `log` / `exp` (same as DXBC, by reading). Next: compare that pass's float
constants with a Windows run; c251 to c255 hold its matrix and gamma.

## Known bug: black picture with Turnip on Android

With the custom Turnip driver from Skate's `driver/` folder (Mesa, API 1.4.328) Trials Fusion plays sound and presents
about 60 frames a second, all black, with the same draws as on PC. Without a `driver/` folder it shows. Cause unknown; it
is not FXAA, `present_upscale_resolution`, the asynchronous pipelines or the Adreno settings in `android-overrides.toml`.

## Frame rate on the Odin 2 Portal

The Qualcomm driver allocates kernel memory for each `vkCmdCopyBufferToImage` into a compressed image. Trials Fusion
reloads its 2048x2048 virtual texture table all the time, with 8 mip levels, and each frame took 170 ms (5 FPS).
`vulkan_uncompressed_textures` creates textures mutable, which turns that compression off: the title screen went to 84
FPS (12 ms a frame). `vulkan_storage_image_texture_loads` also writes the front buffer and movie planes without a copy.

On the first track it runs at about 22 FPS, limited by the GPU thread (45 to 50 ms a frame), and no longer runs out of
memory with `gpu_force_msaa_1x`.

## Tracks on the Odin 2 Portal

The first track renders at about 23 FPS, limited by the GPU thread (40 to 45 ms a frame, 2,500 to 3,400 draws).

Tracks used to be black behind the HUD. With the Qualcomm driver, the float control execution modes
(`DenormFlushToZero`, `SignedZeroInfNanPreserve`, `RoundingModeRTE`) make `gl_FragCoord.xy` read as 0 in pixel shaders,
while Z stays right. Trials Fusion draws its colour table from the pixel position (shader `3EBCA95DEB27B0CD`), so the table
was black. `vulkan_shader_float_controls = false` fixes it. Which of the three modes is at fault has not been narrowed down.

The picture on the Odin has the same green tint as on Linux (bug above).

Turnip: v25.3.0-R11 and v26.0.0-R8 stay black. MrPurple T30 (26.3.0, Vulkan 1.4.359) renders, at 21 FPS on the first track
(47 ms a frame). Its timestamps add up to 23 ms and its GPU thread reports 22, but both leave out binning: the thread sleeps
36 ms a frame on the fence that limits frames in flight, so the GPU is the limit there too, as with the Qualcomm driver. Two
of its runs ended early with no crash report.

A track can be reached without touching the device: `mnk_mode = true`, `keybind_a = 'Space'`, `keybind_start = 'Return'`,
then `adb shell input keyevent --longpress` with 66 (Return) and 62 (Space).

## Expected trouble

* **Trials Evolution on Android**: its title screen and far shadows need render targets emulated exactly. Linux uses
  fragment shader interlock for that; Adreno has none, so host render targets are used, and the "Press START" screen may
  stay black (as it did on D3D12 without ROV).
* **Android performance**: Evolution and Fusion stream virtual textures through memory export and CPU readbacks, costly
  on phone GPUs. They may not reach a playable speed at 720p.
* **Android controls**: a controller is required.
* **Above 720p on Evolution and Fusion**: as on Windows, 720p is the default and the launcher warns about more.
* **Linux packaging**: a folder, not yet the AppImage the launcher's updater expects, so no self-update.
