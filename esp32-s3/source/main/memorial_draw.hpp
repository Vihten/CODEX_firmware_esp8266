#pragma once

#include <cmath>
#include <cstdint>

namespace memorial {
constexpr int kTextWidth = 20;
constexpr int kTextHeight = 5;
constexpr uint8_t kTextBlue = 190;

// Ukraine including Crimea, rasterized for the physical 7 mm x 12 mm LED pitch.
// Outline reference: johan/world.geo.json, countries/UKR.geo.json (Unlicense).
constexpr uint64_t kUkraineOutline[20] = {
    0x0000000000000ULL, 0x00000F0000000ULL, 0x000030F803F00ULL,
    0x0000100FFC0C0ULL, 0x0000600000040ULL, 0x00198000000C0ULL,
    0x01E6000000030ULL, 0x0E00000000008ULL, 0x0800000000018ULL,
    0x0400000000004ULL, 0x080000003F00CULL, 0x07000001C1FF0ULL,
    0x00C0000180000ULL, 0x0070000200000ULL, 0x000F007780000ULL,
    0x0000C7CAC0000ULL, 0x00064F0460000ULL, 0x00078F03C0000ULL,
    0x0001C80000000ULL, 0x00003C0000000ULL};

inline bool outline(int x, int y) {
  return x >= 0 && x < 50 && y >= 0 && y < 20 &&
      (kUkraineOutline[y] & (uint64_t(1) << x)) != 0;
}

inline bool active(uint32_t now, uint32_t startedAt, uint32_t durationMs) {
  return uint32_t(now - startedAt) < durationMs;
}

struct Pixel {
  uint8_t red, green, blue;
};
struct Palette {
  Pixel background, text;
  Pixel wax, waxShade, waxHighlight, wick, core, edge;
  Pixel borderUpper, borderLower;
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
  return {{0, 0, 0},
          color(232, 219, kTextBlue), color(184, 170, 135),
          color(118, 104, 78), color(218, 205, 173), color(95, 53, 20),
          color(255, 191, 66), color(229, 89, 14),
          color(63, 121, 184), color(173, 141, 57),
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

inline uint32_t luminance(Pixel color) {
  return uint32_t(color.red) * 54 + uint32_t(color.green) * 183 + uint32_t(color.blue) * 19;
}

inline uint8_t matchingGain(uint32_t reference, uint32_t scene) {
  if (!scene || reference >= scene) return 255;
  return uint8_t((uint64_t(reference) * 255 + scene / 2) / scene);
}

inline Pixel scalePixel(Pixel color, uint8_t gain) {
  const auto channel = [gain](uint8_t value) {
    return uint8_t((uint16_t(value) * gain + 127) / 255);
  };
  return {channel(color.red), channel(color.green), channel(color.blue)};
}

inline uint8_t readableGain(uint8_t gain, uint8_t brightness) {
  if (!gain || !brightness) return gain;
  // At very low global brightness, prevent the ivory text from quantizing
  // entirely to black. The shared LED brightness still supplies the final gain.
  const unsigned minimumChannel = (256U + brightness) / (unsigned(brightness) + 1);
  const unsigned floor = (minimumChannel * 255 + kTextBlue - 1) / kTextBlue;
  const unsigned bounded = floor > 255 ? 255 : floor;
  return uint8_t(gain > bounded ? gain : bounded);
}

// Logical coordinates; wiring and the user's global brightness belong to the caller.
inline Pixel pixel(int x, int y, int width, int height, const Palette &colors) {
  if (x < 0 || x >= width || y < 0 || y >= height) return colors.background;
  const int sourceX = x + 25 - width / 2;
  if (outline(sourceX, y)) return y < 10 ? colors.borderUpper : colors.borderLower;
  const int candleX = sourceX - 33;
  if (y >= 11 && y <= 13 && candleX >= 0 && candleX < 4) {
    if (candleX == 0 || candleX == 3 || y == 13) return colors.waxShade;
    return colors.waxHighlight;
  }
  if (y == 10 && candleX >= 1 && candleX <= 2) return colors.wick;
  if (y >= 7 && y <= 9) {
    // Horizontal radii compensate for the taller physical LED pitch. Only the
    // flame leans; soft coverage stays beside the fixed clock and off the border.
    constexpr float radius[] = {1, 2, 1.5f};
    constexpr float coreRadius[] = {.25f, 1, .75f};
    const int flameRow = y - 7;
    const float center = 1.5f + colors.lean * (3 - flameRow) / 3;
    const float distance = std::fabs(float(candleX) - center);
    const Pixel flame = blend(colors.edge, colors.core, coreRadius[flameRow] + .5f - distance);
    if (distance < radius[flameRow] + .5f)
      return blend(colors.background, flame, radius[flameRow] + .5f - distance);
  }
  const int localX = sourceX - 12;
  const int row = y - 5;
  if (row < 0 || row >= kTextHeight) return colors.background;
  constexpr uint8_t nine[5] = {14, 17, 15, 1, 14};
  constexpr uint8_t zero[5] = {14, 17, 17, 17, 14};
  bool lit = false;
  if (localX >= 0 && localX < 5) lit = nine[row] & (1U << (4 - localX));
  else if (localX == 7) lit = row == 1 || row == 3;
  else if (localX >= 9 && localX < 14) lit = zero[row] & (1U << (4 - (localX - 9)));
  else if (localX >= 15 && localX < 20) lit = zero[row] & (1U << (4 - (localX - 15)));
  return lit ? colors.text : colors.background;
}

template <typename SetPixel>
inline void draw(int width, int height, uint32_t elapsedMs, uint32_t durationMs, SetPixel setPixel) {
  const Palette colors = palette(elapsedMs, durationMs);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) setPixel(x, y, pixel(x, y, width, height, colors));
  }
}

inline float slideOffset(uint32_t elapsedMs, uint32_t slideDurationMs, int height) {
  if (height <= 0 || slideDurationMs == 0 || elapsedMs >= slideDurationMs) return 0;
  const float remaining = float(double(slideDurationMs - elapsedMs) / slideDurationMs);
  return -float(height) * remaining * remaining * remaining;
}

// Translate the entire scene down from above. Off-canvas samples are black;
// nonfinite offsets hide the scene. The caller owns both animation clocks.
template <typename SetPixel>
inline void drawSliding(int width, int height, uint32_t elapsedMs, uint32_t durationMs,
                        float offsetY, SetPixel setPixel) {
  if (width <= 0 || height <= 0) return;
  const Palette colors = palette(elapsedMs, durationMs);
  // Double keeps the clamped shift representable as int even at INT_MAX height.
  const double shift = !std::isfinite(offsetY) ? double(height) :
      offsetY >= 0 ? 0.0 : offsetY <= -double(height) ? double(height) : -double(offsetY);
  const int rowShift = int(shift);
  const float fraction = float(shift - rowShift);
  for (int y = 0; y < height; ++y) {
    const bool firstVisible = rowShift < height - y;
    const int sourceRow = firstVisible ? y + rowShift : 0;
    const bool secondVisible = firstVisible && sourceRow + 1 < height;
    for (int x = 0; x < width; ++x) {
      const Pixel first = firstVisible ? pixel(x, sourceRow, width, height, colors) : Pixel{};
      const Pixel result = fraction == 0 ? first : blend(first,
          secondVisible ? pixel(x, sourceRow + 1, width, height, colors) : Pixel{}, fraction);
      setPixel(x, y, result);
    }
  }
}
}  // namespace memorial
