# Trials on Android (arm64)

One APK per game. Each game runs on its own: the launcher is a desktop program.

1. Install `<game>.apk`, start it once and allow "All files access", then quit.
2. On a computer, extract your package: `trials_extract <package> <folder>` (built by `build-linux.sh`, in
   `out/launcher/`), or use the folder the Linux launcher installed.
3. Make a folder named after the game at the root of the internal storage or of a microSD card, and put the extracted
   files in `game/` inside it:

       trials_hd/game/default.xex ...
       trials_evolution/game/default.xex ...
       trials_fusion/game/default.xex ...

4. Start it again. Settings are in `<game>/<game>.toml`, logs in `<game>/logs/`, a crash report in `<game>/crash.txt`.

A controller is needed: there are no touch controls.

Custom GPU driver (optional; Trials Fusion stays black with Turnip before 26.3, see `docs/linux_android.md`): unzip a driver package for Adreno (Turnip, or a newer Qualcomm driver, as made for
AdrenoTools: `meta.json` and the driver `.so`) into `<game>/driver/`. The outcome is written to
`<game>/driver_status.txt`. Remove the folder to go back to the system driver.

GPU turbo (optional): `gpu_turbo = true` in the settings keeps the Adreno at its top clock (more heat and battery use).
