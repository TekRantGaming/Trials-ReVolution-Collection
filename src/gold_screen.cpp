// Trials Evolution: Gold Edition (Home's tile after the games): Ubisoft's PC release on Steam, its description from
// the Steam store and a link to its store page.

#include <string>

#include <trg/platform.h>

#include "app.h"
#include "sounds.h"

namespace trials {
namespace {

using namespace dash;

// From its Steam store page (app 220160), in short.
const char* kDescription =
    "Trials Evolution: Gold Edition brought the Trials series back to PC in 2013. It holds all of Trials Evolution, "
    "the best selling Xbox Live Arcade game, together with the single player tracks and skill games of Trials HD: "
    "two complete games in one.";
const char* kFeatures[] = {"Multiplayer", "Track editor", "Indoor and outdoor environments",
                           "Bike and rider customization", "Global leaderboards"};
const char* kFacts = "RedLynx and Ubisoft Shanghai  \xC2\xB7  Ubisoft  \xC2\xB7  21 March 2013";

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
  Text(f.regular, 16, ImVec2(f.X(48), y), White(0.9f), kDescription, wrap);
  y += TextSize(f.regular, 16, kDescription, wrap).y + 22 * f.s;
  Text(f.bold, 20, ImVec2(f.X(48), y), White(), "Key features");
  y += 34 * f.s;
  for (const char* feature : kFeatures) {
    f.dl->AddCircleFilled(ImVec2(f.X(56), y + 11 * f.s), 4 * f.s, IM_COL32(240, 106, 18, 255));
    Text(f.semibold, 16, ImVec2(f.X(72), y), White(0.9f), feature, wrap - 24);
    y += 28 * f.s;
  }
  y += 10 * f.s;
  Text(f.regular, 14, ImVec2(f.X(48), y), White(0.6f), kFacts, wrap);
  y += 34 * f.s;
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
