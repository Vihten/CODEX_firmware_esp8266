#pragma once

#include <cstdint>

namespace air_alert {

constexpr uint32_t kTransitionMs = 600;
constexpr float kSkyTintStrength = .35f;

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
      to_.amount = level == 1 || level == 2 ? kSkyTintStrength : 0.0f;
      if (level == 1) { to_.red = 1.8f; to_.green = 1.2f; to_.blue = 0; }
      if (level == 2) { to_.red = 1.8f; to_.green = .06f; to_.blue = 0; }
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
  // A color grade, not a luminous overlay: retain darkness and object contrast.
  if (!peak || peak > 64 || (rgb[0] >= rgb[1] && rgb[1] > rgb[2] && peak > 24)) return;
  const float palette[] = {tint.red, tint.green, tint.blue};
  for (unsigned channel = 0; channel < 3; ++channel)
    rgb[channel] = uint8_t(rgb[channel] * (1 - tint.amount) +
                           peak * palette[channel] * tint.amount + .5f);
}

inline void tintBackdrop(uint8_t *rgb, const uint8_t *background,
                         const Tint &tint) {
  if (tint.amount <= 0) return;
  uint8_t graded[3] = {background[0], background[1], background[2]};
  tintSky(graded, 0, false, tint);
  // Reconstruct only the sky contribution to pale anti-aliased edges. Opaque
  // white cores have alpha 1 and remain unchanged; emission keeps its detail.
  float alpha = 1;
  for (unsigned channel = 0; channel < 3; ++channel) {
    if (background[channel] == 255) continue;
    const float coverage = float(int(rgb[channel]) - background[channel]) /
                           (255 - background[channel]);
    if (coverage < alpha) alpha = coverage;
  }
  if (alpha < 0) alpha = 0;
  for (unsigned channel = 0; channel < 3; ++channel) {
    const float value = rgb[channel] + (1 - alpha) *
        (int(graded[channel]) - background[channel]);
    rgb[channel] = value < 0 ? 0 : value > 255 ? 255 : uint8_t(value + .5f);
  }
}

inline bool indicatorVisible(uint8_t level, bool /* alternateScreen */, bool on,
                             bool maintenance, uint8_t brightness) {
  level = normalizedLevel(level);
  return on && !maintenance && brightness && level;
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
