// Depth of field off (the launcher's Graphics > Depth of field, trials_depth_of_field). The scene renderer
// (sub_825F0278, r3 its object) hands the post blur shader "dofParameters" from the four floats at +528 of the
// object at +52; the shader's blur amount is the last two (+536, +540) times the distance from the focus. Zero, the
// combine keeps the sharp picture (and its bloom). Set before each call, since the game sets them per track.

#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>

REXCVAR_DEFINE_BOOL(trials_depth_of_field, true, "Collection", "Depth of field blur in the distance");

REX_EXTERN(__imp__sub_825F0278);
REX_HOOK_RAW(sub_825F0278) {
  if (!REXCVAR_GET(trials_depth_of_field)) {
    const uint32_t self = ctx.r3.u32;
    auto load32 = [base](uint32_t a) {
      return uint32_t(base[a]) << 24 | uint32_t(base[a + 1]) << 16 | uint32_t(base[a + 2]) << 8 | base[a + 3];
    };
    const uint32_t params = self ? load32(self + 52) : 0;
    if (params) {
      for (uint32_t i = 536; i < 544; ++i) base[params + i] = 0;  // 0.0f, big endian
    }
  }
  __imp__sub_825F0278(ctx, base);
}
