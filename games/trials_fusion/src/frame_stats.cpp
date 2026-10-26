// Frame counter: counts the game's own frames (sub_826CB050 is the one function that calls VdSwap), for the SDK's F3
// overlay ("Guest: N FPS") and, with trials_log_fps on, a line in the log each second (frames, average and slowest
// frame).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ui/overlay/debug_overlay.h>

REXCVAR_DEFINE_BOOL(trials_log_fps, false, "Collection", "Log the game's frame rate once a second");

namespace trials {

void LimitFrame();  // frame_limit.cpp (shared)

namespace {

using Clock = std::chrono::steady_clock;

struct Window {
  Clock::time_point start = Clock::now(), last = start;
  uint32_t frames = 0;
  double slowest_ms = 0.0;
};
Window g_window;  // only the game's render thread presents
std::atomic<uint64_t> g_total{0};
std::atomic<double> g_fps{0.0}, g_frame_ms{0.0};

}  // namespace

void CountFrame() {
  const Clock::time_point now = Clock::now();
  Window& w = g_window;
  w.slowest_ms = std::max(w.slowest_ms, std::chrono::duration<double, std::milli>(now - w.last).count());
  w.last = now;
  ++w.frames;
  ++g_total;
  const double secs = std::chrono::duration<double>(now - w.start).count();
  if (secs < 1.0) return;
  g_fps = w.frames / secs;
  g_frame_ms = secs * 1000.0 / w.frames;
  if (REXCVAR_GET(trials_log_fps)) {
    REXLOG_INFO("Trials fps: {:.1f} ({} frames, {:.2f} ms average, slowest {:.2f} ms)", g_fps.load(), w.frames,
                g_frame_ms.load(), w.slowest_ms);
  }
  w = Window();
  w.start = w.last = now;
}

rex::ui::FrameStats GuestFrameStats() {
  rex::ui::FrameStats s;
  s.fps = g_fps;
  s.frame_time_ms = g_frame_ms;
  s.frame_count = g_total;
  return s;
}

}  // namespace trials

REX_EXTERN(__imp__sub_826CB050);
REX_HOOK_RAW(sub_826CB050) {
  __imp__sub_826CB050(ctx, base);
  trials::CountFrame();
  trials::LimitFrame();
}
