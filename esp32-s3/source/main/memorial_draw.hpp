#pragma once

#include <cmath>
#include <cstdint>

namespace memorial {
constexpr int kTextWidth = 42;
constexpr int kTextHeight = 7;

inline bool active(uint32_t now, uint32_t startedAt, uint32_t durationMs) {
  return uint32_t(now - startedAt) < durationMs;
}

struct Pixel {
  uint8_t red, green, blue;
};
struct Palette {
  Pixel background, text;
  Pixel wax, waxShade, waxHighlight, wick, core, edge;
  float lean;
};

inline Palette palette(uint32_t elapsedMs, uint32_t durationMs) {
  if (elapsedMs >= durationMs) return {};
  const uint32_t remaining = durationMs - elapsedMs;
  const uint32_t edge = elapsedMs < remaining ? elapsedMs : remaining;
  const float ramp = edge < 1000 ? edge / 1000.0f : 1.0f;
  const float fade = ramp * ramp * (3.0f - 2.0f * ramp);
  const auto channel = [fade](unsigned value) {
    return uint8_t(value * fade + .5f);
  };
  const auto color = [channel](unsigned r, unsigned g, unsigned b) {
    return Pixel{channel(r), channel(g), channel(b)};
  };
  const auto background = [fade](unsigned channel) {
    return uint8_t(std::ceil(channel * fade));
  };
  return {{background(1), background(3), background(9)},
          color(232, 219, 190), color(184, 170, 135),
          color(118, 104, 78), color(218, 205, 173), color(95, 53, 20),
          color(255, 191, 66), color(229, 89, 14),
          .5f * std::sin(float(elapsedMs % 4000) * .0015707963267948966f)};
}

inline Pixel blend(Pixel under, Pixel over, float coverage) {
  const float amount = coverage < 0 ? 0 : coverage > 1 ? 1 : coverage;
  const auto channel = [amount](uint8_t a, uint8_t b) {
    return uint8_t(float(a) + (float(b) - float(a)) * amount + .5f);
  };
  return {channel(under.red, over.red), channel(under.green, over.green),
          channel(under.blue, over.blue)};
}

// Logical coordinates; wiring and the user's global brightness belong to the caller.
inline Pixel pixel(int x, int y, int width, int height, const Palette &colors) {
  if (x < 0 || x >= width || y < 0 || y >= height) return colors.background;
  const int candleX = x - (width / 2 - 3);
  if (y >= 14 && y <= 18 && candleX >= 0 && candleX < 6) {
    if (candleX == 0 || candleX == 5 || y == 18) return colors.waxShade;
    return candleX == 2 || candleX == 3 ? colors.waxHighlight : colors.wax;
  }
  if (y == 13 && candleX >= 2 && candleX <= 3) return colors.wick;
  if (y >= 9 && y <= 12) {
    // Horizontal radii compensate for the taller physical LED pitch. Only the
    // flame leans; soft coverage is clipped to these four rows, clear of text.
    constexpr float radius[] = {1, 2, 3, 2};
    constexpr float coreRadius[] = {.25f, .75f, 1.5f, 1};
    const int flameRow = y - 9;
    const float center = 2.5f + colors.lean * (4 - flameRow) * .25f;
    const float distance = std::fabs(float(candleX) - center);
    const Pixel flame = blend(colors.edge, colors.core, coreRadius[flameRow] + .5f - distance);
    return blend(colors.background, flame, radius[flameRow] + .5f - distance);
  }
  const int localX = x - (width - kTextWidth) / 2;
  const int row = y - 1;
  if (row < 0 || row >= kTextHeight) return colors.background;
  constexpr uint8_t nine[7] = {14, 17, 17, 15, 1, 1, 14};
  constexpr uint8_t zero[7] = {14, 17, 17, 17, 17, 17, 14};
  bool lit = false;
  if (localX >= 0 && localX < 10) lit = nine[row] & (1U << (4 - localX / 2));
  else if (localX >= 14 && localX < 16) lit = row == 2 || row == 4;
  else if (localX >= 20 && localX < 30) lit = zero[row] & (1U << (4 - (localX - 20) / 2));
  else if (localX >= 32 && localX < 42) lit = zero[row] & (1U << (4 - (localX - 32) / 2));
  return lit ? colors.text : colors.background;
}

template <typename SetPixel>
inline void draw(int width, int height, uint32_t elapsedMs, uint32_t durationMs, SetPixel setPixel) {
  const Palette colors = palette(elapsedMs, durationMs);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) setPixel(x, y, pixel(x, y, width, height, colors));
  }
}
}  // namespace memorial
