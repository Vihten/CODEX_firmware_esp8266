#pragma once

#include <cmath>
#include <cstdint>

namespace quota_recovery {

constexpr int32_t signedPhase(uint32_t phase) {
  return phase > 100000 ? int32_t(phase) - 200000 : int32_t(phase);
}

// Rare quota resets use the authored sky rotation, not a new full scene entrance.
class Motion {
 public:
  static constexpr uint32_t durationMs = 2600;

  void observe(uint32_t epoch, bool visible, bool allowNew = true) {
    if (epoch != epoch_) {
      epoch_ = epoch;
      pending_ = visible && allowNew;
    }
    if (!visible) { pending_ = false; active_ = false; }
  }

  bool pending() const { return pending_; }
  bool active() const { return active_; }

  void begin(uint32_t from, uint32_t target) {
    pending_ = false;
    const int32_t distance = int32_t(from) - int32_t(target);
    offset_ = float(signedPhase(uint32_t((distance + 200000) % 200000)));
    elapsed_ = 0;
    active_ = offset_ > 0;
  }

  uint32_t advance(uint32_t delta, uint32_t target) {
    if (!active_) return target;
    elapsed_ += delta < durationMs - elapsed_ ? delta : durationMs - elapsed_;
    if (elapsed_ == durationMs) { active_ = false; return target; }
    const float seconds = elapsed_ / 1000.0f;
    const float t = float(elapsed_) / durationMs;
    const float taper = 1 - t * t * (3 - 2 * t);
    // Zero initial velocity; a small overshoot naturally settles at solar noon.
    const float displacement = offset_ * std::exp(-5.0f * seconds) *
        (std::cos(4.2f * seconds) + (5.0f / 4.2f) * std::sin(4.2f * seconds)) * taper;
    const int32_t phase = int32_t(target) + int32_t(std::lround(displacement));
    return uint32_t((phase % 200000 + 200000) % 200000);
  }

 private:
  uint32_t epoch_ = 0, elapsed_ = 0;
  float offset_ = 0;
  bool pending_ = false, active_ = false;
};

}  // namespace quota_recovery
