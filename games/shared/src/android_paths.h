// Android: where a game keeps everything. The app root is <storage>/<game> when that holds game/default.xex, else the
// app's external files folder (the SDK's FindAppRoot, tools/android/README.md).

#pragma once

#if defined(__ANDROID__)

#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/rex_app.h>

namespace trials {

inline void ConfigureAndroidPaths(rex::PathConfig& paths, std::string_view name) {
  const std::filesystem::path root = rex::filesystem::GetAppRootFolder();
  if (paths.game_data_root.empty() || paths.game_data_root.is_relative()) paths.game_data_root = root / "game";
  paths.user_data_root = root / "userdata";
  paths.cache_root = paths.user_data_root / "cache";
  paths.config_path = root / (std::string(name) + ".toml");
  std::error_code ec;
  std::filesystem::create_directories(root / "logs", ec);
  if (rex::cvar::GetFlagByName("log_file").empty())
    rex::cvar::SetFlagByName("log_file", (root / "logs" / (std::string(name) + ".log")).string());
}

}  // namespace trials

#endif  // __ANDROID__
