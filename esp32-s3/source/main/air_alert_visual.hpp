#pragma once

#include <cstdint>

namespace air_alert {
constexpr uint32_t kTransitionMs = 600;

struct Overlay {
  float red = 0, green = 0, blue = 0, amount = 0;
};

class Visual {
 public:
  Overlay update(uint8_t level, uint32_t now) {
    const uint32_t elapsed = now - startedAt_;
    if (elapsed >= kTransitionMs) current_ = from_ = to_;
    else {
      const float t = float(elapsed) / kTransitionMs;
      current_ = {from_.red + (to_.red - from_.red) * t,
                  from_.green + (to_.green - from_.green) * t,
                  from_.blue + (to_.blue - from_.blue) * t,
                  from_.amount + (to_.amount - from_.amount) * t};
    }
    level = level <= 3 ? level : 3;
    if (level != level_) {
      level_ = level;
      from_ = to_ = current_;
      to_.amount = level ? 1.0f : 0.0f;
      if (level == 1) { to_.red = 96; to_.green = 67; to_.blue = 0; }
      if (level == 2) { to_.red = 96; to_.green = 6; to_.blue = 0; }
      if (level == 3) { to_.red = 40; to_.green = 40; to_.blue = 40; }
      startedAt_ = now;
    }
    return current_;
  }
  Overlay overlay() const { return current_; }

 private:
  Overlay from_, to_, current_;
  uint32_t startedAt_ = 0;
  uint8_t level_ = 0;
};

inline bool cornerPixel(unsigned x, unsigned y, unsigned width, unsigned height) {
  constexpr unsigned rows[] = {7, 5, 4, 3, 2, 1};
  if (x >= width || y >= height || y >= 6) return false;
  const unsigned extent = rows[y] < width / 2 ? rows[y] : width / 2;
  return x < extent || x >= width - extent;
}

inline bool indicatorVisible(float amount, bool on, bool maintenance, uint8_t brightness) {
  return amount > 0 && on && !maintenance && brightness;
}

// The background is already brightness-scaled GRB. Never rescale the scene.
inline uint8_t composite(uint8_t background, float channel, float amount, uint8_t brightness) {
  const unsigned raw = unsigned(channel + .5f);
  const unsigned target = brightness == 255 ? raw : (raw * (brightness + 1)) >> 8;
  return uint8_t(background * (1 - amount) + target * amount + .5f);
}
}  // namespace air_alert
