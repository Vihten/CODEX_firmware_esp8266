#pragma once
#include <cstdint>

namespace external_light {
class Sequence {
 public:
  // Call from one owner with monotonic milliseconds modulo 2^32, at least once
  // per clock wrap. matrixDark must describe the last completed output frame.
  void update(uint32_t now, bool follow, bool requestedOn, bool matrixDark,
              bool forcedOff, bool paused = false) {
    const uint32_t delta = clockStarted_ ? uint32_t(now - lastNow_) : 0;
    lastNow_ = now;
    clockStarted_ = true;
    const bool advance = !paused && !wasPaused_;
    wasPaused_ = paused;
    forced_ = forcedOff;
    independent_ = !follow;
    requestedOn_ = requestedOn;
    if (forcedOff) {
      settle(0);
      return;
    }
    if (!follow) {
      // Retain the actual bypass gain when follow mode is enabled again.
      settle(1023);
      return;
    }
    if (paused) return;
    if (advance && moving_) {
      elapsed_ += delta < kDurationMs - elapsed_ ? delta : kDurationMs - elapsed_;
      const uint64_t t = elapsed_;
      const uint64_t numerator = t * t * (3U * kDurationMs - 2U * t);
      constexpr uint64_t denominator = uint64_t(kDurationMs) * kDurationMs * kDurationMs;
      const uint16_t distance = from_ < target_ ? uint16_t(target_ - from_) : uint16_t(from_ - target_);
      const uint16_t change = uint16_t((distance * numerator + denominator / 2) / denominator);
      gain_ = target_ >= from_ ? uint16_t(from_ + change) : uint16_t(from_ - change);
      if (elapsed_ == kDurationMs) settle(target_);
    }
    if (requestedOn) {
      aim(1023);
    } else if (!matrixDark) {
      // Hold illumination until the renderer confirms its final black frame.
      settle(gain_);
    } else {
      aim(0);
    }
  }

  uint16_t gain() const { return forced_ ? 0 : independent_ ? 1023 : gain_; }
  bool readyToOpen() const {
    return !forced_ && (independent_ || (requestedOn_ && gain_ == 1023 && !moving_));
  }

 private:
  static constexpr uint32_t kDurationMs = 700;
  uint32_t lastNow_ = 0, elapsed_ = 0;
  uint16_t gain_ = 0, from_ = 0, target_ = 0;
  bool clockStarted_ = false, wasPaused_ = false, moving_ = false;
  bool independent_ = false, forced_ = false, requestedOn_ = false;

  void settle(uint16_t value) {
    gain_ = from_ = target_ = value;
    elapsed_ = 0;
    moving_ = false;
  }
  void aim(uint16_t value) {
    if (moving_ && target_ == value) return;
    if (gain_ == value) { settle(value); return; }
    from_ = gain_;
    target_ = value;
    elapsed_ = 0;
    moving_ = true;
  }
};
}  // namespace external_light
