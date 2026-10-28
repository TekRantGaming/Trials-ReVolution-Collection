// View distances. sub_8232AC08 and sub_8232AC10 store maxViewDistance and maxObjectViewDistance (+176 and +180 of the
// settings object, the track value times 0.1). trials_view_distance scales both; 1 keeps the Xbox 360 distances.

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
