// Object draw distance. Objects fade out between lodDissolveNearest (0.75) and lodDissolveFarest (0.95) of their
// LOD distance, the 360 values (Graphic:Advance:RenderingDebug, floats at 0x82FDC150 and 0x82FDC14C). Far buildings
// and props were missing until the bike got close, then appeared all at once. trials_object_distance scales both;
// 1 keeps the 360 distances. Written when a track loads (sub_821D08A8 picks the LOD range then). At 2 a release
// build held 60 FPS over a full ride.

#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>

// Shadow cascades. The 360 redraws the near PSSM cascade every frame, the second every 2nd and the far two every 4th
// (Graphic:Advance:Shadow:PSSMInterleavedRendering, byte at 0x82FDC74F). trials_shadow_every_frame redraws all four
// every frame instead. Off by default (the Xbox 360 schedule): the shadow faults it was added for were the 16 bit
// EDRAM unpack (SDK patch 0011), and at 4K the extra cascades cost frames (the second track's race at 2x: 59.9
// average and 58.9 lowest per second with the 360 schedule, 56 and 45.3 redrawing every cascade, 30 Oct 2026).
REXCVAR_DEFINE_BOOL(trials_shadow_every_frame, false, "Collection", "Redraw every shadow cascade each frame");

REXCVAR_DEFINE_DOUBLE(trials_object_distance, 2.0, "Collection",
                      "Object draw distance, times the original (1 = Xbox 360)");

namespace {

void StoreFloat(uint8_t* base, uint32_t address, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, 4);
  base[address] = uint8_t(bits >> 24);
  base[address + 1] = uint8_t(bits >> 16);
  base[address + 2] = uint8_t(bits >> 8);
  base[address + 3] = uint8_t(bits);
}

}  // namespace

REX_EXTERN(__imp__sub_821D08A8);
REX_HOOK_RAW(sub_821D08A8) {
  __imp__sub_821D08A8(ctx, base);
  double k = REXCVAR_GET(trials_object_distance);
  if (!(k > 0.0)) k = 1.0;
  if (k > 8.0) k = 8.0;
  StoreFloat(base, 0x82FDC14C, float(0.95 * k));  // lodDissolveFarest
  StoreFloat(base, 0x82FDC150, float(0.75 * k));  // lodDissolveNearest
  base[0x82FDC74F] = REXCVAR_GET(trials_shadow_every_frame) ? 0 : 1;  // PSSMInterleavedRendering
}

// Shadow distance. Shadows fade out between 0.75 and 1 times the shadow view distance a track sets every frame
// (sub_828177C8; 32 on the first tracks), so anything a little ahead of the camera had none and shadows popped in as
// the bike came closer. trials_shadow_distance scales it through the game's own debug override
// (enableTweakingShadowDrawDistance, byte at 0x83059A5A, with MaxShadowViewDistance, float at 0x82FDC788); 1 keeps
// the 360 distance.
REXCVAR_DEFINE_DOUBLE(trials_shadow_distance, 3.0, "Collection", "Shadow distance, times the original (1 = Xbox 360)");

REX_EXTERN(__imp__sub_828177C8);
REX_HOOK_RAW(sub_828177C8) {
  const double distance = ctx.f1.f64;
  __imp__sub_828177C8(ctx, base);
  double k = REXCVAR_GET(trials_shadow_distance);
  if (!(k > 0.0)) k = 1.0;
  if (k > 8.0) k = 8.0;
  base[0x83059A5A] = k != 1.0;  // enableTweakingShadowDrawDistance
  StoreFloat(base, 0x82FDC788, float(distance * k));  // MaxShadowViewDistance
}
