// Frame rate for Trials Evolution and Trials Fusion: their engines time each frame themselves (the PC versions run
// unlocked), so above or below 60 only the pacing changes. At 60 the game keeps its own vertical sync; at any other
// rate that is turned off and the frame presenter (the one function that calls VdSwap) waits out the rest of each
// frame here. trials_frame_rate is the launcher's setting (0 is no limit). Trials HD has its own loop
// (fps_unlock.cpp).

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#endif

#include <rex/cvar.h>

REXCVAR_DEFINE_INT32(trials_frame_rate, 60, "Collection",
                     "Frames a second: 60 is the original pacing, 0 is no limit, any other rate replaces vsync");

namespace trials {
namespace {

using Clock = std::chrono::steady_clock;
Clock::time_point g_next;

}  // namespace

void ApplyFrameRate() {
  if (REXCVAR_GET(trials_frame_rate) == 60) return;
  rex::cvar::SetFlagByName("vsync", "false");
  rex::cvar::SetFlagByName("d3d12_submit_on_primary_buffer_end", "false");
  rex::cvar::SetFlagByName("vulkan_submit_on_primary_buffer_end", "false");
#if defined(_WIN32)
  timeBeginPeriod(1);  // millisecond sleeps for the wait below
#endif
}

// Called after each presented frame.
void LimitFrame() {
  const int rate = REXCVAR_GET(trials_frame_rate);
  if (rate == 60 || rate <= 0) return;
  const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / rate));
  const Clock::time_point now = Clock::now();
  if (g_next < now - period) g_next = now;  // fell behind (loading): start again from now
  g_next += period;
  // Sleep most of the way, then spin the last stretch for an even pace.
  const Clock::time_point sleep_until = g_next - std::chrono::milliseconds(2);
  if (sleep_until > now) std::this_thread::sleep_until(sleep_until);
  while (Clock::now() < g_next) std::this_thread::yield();
}

}  // namespace trials
