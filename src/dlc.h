// Downloadable content: the add-ons each game had on Xbox Live, installed from the player's own packages. The DLC
// page (dlc_screen.cpp) lists them and asks for each package; the install puts the package's files where the game
// finds its marketplace content, as on the console's hard drive.
#pragma once

#include <string>
#include <vector>

#include <trg/platform.h>

#include "games.h"

namespace trials {

struct DlcInfo {
  const char* name;          // as the player knows it: "Riders of the Rustland"
  const char* package_name;  // the package's display name, which identifies it: "Trials Fusion - Content Pack 1"
  const char* file;          // the package's file name (in Content/<profile>/<title ID>/00000002): its content folder
  const char* about;         // one line about it
  bool profile_unlocks;      // not a package: a profile file (.gpd) whose title settings unlock items
};

const std::vector<DlcInfo>& DlcFor(const GameInfo& game);

// Where the game finds marketplace content: <saves>/0000000000000000/<title ID>/00000002 (the SDK's content
// manager keeps add-ons for every profile there).
std::string DlcContentDir(const GameInfo& game);
bool DlcInstalled(const GameInfo& game, const DlcInfo& dlc);

// Installs one add-on from `path`: its package file, the zip it came in, or (for the profile unlocks) its .gpd
// file or zip. Checks that the package is this game's and this add-on. Runs on the task's thread.
std::string InstallDlc(trg::Task& task, const GameInfo& game, const DlcInfo& dlc, const std::string& path);
// Removes an installed add-on (the profile unlocks are put back from the backup made when they were installed).
std::string RemoveDlc(const GameInfo& game, const DlcInfo& dlc);

}  // namespace trials
