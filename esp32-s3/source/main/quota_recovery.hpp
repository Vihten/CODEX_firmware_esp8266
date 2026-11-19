#pragma once

#include <cmath>
#include <cstdint>

namespace quota_recovery {

constexpr int32_t signedPhase(uint32_t phase) {
  return phase > 100000 ? int32_t(phase) - 200000 : int32_t(phase);
}

// One authored scene handoff: the palette must not arrive before the quota.
// Retarget from the last displayed sample, with bounded work and no allocation.
class ProviderTransition {
 public:
  // Leave room for the next rendered frame within the three-second handoff.
  static constexpr uint32_t durationMs = 2900;
  void begin(uint32_t phase, uint8_t tint) {
    position_ = fromPhase_ = int32_t(phase);
    fromTint_ = tint;
    elapsed_ = phaseStartedAt_ = 0;
    hasTarget_ = false;
  }
  bool active() const { return elapsed_ < durationMs; }
  void advance(uint32_t delta, uint32_t target, uint8_t tintTarget,
               uint32_t &phase, uint8_t &tint) {
    if (!hasTarget_) {
      destination_ = fromPhase_ + signedPhase((target + 200000 - uint32_t(fromPhase_)) % 200000);
      targetPhase_ = target;
      hasTarget_ = true;
    } else if (targetPhase_ != target) {
      // Keep the destination on the same arc when a new limit arrives mid-fade.
      destination_ += signedPhase((target + 200000 - targetPhase_) % 200000);
      targetPhase_ = target;
      fromPhase_ = position_;
      phaseStartedAt_ = elapsed_;
    }
    elapsed_ += delta < durationMs - elapsed_ ? delta : durationMs - elapsed_;
    if (!active()) { phase = target; tint = tintTarget; return; }
    const float progress = ease(float(elapsed_) / durationMs);
    const float phaseProgress = phaseStartedAt_
        ? ease(float(elapsed_ - phaseStartedAt_) / (durationMs - phaseStartedAt_)) : progress;
    position_ = fromPhase_ + int32_t(std::lround((destination_ - fromPhase_) * phaseProgress));
    phase = uint32_t((position_ % 200000 + 200000) % 200000);
    tint = uint8_t(std::lround(fromTint_ + (int(tintTarget) - int(fromTint_)) * progress));
  }
 private:
  // Match the incumbent scene's smoothstep for position and palette.
  static float ease(float t) { return t * t * (3 - 2 * t); }
  int32_t position_ = 0, fromPhase_ = 0, destination_ = 0;
  uint32_t targetPhase_ = 0, elapsed_ = durationMs, phaseStartedAt_ = 0;
  uint8_t fromTint_ = 0;
  bool hasTarget_ = false;
};

// Rare quota resets use the authored sky rotation, not a new full scene entrance.
class Motion {
 public:
  static constexpr uint32_t durationMs = 1733;

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
    const float t = float(elapsed_) / durationMs;
    // Preserve the authored spring shape while playing it 50% faster.
    const float seconds = t * 2.6f;
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
