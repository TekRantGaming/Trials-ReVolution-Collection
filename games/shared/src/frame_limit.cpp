// Frame rate for Trials Evolution and Trials Fusion: always 60, the game's own vertical sync. Their game speed follows
// the frame rate (at 120 the race countdown took 1.5 seconds instead of 3.5, at 30 everything ran slow), and unlike
// Trials HD (fps_unlock.cpp) they have no fixed step loop with interpolated frames yet. trials_frame_rate is still
// defined because the launcher's settings file may hold it; it is ignored here, and the launcher offers it only for
// Trials HD.

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_INT32(trials_frame_rate, 60, "Collection",
                     "Frames a second (Trials HD only: this game always runs at 60, its speed follows the frame rate)");

namespace trials {

void ApplyFrameRate() {
  if (REXCVAR_GET(trials_frame_rate) != 60)
    REXLOG_INFO("Trials: frame rate {} ignored, this game runs at 60", REXCVAR_GET(trials_frame_rate));
}

void LimitFrame() {}

}  // namespace trials
