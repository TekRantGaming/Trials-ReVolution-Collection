# Building the collection

One repo holds the launcher and the three games:

| Path | What |
|---|---|
| `src/`, `CMakeLists.txt`, `build.bat` | The launcher (Dear ImGui, SDL2, OpenGL; the TRG Launcher in `third_party/trg-launcher`; the STFS package reader from Trials HD Recompiled) |
| `games/trials_hd`, `games/trials_evolution`, `games/trials_fusion` | The three ports (ReXGlue projects) |
| `games/shared/src` | Code the three games share: achievement notifications, the way back to the launcher, render quality modes, the game window, the FPE guard, the achievement list export |
| `tools/` | The release script, and the source of the collection's logo |
| `docs/` | README images and the Add to Steam artwork |

## Requirements

* Visual Studio 2022 with the C++ tools (the games build with its LLVM clang), CMake 3.25+, Ninja, Python 3.
* The ReXGlue SDK with TekRantGaming's patches (FSR 1 and NIS among them) plus the renderer fixes in
  `sdk-patches/` (apply them with `git am sdk-patches/*.patch` in the SDK checkout), installed to an install prefix.
  Link it as `tools/rexglue/win-amd64` (a junction to the SDK's `out/install/win-amd64`). The fixes:
  * texture fetches with the integer number format return the stored integers (Evolution and Fusion build their
    virtual textures with them; without it most objects are black or flat);
  * unnormalized texture coordinates with an explicit mip level are texels of that level, and resolves to 16 bit
    formats with an integer number format store integers (the Fusion virtual texture page table needs both);
  * memory export at an even resolution scale reads its interpolants at the guest pixel center (Evolution and
    Fusion copy their texture pages that way; above 1x they were noise), and readback_resolve_scaled_max_kb limits
    readbacks when resolution scaled;
  * getCompTexLOD reports the level of detail the console would at any resolution scale;
  * resolves to 32 bit formats with an integer number format store integers, signed ones in two's complement
    (Evolution reads its occlusion depth map back on the CPU; without it scenery vanishes all over the game);
  * a CPU write to a page the GPU wrote with memory export uploads only the bytes the CPU changed (Evolution
    expands its particles into a vertex buffer the CPU also fills; without it triangles stretch across the screen
    and areas turn black as a track goes on).

## The launcher

```
build.bat
```

Output: `build/trials_launcher.exe`.

## A game

Each game needs code generated from your own copy's `default.xex` (never committed: it is derived from the game):

1. Extract your Xbox Live Arcade package (the one file in `Content\0000000000000000\<title ID>\000D0000`) into
   `games/<game>/assets` (the folder that holds `default.xex`). Any STFS extractor works; the launcher's own reader
   (`src/stfs_package.cpp`) is one.
2. Run the ReXGlue codegen with the game's `<game>_manifest.toml` (it writes `games/<game>/generated/`).
3. Build: `games/<game>/build.bat <short>-release`, where short is `th` (Trials HD), `te` (Trials Evolution) or
   `tf` (Trials Fusion).

Output: `games/<game>/out/build/<short>-release/<game>.exe`.

| Game | Title ID | `default.xex` version |
| --- | --- | --- |
| Trials HD | `5841095A` | 0.0.1.2 |
| Trials Evolution | `58410AAD` | 0.0.0.3 |
| Trials Fusion | `58411401` | 0.0.0.3 |

## Releases

Set `VERSION` to the release's version (the launcher shows it, and every zip's file list carries it), build each
game's release, then:

```
powershell -ExecutionPolicy Bypass -File tools\make_release.ps1 -Version 1.0.0 -Out dist\releases
```

Writes `dist\releases\v1.0.0\` with the players' zip (the launcher with the three games inside), the packages
release (the launcher zip, the three game zips and one `SHA256SUMS.txt`, each zip with its file list for the
updater). Every zip goes through VirusTotal before a release is published.
