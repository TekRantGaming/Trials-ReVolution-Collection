// Object draw distance. Objects fade out between lodDissolveNearest (0.75) and lodDissolveFarest (0.95) of their
// LOD distance, the 360 values (Graphic:Advance:RenderingDebug, floats at 0x82FDC150 and 0x82FDC14C). Far buildings
// and props were missing until the bike got close, then appeared all at once. trials_object_distance scales both;
// 1 keeps the 360 distances. Written when a track loads (sub_821D08A8 picks the LOD range then). Measured 28 Oct
// 2026 on a ride: 1.6 held 60 FPS, 2 fell to 10 to 20 and 2.5 to about 40, so the default stays at 1.5.

#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>

REXCVAR_DEFINE_DOUBLE(trials_object_distance, 1.5, "Collection",
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
}
