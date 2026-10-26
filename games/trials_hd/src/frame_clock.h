// From Trials HD Recompiled by snufflyyy (MIT, see THIRD_PARTY_NOTICES.md): src/frame_clock.h.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace trials_hd {

inline float AdvanceScrollPhase(float phase, float increment) {
  phase += increment;
  return phase > 1.0f ? phase - 1.0f : phase;
}

// Keep the game's 16,666 us tick and five-tick hitch limit, not a render delta.
class FrameClock {
 public:
  using Duration = std::chrono::nanoseconds;
  static constexpr Duration kStep = std::chrono::microseconds(16666);
  static constexpr uint32_t kMaxSteps = 5;

  uint32_t Advance(Duration elapsed) {
    pending_ = std::min(pending_ + std::max(elapsed, Duration::zero()), kStep * kMaxSteps);
    const auto steps = uint32_t(pending_ / kStep);
    pending_ -= kStep * steps;
    return steps;
  }

  Duration UntilNextStep() const { return kStep - pending_; }
  float InterpolationFraction() const { return float(pending_.count()) / float(kStep.count()); }

 private:
  Duration pending_{};
};

}  // namespace trials_hd
