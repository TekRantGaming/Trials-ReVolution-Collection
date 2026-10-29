// Downloadable content (dlc.h). A package is a LIVE package with content type 00000002 (marketplace content); its
// files are extracted to <saves>/0000000000000000/<title ID>/00000002/<package file name>, with a header beside it in
// Headers/00000002 (the SDK's XCONTENT_AGGREGATE_DATA) so the game lists it under its own display name, as Xenia's
// "Install content" does. Packages often come zipped; the zip is unpacked first with the system's tar.
//
// Trials Fusion's "Exclusive Content, Uplay Rewards and Season Pass Unlocks" is not a package but a profile's title
// file (58411401.gpd): its title settings 1 to 3 (0x63E83FFF, 0x63E83FFE, 0x63E83FFD, 1000 bytes each, which the SDK
// keeps as <saves>/58411401/profile/User/<setting>) hold the unlock flags. Copying them over would lose the player's
// own options and progress, so only the bytes the file sets to 0xFF (the unlock masks) are set; the player's files
// are backed up first, and put back by Remove.

#include "dlc.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string_view>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "stfs_package.h"

namespace trials {
namespace fs = std::filesystem;
namespace {

constexpr uint32_t kMarketplaceContent = 0x00000002;
constexpr uint32_t kTitleSettings[] = {0x63E83FFF, 0x63E83FFE, 0x63E83FFD};
constexpr size_t kTitleSettingSize = 1000;

std::string Utf8(const fs::path& p) { return p.u8string(); }

std::string Hex8(uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof b, "%08X", v);
  return b;
}

bool EndsWith(std::string s, std::string_view tail) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

// Unpacks a zip with the system's tar (part of Windows 10 and 11; bsdtar elsewhere).
std::string Unzip(const std::string& zip, const std::string& into) {
#if defined(_WIN32)
  wchar_t system_dir[MAX_PATH];
  GetSystemDirectoryW(system_dir, MAX_PATH);
  const std::wstring tar = std::wstring(system_dir) + L"\\tar.exe";
  std::wstring cmd = L"\"" + tar + L"\" -xf \"" + fs::u8path(zip).wstring() + L"\" -C \"" + fs::u8path(into).wstring() +
                     L"\"";
  STARTUPINFOW si{};
  si.cb = sizeof si;
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(tar.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    return "The zip could not be opened (tar.exe did not start).";
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return code == 0 ? std::string() : "The zip could not be unpacked. It may be damaged.";
#else
  const std::string cmd = "bsdtar -xf '" + zip + "' -C '" + into + "' >/dev/null 2>&1";
  return std::system(cmd.c_str()) == 0 ? std::string() : "The zip could not be unpacked. It may be damaged.";
#endif
}

// The files in a folder and below it.
std::vector<fs::path> FilesIn(const fs::path& dir) {
  std::vector<fs::path> out;
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator();
       it.increment(ec)) {
    if (it->is_regular_file(ec)) out.push_back(it->path());
  }
  return out;
}

std::string ProfileDir(const GameInfo& game) { return SavesDir(game) + "/" + Hex8(game.title_id) + "/profile/User"; }
std::string UnlocksBackupDir(const GameInfo& game) { return BackupsDir(game) + "/before-dlc-unlocks"; }
std::string UnlocksMarker(const GameInfo& game) { return ProfileDir(game) + "/dlc-unlocks-installed.txt"; }

// The header the SDK's content manager reads (XCONTENT_AGGREGATE_DATA, 0x148 bytes, big endian).
std::array<uint8_t, 0x148> ContentHeader(const GameInfo& game, const std::string& display_name,
                                         const std::string& file) {
  std::array<uint8_t, 0x148> h{};
  auto be32 = [&](size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) h[at + i] = uint8_t(v >> (24 - 8 * i));
  };
  be32(0x00, 1);  // device: the hard drive
  be32(0x04, kMarketplaceContent);
  // Display name: UTF-16, big endian (ASCII is enough for these names).
  for (size_t i = 0; i < display_name.size() && i < 127; ++i) h[0x08 + i * 2 + 1] = uint8_t(display_name[i]);
  std::memcpy(&h[0x108], file.data(), std::min<size_t>(file.size(), 42));
  // xuid 0 (every profile) at 0x138, the title at 0x140.
  be32(0x140, game.title_id);
  return h;
}

std::string InstallPackage(trg::Task& task, const GameInfo& game, const DlcInfo& dlc, const fs::path& file) {
  stfs::StfsPackage pkg;
  std::string error;
  if (!pkg.Open(file, &error))
    return "That file is not an Xbox 360 package. Choose the " + std::string(dlc.name) + " package or its zip.";
  if (pkg.title_id() != game.title_id)
    return "That package is not for " + std::string(game.title) + ". Choose the " + dlc.name + " package.";
  if (pkg.content_type() != kMarketplaceContent)
    return "That package is not DLC. Choose the " + std::string(dlc.name) + " package.";
  const std::string file_name = Utf8(file.filename());
  if (pkg.display_name() != dlc.package_name && file_name != dlc.file) {
    for (const DlcInfo& other : DlcFor(game))
      if (pkg.display_name() == other.package_name || file_name == other.file)
        return "That is the " + std::string(other.name) + " package, not " + dlc.name + ".";
    return "That package (" + pkg.display_name() + ") is not " + dlc.name + ".";
  }

  const fs::path root = fs::u8path(DlcContentDir(game));
  const fs::path dest = root / dlc.file;
  const fs::path staging = root / (std::string(dlc.file) + ".part");
  std::error_code ec;
  fs::remove_all(staging, ec);
  fs::create_directories(staging, ec);
  task.SetLabel(std::string("Installing ") + dlc.name);
  const bool ok = pkg.ExtractAll(staging, false,
                                 [&task](const stfs::StfsEntry&, uint64_t done, uint64_t total) {
                                   task.Progress((long long)done, (long long)total);
                                   return !task.cancelled();
                                 },
                                 &error);
  if (!ok) {
    fs::remove_all(staging, ec);
    return task.cancelled() ? std::string("Cancelled.") : "The package could not be read: " + error;
  }
  fs::remove_all(dest, ec);
  fs::rename(staging, dest, ec);
  if (ec) return "The files were copied, but the DLC folder could not be finished: " + ec.message();

  const fs::path headers = fs::u8path(SavesDir(game)) / "0000000000000000" / Hex8(game.title_id) / "Headers" /
                           Hex8(kMarketplaceContent);
  fs::create_directories(headers, ec);
  const auto header = ContentHeader(game, pkg.display_name(), dlc.file);
  std::ofstream out(headers / (std::string(dlc.file) + ".header"), std::ios::binary);
  out.write(reinterpret_cast<const char*>(header.data()), std::streamsize(header.size()));
  return out ? std::string() : "The DLC was copied, but its header could not be written.";
}

// The title settings in a profile's title file (XDBF): settings are namespace 3, each entry a 24 byte header and the
// data.
bool ReadTitleSettings(const fs::path& gpd, std::array<std::vector<uint8_t>, 3>& out) {
  std::ifstream in(gpd, std::ios::binary);
  std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)), {});
  auto be = [&](size_t at, int bytes) {
    uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) v = (v << 8) | d[at + i];
    return v;
  };
  if (d.size() < 24 || std::memcmp(d.data(), "XDBF", 4) != 0) return false;
  const size_t table_len = size_t(be(8, 4)), entry_count = size_t(be(12, 4)), free_len = size_t(be(16, 4));
  const size_t base = 24 + table_len * 18 + free_len * 8;
  int found = 0;
  for (size_t i = 0; i < entry_count && 24 + i * 18 + 18 <= d.size(); ++i) {
    const size_t e = 24 + i * 18;
    const uint32_t ns = uint32_t(be(e, 2));
    const uint64_t id = be(e + 2, 8);
    const size_t off = size_t(be(e + 10, 4)), len = size_t(be(e + 14, 4));
    if (ns != 3 || len < 24 + kTitleSettingSize || base + off + len > d.size()) continue;
    for (int k = 0; k < 3; ++k)
      if (id == kTitleSettings[k]) {
        out[k].assign(d.begin() + long(base + off + 24), d.begin() + long(base + off + 24 + kTitleSettingSize));
        ++found;
      }
  }
  return found == 3;
}

std::string InstallUnlocks(const GameInfo& game, const DlcInfo& dlc, const fs::path& gpd) {
  std::array<std::vector<uint8_t>, 3> unlocks;
  if (!ReadTitleSettings(gpd, unlocks))
    return "That file does not hold the unlocks. Choose the " + std::string(dlc.name) + " file (" +
           Hex8(game.title_id) + ".gpd) or its zip.";
  const fs::path dir = fs::u8path(ProfileDir(game));
  const fs::path backup = fs::u8path(UnlocksBackupDir(game));
  std::error_code ec;
  fs::create_directories(dir, ec);
  const bool reinstall = fs::exists(fs::u8path(UnlocksMarker(game)), ec);
  if (!reinstall) {
    fs::remove_all(backup, ec);
    fs::create_directories(backup, ec);
  }
  for (int k = 0; k < 3; ++k) {
    const fs::path file = dir / Hex8(kTitleSettings[k]);
    std::vector<uint8_t> mine;
    if (fs::exists(file, ec)) {
      std::ifstream in(file, std::ios::binary);
      mine.assign(std::istreambuf_iterator<char>(in), {});
      if (!reinstall) fs::copy_file(file, backup / Hex8(kTitleSettings[k]), fs::copy_options::overwrite_existing, ec);
    }
    if (mine.size() != kTitleSettingSize) {
      mine = unlocks[k];  // no settings yet: the file's own, as a whole
    } else {
      for (size_t i = 0; i < kTitleSettingSize; ++i)
        if (unlocks[k][i] == 0xFF) mine[i] = 0xFF;
    }
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(mine.data()), std::streamsize(mine.size()));
    if (!out) return "The unlocks could not be written to your profile.";
  }
  std::ofstream(fs::u8path(UnlocksMarker(game))) << "Installed by the launcher from " << Utf8(gpd.filename()) << "\n";
  return std::string();
}

}  // namespace

const std::vector<DlcInfo>& DlcFor(const GameInfo& game) {
  static const std::vector<DlcInfo> kHd = {
      {"Big Pack", "Trials HD - Big Pack", "B1A493BA359C56825FB0BD67F37931CEFA71695958",
       "New tracks and skill games.", false},
      {"Big Thrills", "Trials HD - Big Thrills", "C149E88B0A92946B58022D1B063F2911B60748C258",
       "More tracks and skill games.", false},
  };
  static const std::vector<DlcInfo> kEvolution = {
      {"Riders of Doom", "Trials Evolution - Riders of Doom", "99844D256ADF73EF3CA5903AE450814C0328D76458",
       "New tracks, a new bike and new rider gear.", false},
      {"Origin of Pain", "Trials Evolution - Origin of Pain", "1F9CD4031B4A567548866C32B0D9049EA512524558",
       "New tracks, skill games and rider gear.", false},
  };
  static const std::vector<DlcInfo> kFusion = {
      {"Riders of the Rustland", "Trials Fusion - Content Pack 1", "492FB00BD448292BA5B3907CE9DF480EC3F1768358",
       "Content pack 1: new tracks.", false},
      {"Empire of the Sky", "Trials Fusion - Content Pack 2", "B38D502F67671A33569C0E9145C49CCFF5271F5158",
       "Content pack 2: new tracks.", false},
      {"Welcome to the Abyss", "Trials Fusion - Content Pack 3", "C1C476F2BFFA6118E85AB672E514BA60ACEE8E3958",
       "Content pack 3: new tracks.", false},
      {"Fire in the Deep", "Trials Fusion - Content Pack 4", "B5D963C43AF4127DC076FAE9A9FA8B10F2426B1758",
       "Content pack 4: new tracks.", false},
      {"Fault One Zero", "Trials Fusion - Content Pack 5", "D1AEC708EE0B70B41104F2BD31C2B059F11027F158",
       "Content pack 5: new tracks.", false},
      {"After the Incident", "Trials Fusion - Content Pack 6", "58ED7EA61ACF5F84CF1B4113D21C165FFE8F6F0D58",
       "Content pack 6: new tracks.", false},
      {"Fallen Angel Bike Upgrade", "Fallen Angel Bike Upgrade", "0015CB7C24BBC9742903FC47E0CE5D3058D9365D58",
       "The Fallen Angel bike upgrade.", false},
      {"Exclusive Content, Uplay Rewards and Season Pass Unlocks", "", "",
       "Unlocks the exclusive, Uplay and season pass items in your profile.", true},
  };
  static const std::vector<DlcInfo> kNone;
  const std::string id = game.id;
  if (id == "trials_hd") return kHd;
  if (id == "trials_evolution") return kEvolution;
  if (id == "trials_fusion") return kFusion;
  return kNone;
}

std::string DlcContentDir(const GameInfo& game) {
  return SavesDir(game) + "/0000000000000000/" + Hex8(game.title_id) + "/" + Hex8(kMarketplaceContent);
}

bool DlcInstalled(const GameInfo& game, const DlcInfo& dlc) {
  std::error_code ec;
  if (dlc.profile_unlocks) return fs::exists(fs::u8path(UnlocksMarker(game)), ec);
  return fs::is_directory(fs::u8path(DlcContentDir(game)) / dlc.file, ec);
}

std::string InstallDlc(trg::Task& task, const GameInfo& game, const DlcInfo& dlc, const std::string& path) {
  const fs::path chosen = fs::u8path(path);
  std::error_code ec;
  if (!fs::exists(chosen, ec)) return "That file was not found.";
  // A zip: unpacked to a work folder first, then the file in it is installed.
  fs::path work;
  std::vector<fs::path> files = {chosen};
  if (EndsWith(path, ".zip")) {
    work = fs::u8path(LauncherDir()) / "dlc-work" / game.id;
    fs::remove_all(work, ec);
    fs::create_directories(work, ec);
    task.SetLabel("Unpacking " + Utf8(chosen.filename()));
    if (std::string err = Unzip(path, Utf8(work)); !err.empty()) {
      fs::remove_all(work, ec);
      return err;
    }
    files = FilesIn(work);
  }
  std::string result = "Nothing to install was found in that zip.";
  for (const fs::path& f : files) {
    if (dlc.profile_unlocks) {
      if (!EndsWith(Utf8(f.filename()), ".gpd") && files.size() > 1) continue;
      result = InstallUnlocks(game, dlc, f);
    } else {
      result = InstallPackage(task, game, dlc, f);
    }
    if (result.empty() || files.size() == 1) break;
  }
  if (!work.empty()) fs::remove_all(work, ec);
  return result;
}

std::string RemoveDlc(const GameInfo& game, const DlcInfo& dlc) {
  std::error_code ec;
  if (dlc.profile_unlocks) {
    const fs::path backup = fs::u8path(UnlocksBackupDir(game));
    const fs::path dir = fs::u8path(ProfileDir(game));
    for (uint32_t id : kTitleSettings) {
      const fs::path saved = backup / Hex8(id);
      if (fs::exists(saved, ec))
        fs::copy_file(saved, dir / Hex8(id), fs::copy_options::overwrite_existing, ec);
      else
        fs::remove(dir / Hex8(id), ec);  // there were none before: the game makes new ones
    }
    fs::remove(fs::u8path(UnlocksMarker(game)), ec);
    return std::string();
  }
  fs::remove_all(fs::u8path(DlcContentDir(game)) / dlc.file, ec);
  fs::remove(fs::u8path(SavesDir(game)) / "0000000000000000" / Hex8(game.title_id) / "Headers" /
                 Hex8(kMarketplaceContent) / (std::string(dlc.file) + ".header"),
             ec);
  return ec ? "The DLC could not be removed: " + ec.message() : std::string();
}

}  // namespace trials
