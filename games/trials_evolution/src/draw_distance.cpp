// View distances. sub_8232AC08 and sub_8232AC10 store maxViewDistance and maxObjectViewDistance (+176 and +180 of the
// settings object, the track value times 0.1). trials_view_distance scales both; 1 keeps the Xbox 360 distances.

#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>

REXCVAR_DEFINE_DOUBLE(trials_view_distance, 2.0, "Collection",
                      "View and object draw distance, times the original (1 = Xbox 360)");

namespace {

double Scale() {
  double k = REXCVAR_GET(trials_view_distance);
  if (!(k > 0.0)) k = 1.0;
  if (k > 8.0) k = 8.0;
  return k;
}

}  // namespace

REX_EXTERN(__imp__sub_8232AC08);
REX_HOOK_RAW(sub_8232AC08) {
  ctx.f1.f64 *= Scale();  // maxViewDistance
  __imp__sub_8232AC08(ctx, base);
}

REX_EXTERN(__imp__sub_8232AC10);
REX_HOOK_RAW(sub_8232AC10) {
  ctx.f1.f64 *= Scale();  // maxObjectViewDistance
  __imp__sub_8232AC10(ctx, base);
}

// Shadow cascades. sub_8266A050 renders the four PSSM cascades: the near one every frame, the second when the frame
// counter (+444) is even, the third when it is 1 and the far one when it is 3 (mod 4), unless the int at +0x21554 is
// set (the game sets it for a full redraw). trials_shadow_every_frame sets it before each call, so all four are
// redrawn every frame.
REXCVAR_DEFINE_BOOL(trials_shadow_every_frame, true, "Collection", "Redraw every shadow cascade each frame");

REX_EXTERN(__imp__sub_8266A050);
REX_HOOK_RAW(sub_8266A050) {
  if (REXCVAR_GET(trials_shadow_every_frame)) {
    uint8_t* flag = base + ctx.r3.u32 + 0x21554;
    flag[0] = 0;
    flag[1] = 0;
    flag[2] = 0;
    flag[3] = 1;
  }
  __imp__sub_8266A050(ctx, base);
}
