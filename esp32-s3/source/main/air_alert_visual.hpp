#pragma once

#include <cstdint>

namespace air_alert {
constexpr uint32_t kTransitionMs = 600;
constexpr unsigned kIconWidth = 13, kIconHeight = 8;

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
    // Verified active with unspecified cause is distinct from unavailable data.
    level = level == 1 || level == 2 || level == 4 ? level : 0;
    if (level != level_) {
      level_ = level;
      if (level) glyphLevel_ = level;
      from_ = to_ = current_;
      to_.amount = level ? 1.0f : 0.0f;
      if (level == 1) { to_.red = 96; to_.green = 67; to_.blue = 0; }
      if (level == 2) { to_.red = 96; to_.green = 6; to_.blue = 0; }
      if (level == 4) { to_.red = 80; to_.green = 44; to_.blue = 0; }
      startedAt_ = now;
    }
    return current_;
  }
  Overlay overlay() const { return current_; }
  uint8_t glyphLevel() const { return glyphLevel_; }

 private:
  Overlay from_, to_, current_;
  uint32_t startedAt_ = 0;
  uint8_t level_ = 0;
  uint8_t glyphLevel_ = 0;
};

inline bool cornerPixel(unsigned x, unsigned y, unsigned width, unsigned height) {
  if (x >= width || y >= height || y >= kIconHeight) return false;
  const unsigned extent = kIconWidth < width / 2 ? kIconWidth : width / 2;
  return x < extent || x >= width - extent;
}

inline bool indicatorVisible(float amount, bool on, bool maintenance, uint8_t brightness) {
  return amount > 0 && on && !maintenance && brightness;
}

inline bool iconPixel(uint8_t level, unsigned x, unsigned y) {
  if (x >= kIconWidth || y >= kIconHeight) return false;
  // Native silhouettes for 7 x 12 mm pitch: solid delta wing versus a long
  // missile body with short mid-wings. Both noses point straight down at (6,6).
  static constexpr char drone[kIconHeight][kIconWidth + 1] = {
      ".............", "..#...#...#..", "..#########..", "...#######...",
      "....#####....", ".....###.....", "......#......", "............."};
  static constexpr char rocket[kIconHeight][kIconWidth + 1] = {
      ".............", "....#####....", "......#......", "...#######...",
      "......#......", "......#......", "......#......", "............."};
  static constexpr char unspecified[kIconHeight][kIconWidth + 1] = {
      ".............", "......##.....", "......##.....", "......##.....",
      "......##.....", ".............", "......##.....", "............."};
  const auto *glyph = level == 1 ? drone : level == 2 ? rocket :
      level == 4 ? unspecified : nullptr;
  return glyph && glyph[y][x] == '#';
}

inline bool iconBackdrop(uint8_t level, unsigned x, unsigned y) {
  if (x >= kIconWidth || y >= kIconHeight) return false;
  for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx)
    if (iconPixel(level, unsigned(int(x) + dx), unsigned(int(y) + dy))) return true;
  return false;
}

// The background is already brightness-scaled GRB. Never rescale the scene.
inline uint8_t composite(uint8_t background, float channel, float amount, uint8_t brightness) {
  const unsigned raw = unsigned(channel + .5f);
  const unsigned target = brightness == 255 ? raw : (raw * (brightness + 1)) >> 8;
  return uint8_t(background * (1 - amount) + target * amount + .5f);
}
}  // namespace air_alert
