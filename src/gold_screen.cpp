// Trials Evolution: Gold Edition (Home's tile after the games): Ubisoft's PC release on Steam, with a link to its
// store page, and why this collection recompiles the original Xbox 360 games anyway. The README has the long text.

#include <string>

#include <trg/platform.h>

#include "app.h"
#include "sounds.h"

namespace trials {
namespace {

using namespace dash;

struct Point {
  const char* title;
  const char* text;
};

const Point kPoints[] = {
    {"The original physics",
     "Gold Edition rebuilt the Trials HD tracks on the Trials Evolution engine, so they ride differently and some "
     "of them changed. Here each game runs its own code, exactly as on the Xbox 360."},
    {"Four expansions",
     "Gold Edition leaves out the four major expansions of Trials HD and Trials Evolution: Big Pack, Big Thrills, "
     "Riders of Doom and Origin of Pain, 151 tracks in all. Only the original games have them."},
    {"Each game as itself",
     "Trials HD, Trials Evolution and Trials Fusion stay three games, each with its own look, physics and content."},
};

}  // namespace

void DrawGoldEdition(App& app) {
  const Frame& f = F();
  if (Pressed(Btn::kB)) {
    app.OpenHome(-3);
    return;
  }

  f.dl->AddRectFilled(f.o, f.end, IM_COL32(8, 8, 10, 255));
  Backdrop(app.art().Get(kGoldEditionArt, ArtKind::kHero), 1.0f, 0.55f);
  Shade();
  f.dl->AddRectFilledMultiColor(f.o, f.end, Black(0.75f), Black(0.35f), Black(0.35f), Black(0.75f));
  TopBar({"Home", "Trials Evolution: Gold Edition"});

  // The text, on the left.
  const float wrap = 640;
  float y = f.Y(84);
  if (const Texture& logo = app.art().Get(kGoldEditionArt, ArtKind::kLogo)) {
    const float w = 300 * f.s, h = w * float(logo.height) / float(logo.width);
    f.dl->AddImage(ImTextureRef(logo.id), ImVec2(f.X(48), y), ImVec2(f.X(48) + w, y + h));
    y += h + 14 * f.s;
  }
  const std::string intro =
      "Trials Evolution: Gold Edition is Ubisoft's PC release of Trials Evolution, with the tracks of Trials HD. If "
      "you would like to play it, please buy it on Steam: it supports the people who made these games.";
  Text(f.regular, 16, ImVec2(f.X(48), y), White(0.9f), intro, wrap);
  y += TextSize(f.regular, 16, intro, wrap).y + 22 * f.s;
  Text(f.bold, 22, ImVec2(f.X(48), y), White(), "Why this collection exists");
  y += 38 * f.s;
  for (const Point& p : kPoints) {
    f.dl->AddCircleFilled(ImVec2(f.X(56), y + 11 * f.s), 4 * f.s, IM_COL32(240, 106, 18, 255));
    Text(f.semibold, 16, ImVec2(f.X(72), y), White(), p.title, wrap - 24);
    y += 24 * f.s;
    Text(f.regular, 15, ImVec2(f.X(72), y), White(0.78f), p.text, wrap - 24);
    y += TextSize(f.regular, 15, p.text, wrap - 24).y + 14 * f.s;
  }
  const std::string goal =
      "This collection does not replace or compete with Ubisoft's PC releases. Its goal is to preserve the original "
      "Xbox 360 games themselves.";
  y += 4 * f.s;
  Text(f.semibold, 15, ImVec2(f.X(48), y), White(0.9f), goal, wrap);
  y += TextSize(f.semibold, 15, goal, wrap).y + 22 * f.s;
  const PillRect steam = Pill(ImVec2(f.X(48), y), "View on Steam", true, true);
  const bool click = Clicked(steam.a, steam.b);

  // The box art, on the right.
  if (const Texture& grid = app.art().Get(kGoldEditionArt, ArtKind::kGrid)) {
    const float h = 470, w = h * float(grid.width) / float(grid.height);
    const ImVec2 a(f.X(1280 - 48 - w), f.Y(110)), b(f.X(1280 - 48), f.Y(110 + h));
    f.dl->AddRectFilled(ImVec2(a.x + 6 * f.s, a.y + 8 * f.s), ImVec2(b.x + 6 * f.s, b.y + 8 * f.s), Black(0.5f),
                        12 * f.s);
    FitArt(grid, a, b, 10 * f.s);
  }

  if (Pressed(Btn::kA) || click) {
    sound::Play(sound::Sound::kSelect);
    trg::OpenUrl(kGoldEditionStoreUrl);
  }
  HintBar({{Btn::kA, "View on Steam"}, {Btn::kB, "Back"}});
}

}  // namespace trials
