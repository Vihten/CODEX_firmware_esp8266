#pragma once

#include <cstdint>

namespace air_alert {

constexpr uint32_t kTransitionMs = 600;

struct Tint {
  float red = 0, green = 0, blue = 0, amount = 0;
};

struct Color {
  uint8_t red, green, blue;
};

inline uint8_t normalizedLevel(uint8_t level) { return level <= 3 ? level : 3; }

class Visual {
 public:
  Tint update(uint8_t level, uint32_t now) {
    const uint32_t elapsed = now - startedAt_;
    if (elapsed >= kTransitionMs) {
      current_ = from_ = to_;
    } else {
      const float t = float(elapsed) / kTransitionMs;
      current_ = {from_.red + (to_.red - from_.red) * t,
                  from_.green + (to_.green - from_.green) * t,
                  from_.blue + (to_.blue - from_.blue) * t,
                  from_.amount + (to_.amount - from_.amount) * t};
    }
    level = normalizedLevel(level);
    if (level != level_) {
      level_ = level;
      from_ = to_ = current_;
      to_.amount = level == 1 || level == 2 ? 1.0f : 0.0f;
      if (level == 1) { to_.red = 1; to_.green = .70f; to_.blue = 0; }
      if (level == 2) { to_.red = 1; to_.green = .06f; to_.blue = 0; }
      startedAt_ = now;
    }
    return current_;
  }

  Tint tint() const { return current_; }

 private:
  Tint from_, to_, current_;
  uint32_t startedAt_ = 0;
  uint8_t level_ = 0;
};

inline void tintSky(uint8_t *rgb, uint8_t effectMask, bool fixed,
                    const Tint &tint) {
  if (effectMask || fixed || tint.amount <= 0) return;
  unsigned peak = rgb[0];
  if (rgb[1] > peak) peak = rgb[1];
  if (rgb[2] > peak) peak = rgb[2];
  // A bounded source-space lift keeps nights readable without bypassing dimming.
  if (peak < 48) peak = 48;
  if (peak > 96) peak = 96;
  const float palette[] = {tint.red, tint.green, tint.blue};
  for (unsigned channel = 0; channel < 3; ++channel)
    rgb[channel] = uint8_t(rgb[channel] * (1 - tint.amount) +
                           peak * palette[channel] * tint.amount + .5f);
}

inline bool indicatorVisible(uint8_t level, bool alternateScreen, bool on,
                             bool maintenance, uint8_t brightness) {
  level = normalizedLevel(level);
  return on && !maintenance && brightness && level &&
      (alternateScreen || level == 3);
}

inline bool indicatorPixel(uint8_t level, unsigned x, unsigned y) {
  if (x >= 3 || y >= 5) return false;
  if (normalizedLevel(level) == 3) {
    constexpr uint8_t question[] = {7, 1, 2, 0, 2};
    return (question[y] & (4 >> x)) != 0;
  }
  return (level == 1 || level == 2) && x < 2 && y < 2;
}

inline Color indicatorColor(uint8_t level) {
  if (level == 1) return {96, 67, 0};
  if (level == 2) return {96, 6, 0};
  return {64, 64, 64};
}

}  // namespace air_alert
