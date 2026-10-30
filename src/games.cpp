#include "games.h"

#include <cstdlib>
#include <filesystem>
#include <string_view>

#include <trg/updater.h>

#if defined(_WIN32)
#include <windows.h>
#include <shlobj.h>
#endif

namespace trials {
namespace fs = std::filesystem;

namespace {

std::string Utf8(const fs::path& p) {
  auto s = p.generic_u8string();
  return std::string(s.begin(), s.end());
}

fs::path PathOf(const std::string& utf8) { return fs::u8path(utf8); }

}  // namespace

const std::array<GameInfo, kGameCount>& Games() {
  static const std::array<GameInfo, kGameCount> games = {{
      {"trials_hd", "Trials HD", "trials_hd.exe", kCollectionRepo, "TrialsHD", 0x5841095A, 5279176,
       ImVec4(0.86f, 0.46f, 0.14f, 1.0f), "Where it all began: one bike, one rider, physics.", "0.0.1.2",
       "217,092,096 bytes", "CAD16E3104B3C584773E1A3F813DF923642CBA94", "about 210 MB", "22E417BF",
       "E9FAE8E7B723DF67383EB418C53841C5", "1600C80D", "6E133151FC02CFA6D6653C9AAE46D1DC8014605158"},
      {"trials_evolution", "Trials Evolution", "trials_evolution.exe", kCollectionRepo, "TrialsEvolution",
       0x58410AAD, 5499128, ImVec4(0.95f, 0.68f, 0.16f, 1.0f), "The tracks leave the warehouse.", "0.0.0.3",
       "1,316,257,792 bytes", "6BD8FBCDFC45911F1317D462DC24440C1F277BA1", "about 1.3 GB", "739182AD",
       "FBF4A7E527647AA52F193143A6C03920", "278D66E8", "FB397172C4B4BDBE075DF758DAE196759A388B6958"},
      {"trials_fusion", "Trials Fusion", "trials_fusion.exe", kCollectionRepo, "TrialsFusion", 0x58411401, 2877,
       ImVec4(0.16f, 0.56f, 0.95f, 1.0f), "Tricks and tracks in a bright future.", "0.0.0.3",
       "1,632,141,312 bytes", "5695B9CB78B1A1925043874DAA83BBF7564CE2AA", "about 1.6 GB", "5AF34C9D",
       "CC034DBBECED49679B9447B247496E87", "0C62B4D4", "B2F2C57C1F509A73DAF402B29BDA3FBA6B989AD258"},
  }};
  return games;
}

std::string ProgramDir() {
  static const std::string dir = [] {
#if defined(_WIN32)
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
    if (n > 0 && n < std::size(buf)) return Utf8(fs::path(buf).parent_path());
#else
    std::error_code ec;
    const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) return Utf8(exe.parent_path());
#endif
    return Utf8(fs::current_path());
  }();
  return dir;
}

#if defined(_WIN32)
std::string LauncherDir() { return ProgramDir(); }
#else
namespace {
// XDG_DATA_HOME, or ~/.local/share: where the SDK keeps each game's user folder too.
std::string DataHome() {
  if (const char* x = getenv("XDG_DATA_HOME"); x && *x) return x;
  return std::string(getenv("HOME") ? getenv("HOME") : ".") + "/.local/share";
}
}  // namespace

// On Linux the launcher and the games are inside the read only AppImage: everything written goes to the user's
// data folder.
std::string LauncherDir() {
  static const std::string dir = DataHome() + "/trials_collection";
  return dir;
}
#endif

std::string GameDir(const GameInfo& game) { return LauncherDir() + "/games/" + game.id; }
std::string GameDataDir(const GameInfo& game) { return GameDir(game) + "/game"; }
#if defined(_WIN32)
std::string GameExePath(const GameInfo& game) { return GameDir(game) + "/" + game.exe; }
std::string GameSettingsPath(const GameInfo& game) { return GameDir(game) + "/" + game.id + ".toml"; }
#else
// The games come inside the AppImage, beside the launcher, without ".exe"; each reads its settings from its user
// folder (the games' OnConfigurePaths).
std::string GameExePath(const GameInfo& game) {
  std::string exe = game.exe;
  if (exe.size() > 4 && exe.compare(exe.size() - 4, 4, ".exe") == 0) exe.resize(exe.size() - 4);
  return ProgramDir() + "/" + exe;
}
std::string GameSettingsPath(const GameInfo& game) { return SavesDir(game) + "/" + game.id + ".toml"; }
#endif
std::string BackupsDir(const GameInfo& game) { return LauncherDir() + "/save-backups/" + game.id; }
std::string CacheDir() { return LauncherDir() + "/cache"; }

std::string SavesDir(const GameInfo& game) {
  // Testing: TRIALS_TEST_DOCUMENTS stands in for Documents (the DLC install test).
  if (const char* test = std::getenv("TRIALS_TEST_DOCUMENTS"); test && *test) return std::string(test) + "/" + game.id;
#if defined(_WIN32)
  PWSTR docs = nullptr;
  std::string out;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &docs))) {
    out = Utf8(fs::path(docs) / game.id);
  }
  CoTaskMemFree(docs);
  return out;
#else
  return DataHome() + "/" + game.id;  // the SDK's user folder for the game
#endif
}

bool HasIssuesAbove720p(const GameInfo& game) {
  const std::string_view id = game.id;
  return id == "trials_evolution" || id == "trials_fusion";
}

GameState DetectState(const GameInfo& game) {
  std::error_code ec;
  if (!fs::exists(PathOf(GameExePath(game)), ec)) return GameState::kNotDownloaded;
  if (!fs::exists(PathOf(GameDataDir(game) + "/default.xex"), ec)) return GameState::kDownloaded;
  return GameState::kInstalled;
}

std::string InstalledVersion(const GameInfo& game) {
#if defined(_WIN32)
  auto manifest = trg::update::ReadManifest(GameDir(game) + "/update-files.txt");
  return manifest ? manifest->version : std::string();
#else
  (void)game;
  return TRIALS_VERSION;  // the games come inside the launcher's AppImage and update with it
#endif
}

}  // namespace trials
