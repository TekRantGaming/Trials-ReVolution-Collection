// Getting a game onto the PC, in the two steps the home screen shows:
//   Download  the newest release of the game's recompilation from GitHub (the
//             TRG updater: checked download, every listed file installed, undo
//             on failure). The same job later installs updates automatically.
//   Install   the files from the player's own Xbox Live Arcade package (or a
//             folder it was extracted to), after checking its title ID.
#pragma once

#include <string>

#include <trg/platform.h>
#include <trg/updater.h>

#include "games.h"

namespace trials {

trg::UpdaterConfig GameUpdaterConfig(const GameInfo& game);

// Testing: where releases are asked for instead of GitHub ("" = GitHub). Set
// by --release-server or TRIALS_RELEASE_SERVER; tools/fake_github.py serves
// test releases there.
extern std::string g_release_server;

struct GameJobs {
  enum class Kind { kNone, kDownload, kUpdateCheck, kInstall };
  Kind kind = Kind::kNone;
  std::string message;       // the last result, for the player
  bool message_is_error = false;
  bool quiet = false;        // the automatic startup update: no message unless something was installed
  std::string update_found;  // a newer version found by the startup check ("" when none)
  std::string from_version;  // what was installed when the download started ("" for a first download)
  trg::UpdateCheck check;
  trg::InstallReport report;
  trg::Task task;            // last, so it stops before the results above go away
};

// Download (first install) or update: checks GitHub and installs the newest
// release. `quiet_if_current` keeps the message empty when nothing is newer.
void StartDownload(const GameInfo& game, GameJobs& jobs, bool quiet = false);
// Startup check: finds out whether a newer release exists, installs nothing.
void StartUpdateCheck(const GameInfo& game, GameJobs& jobs);
// Extracts the player's package (or copies an extracted folder) into the game folder after checking its title ID.
// `path` may be the package, a folder holding it, or default.xex or its folder.
void StartPackageInstall(const GameInfo& game, const std::string& path, GameJobs& jobs);
// Call each frame: collects a finished job's result into jobs.message.
// Returns true on the frame a job finished.
bool PollJobs(const GameInfo& game, GameJobs& jobs);

// The game a title ID belongs to, in words, when it is not the one being installed.
std::string DescribeTitleId(uint32_t title_id);

}  // namespace trials
