#pragma once
#include <cstdint>

namespace memorial {
constexpr uint32_t kSettledBeforeMinuteMs = 2000;
constexpr uint32_t kPreparationMs = 5000 + kSettledBeforeMinuteMs;
constexpr uint32_t kMinuteMs = 60000;

struct Clock {
  uint32_t date = 0; // Sortable local Kyiv day key, e.g. YYYYMMDD.
  uint8_t hour = 0, minute = 0, second = 0;
  bool valid = false;
  uint16_t millisecond = 0;
};

struct State {
  bool active = false, manual = false, preparing = false;
  uint32_t startedAt = 0, durationMs = kMinuteMs;
  uint32_t consumedDate = 0, lastDate = 0;

  void update(uint32_t now, const Clock &clock, bool enabled, bool matrixOn) {
    const bool valid = clock.valid && clock.date && clock.hour < 24 &&
                       clock.minute < 60 && clock.second < 60 && clock.millisecond < 1000;
    if (valid && clock.date > lastDate) lastDate = clock.date;
    windowDate_ = valid && clock.hour == 9 && clock.minute == 0 ? clock.date : 0;
    const bool wasPreparing = preparing;
    preparing = false;
    if (!matrixOn) {
      clear();
      return;
    }
    // Observe overlap before expiry so a preview ending in this update consumes today.
    if (active && manual && windowDate_) consume(windowDate_);
    if (active && ((!manual && !enabled) || uint32_t(now - startedAt) >= durationMs))
      clear();
    if (active || !enabled || !valid || clock.date < lastDate || clock.date <= consumedDate) return;
    const uint32_t minuteMs = uint32_t(clock.second) * 1000 + clock.millisecond;
    if (clock.hour == 8 && clock.minute == 59 && kMinuteMs - minuteMs <= kPreparationMs) {
      preparing = true;
      startedAt = now + kMinuteMs - minuteMs;
      durationMs = kMinuteMs;
      return;
    }
    if (!windowDate_) return;
    consume(windowDate_);
    active = true;
    manual = false;
    startedAt = wasPreparing ? now - minuteMs : now;
    durationMs = wasPreparing ? kMinuteMs : kMinuteMs - minuteMs;
  }

  // The caller must freshly validate matrix power before requesting a preview.
  bool start(uint32_t now) {
    if (active) return false;
    preparing = false;
    active = true;
    manual = true;
    startedAt = now;
    durationMs = kMinuteMs;
    if (windowDate_) consume(windowDate_);
    return true;
  }

  // Uses the latest update's clock window, including an idle stop command.
  void stop() {
    if (windowDate_) consume(windowDate_);
    clear();
  }

 private:
  uint32_t windowDate_ = 0;

  void consume(uint32_t date) {
    if (date > consumedDate) consumedDate = date;
  }
  void clear() {
    active = false;
    manual = false;
    preparing = false;
  }
};
} // namespace memorial
