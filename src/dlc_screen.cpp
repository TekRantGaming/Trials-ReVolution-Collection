// The DLC page (Add DLC on a game's page): the game's add-ons, each with Install. Install asks for the player's own
// package of that add-on (or the zip it came in) in the file window, checks it is that add-on and installs it
// (dlc.h). Installed ones can be removed or installed again.

#include <string>

#include "app.h"
#include "dlc.h"
#include "sounds.h"

namespace trials {

using namespace dash;

void DrawDlc(App& app, int g) {
  const Frame& f = F();
  const GameInfo& game = Games()[g];
  GameUi& u = app.game_ui(g);
  const std::vector<DlcInfo>& list = DlcFor(game);

  // The file window's answer: the add-on is the tag minus one.
  {
    int pg = -1, tag = 0;
    std::string path;
    if (TakeDlcPick(pg, tag, path) && !path.empty() && pg >= 0 && pg < kGameCount) {
      const GameInfo& pgame = Games()[pg];
      const std::vector<DlcInfo>& plist = DlcFor(pgame);
      if (tag >= 1 && tag <= int(plist.size()) && !app.game_ui(pg).dlc_task.running()) {
        const DlcInfo dlc = plist[size_t(tag - 1)];
        app.game_ui(pg).dlc_message.clear();
        app.game_ui(pg).dlc_task.Start(std::string("Installing ") + dlc.name,
                                       [pgame, dlc, path](trg::Task& task) { return InstallDlc(task, pgame, dlc, path); });
      }
    }
  }
  {
    std::string error;
    const trg::Task::State state = u.dlc_task.Finish(&error);
    if (state == trg::Task::State::kDone) {
      u.dlc_error = false;
      u.dlc_message = "Installed. It is there the next time you play.";
      app.SetToast(u.dlc_message);
      sound::Play(sound::Sound::kSelect);
    } else if (state == trg::Task::State::kFailed || state == trg::Task::State::kCancelled) {
      u.dlc_error = state == trg::Task::State::kFailed;
      u.dlc_message = state == trg::Task::State::kCancelled ? "Cancelled." : error;
      if (u.dlc_error) app.SetToast(u.dlc_message, true);
    }
  }
  if (Pressed(Btn::kB)) {
    app.OpenHub(g);
    return;
  }

  f.dl->AddRectFilled(f.o, f.end, IM_COL32(8, 8, 10, 255));
  Backdrop(app.art().Get(g, ArtKind::kHero), 0.55f, 0.72f);
  Shade();
  f.dl->AddRectFilled(f.o, f.end, Black(0.35f));
  TopBar({game.title, "DLC"});
  Text(f.semibold, 28, f.P(72, 90), White(), "Downloadable content");

  const bool busy = u.dlc_task.running();
  Rows r(u.dlc_rows, f.P(48, 146), ImVec2(f.X(820), f.B(70)), true);
  if (busy) {
    r.Progress(u.dlc_task.label(), u.dlc_task.fraction());
    r.Paragraph("You can leave this screen: the install carries on.");
    if (r.Button("Cancel", "Stops the install. Nothing is left half done.")) u.dlc_task.Cancel();
  } else {
    r.Paragraph("The DLC " + std::string(game.title) + " had on Xbox Live. Each one installs from your own "
                "package of it (from your console's Content folder, 00000002), or the zip it is in. Nothing from "
                "them comes with the collection.");
    if (!u.dlc_message.empty()) r.Paragraph(u.dlc_message, !u.dlc_error);
    if (DlcPickerOpen()) {
      r.Paragraph("Choose the DLC package in the file window that opened.");
      if (r.Button("Show the file window", "Brings the file window to the front, in case it opened behind."))
        DlcPickerToFront();
    }
    for (size_t i = 0; i < list.size(); ++i) {
      const DlcInfo& dlc = list[i];
      const bool installed = DlcInstalled(game, dlc);
      const std::string help = std::string(installed ? "Installed. " : "") + dlc.about;
      if (r.Button(dlc.name, help, installed ? "Remove" : "Install") && !DlcPickerOpen()) {
        if (installed) {
          const GameInfo* pg = &game;
          const DlcInfo d = dlc;
          GameUi* pu = &u;
          Confirm("Remove " + std::string(dlc.name) + "?",
                  d.profile_unlocks ? "Your profile's settings go back to how they were before the unlocks."
                                    : "Its files are deleted. Your saves stay. You can install it again later.",
                  "Remove", [pg, d, pu]() {
                    pu->dlc_message = RemoveDlc(*pg, d);
                    pu->dlc_error = !pu->dlc_message.empty();
                    if (pu->dlc_message.empty()) pu->dlc_message = std::string(d.name) + " is removed.";
                  });
        } else {
          OpenDlcPicker(app.window(), g, int(i) + 1,
                        "Choose your " + std::string(dlc.name) + (dlc.profile_unlocks ? " file (.gpd) or zip"
                                                                                        : " package or zip"));
        }
      }
    }
  }
  HintBar({{Btn::kA, "Select"}, {Btn::kB, "Back"}});
}

}  // namespace trials
