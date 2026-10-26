// The three games of the collection and where each one lives on disk.
#pragma once

#include <array>
#include <cstdint>
#include <string>

#include <imgui.h>

namespace trials {

// Where the collection is released: one GitHub repo for the launcher and the three games. Each version has two
// releases: vX, the one players see, with only the portable launcher's zip, and packages-vX with what the
// launcher downloads: TrialsCollection-vX-windows-x64.zip (its own update), TrialsHD-, TrialsEvolution- and
// TrialsFusion-vX-windows-x64.zip and SHA256SUMS.txt.
inline constexpr const char* kCollectionRepo = "TekRantGaming/trials-recompilation-collection";
inline constexpr const char* kPackagesTagPrefix = "packages-v";

struct GameInfo {
  const char* id;           // folder and settings name: "trials_hd"
  const char* title;        // "Trials HD"
  const char* exe;          // "trials_hd.exe" (also the ReXGlue app name, so the settings file is trials_hd.toml)
  const char* repo;         // GitHub "owner/name" its releases come from (kCollectionRepo)
  const char* zip_folder;   // the folder inside its release zip, and the zip's name start ("TrialsHD")
  uint32_t title_id;        // the Xbox 360 title ID its package must have
  int steamgriddb_id;       // https://www.steamgriddb.com/game/<id>
  ImVec4 accent;            // the game's colour in the launcher
  const char* tagline;      // one line under the title on the home screen
  // The supported Xbox Live Arcade package, shown on the install page (only the title ID is checked).
  const char* package_version;  // default.xex's version, the one the port's code was made from
  const char* package_size;     // "217,092,096 bytes"
  const char* package_sha1;
  const char* installed_size;
  // More about the package, for the install screen (from the package the port was made with and its default.xex).
  const char* media_id;         // the execution info's media ID
  const char* package_md5;
  const char* package_crc32;
  const char* package_name;     // the package's file name (in Content/<profile>/<title ID>/000D0000)
};

enum class GameState {
  kNotDownloaded,  // no program files yet: the first button says Download
  kDownloaded,     // program files, but no game files from the player's package: Install
  kInstalled,      // ready: Play
};

constexpr int kGameCount = 3;
const std::array<GameInfo, kGameCount>& Games();

// Folders (UTF-8, forward slashes). Everything lives beside the launcher, so the
// collection can be moved or deleted as one folder; saves stay where the games
// keep them (Documents/<id>).
std::string ProgramDir();   // the launcher's own folder (inside the AppImage on Linux)
std::string LauncherDir();  // where it writes: its folder on Windows, XDG_DATA_HOME/trials_collection on Linux
std::string GameDir(const GameInfo& game);       // games/<id>: the program files from the release
std::string GameDataDir(const GameInfo& game);   // games/<id>/game: the files from the player's package
std::string GameExePath(const GameInfo& game);
std::string GameSettingsPath(const GameInfo& game);  // games/<id>/<id>.toml, read by the game at start
std::string SavesDir(const GameInfo& game);      // Documents/<id>
std::string BackupsDir(const GameInfo& game);    // save-backups/<id>
std::string CacheDir();                          // cache (art)

GameState DetectState(const GameInfo& game);
std::string InstalledVersion(const GameInfo& game);  // from update-files.txt; "" when unknown

}  // namespace trials
