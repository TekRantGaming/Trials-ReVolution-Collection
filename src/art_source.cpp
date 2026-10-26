// Where the game art is downloaded from: SteamGridDB's image server
// (cdn2.steamgriddb.com), which serves each image to anyone without an API key.
// The images were picked from the games' SteamGridDB pages;
// change a line below to use another one. The result is cached (art.h), so
// each image is downloaded once.
//
//   Trials HD         https://www.steamgriddb.com/game/5279176
//   Trials Evolution  https://www.steamgriddb.com/game/5499128 (the Gold Edition, 2260, has more)
//   Trials Fusion     https://www.steamgriddb.com/game/2877
//
// Other images on those pages (for swapping):
//   Trials HD grids         25d2570f7caac8015fc687cd54df062d.png, 548a9b2795d2a7ba2cf435f1fb6045a6.png
//   Trials Evolution grids  8b4a0915b01344fd562c8b01c4bab470.png; Gold Edition 4afab2bc5757a92efb5266c7ea0365d1.png,
//                           hero 52b243733f53d6184e349f187c32c961.png
//   Trials Fusion grids     918f713bf98dfe11ecb21d67ec5d33df.png; hero 4e9e522510b41cda61ce6460b460fec3.jpg

#include <string>

#include <trg/download.h>

#include "art.h"

namespace trials {
namespace {

struct ArtSet {
  int steamgriddb_id;
  const char* grid;  // 600 x 900 card
  const char* hero;  // 1920 x 620 background
  const char* logo;  // transparent title
};

const ArtSet kArt[] = {
    {5279176, "grid/bdd79bc4508287d24d048b93eb991564.jpg", "hero/e461188685e7bd9c59c70a633bd2111d.jpg",
     "logo/c8d07e8982eec15cf1002efafc1d7cf8.png"},
    {5499128, "grid/6e9c82745b3c936b267f577084c6388f.png", "hero/23736f4ad9ebdd2b4ae773263f4a94a5.png",
     "logo/d033187b6f6d8fd7441666c244823b91.png"},
    {2877, "grid/67c68199f158340828fc50c3f66c99c5.png", "hero/67249303d47d37fe078e1439f958b92b.png",
     "logo/65a579da11fa712149bca347ecfcb208.png"},
};

}  // namespace

bool g_art_downloads_allowed = true;

std::string FetchArt(int steamgriddb_id, ArtKind kind, std::string& bytes, std::string& ext) {
  bytes.clear();
  if (!g_art_downloads_allowed) return "offline";
  for (const ArtSet& a : kArt) {
    if (a.steamgriddb_id != steamgriddb_id) continue;
    const char* path = kind == ArtKind::kGrid ? a.grid : kind == ArtKind::kHero ? a.hero : a.logo;
    const std::string p = path;
    ext = p.substr(p.rfind('.'));
    return trg::HttpGet("https://cdn2.steamgriddb.com/" + p, bytes);
  }
  return "no art for this game";
}

}  // namespace trials
