#pragma once

#include <cmath>
#include <cstdint>

namespace subpixel {
struct Footprint {
  int16_t x, y;
  uint16_t weight[4];
};

inline Footprint footprint(float x, float y) {
  const int16_t left = static_cast<int16_t>(std::floor(x));
  const int16_t top = static_cast<int16_t>(std::floor(y));
  const uint16_t fx = static_cast<uint16_t>((x - left) * 256);
  const uint16_t fy = static_cast<uint16_t>((y - top) * 256);
  Footprint result{left, top, {}};
  result.weight[0] = ((256 - fx) * (256 - fy)) >> 8;
  result.weight[1] = (fx * (256 - fy)) >> 8;
  result.weight[2] = ((256 - fx) * fy) >> 8;
  result.weight[3] = 256 - result.weight[0] - result.weight[1] - result.weight[2];
  return result;
}

inline uint8_t mix(uint8_t a, uint8_t b, uint16_t fraction) {
  return (uint32_t(a) * (1000 - fraction) + uint32_t(b) * fraction + 500) / 1000;
}

inline bool light(uint8_t mask) { return mask == 2 || mask == 3 || mask >= 64; }

inline uint8_t mask(uint8_t a, uint8_t b, uint16_t fraction) {
  if (!fraction) return a;
  if (fraction == 1000) return b;
  if (light(a)) return a;
  if (light(b)) return b;
  return fraction < 500 ? a : b;
}

// Premultiplied linear color; compose once to avoid dim seams between body pixels.
struct Pixel {
  uint16_t red = 0, green = 0, blue = 0, alpha = 0;
  void add(uint8_t r, uint8_t g, uint8_t b, uint16_t coverage) {
    const auto sum = [](uint16_t old, uint32_t value) -> uint16_t {
      return value + old > 65280 ? 65280 : uint16_t(value + old);
    };
    red = sum(red, uint32_t(r) * coverage);
    green = sum(green, uint32_t(g) * coverage);
    blue = sum(blue, uint32_t(b) * coverage);
    alpha = alpha + coverage > 256 ? 256 : alpha + coverage;
  }
};

inline uint8_t composite(uint8_t background, uint16_t color, uint16_t alpha,
                         uint8_t brightness, bool additive = false) {
  const uint32_t scaled = brightness == 255 ? color : (uint32_t(color) * (brightness + 1)) >> 8;
  const uint32_t value = (uint32_t(background) * (additive ? 256 : 256 - alpha) + scaled + 128) >> 8;
  const uint8_t ceiling = background > brightness ? background : brightness;
  return value > ceiling ? ceiling : value;
}

inline float trajectory(float step, uint16_t total, uint16_t sequence, bool attributed) {
  const float p = step < 0 ? 0 : step > total ? 1 : step / total;
  if (attributed) {
    const unsigned variant = (sequence / 6) % 3;
    return 2 + 2 * (sequence % 6) + (variant == 1 ? 2 * p : variant == 2 ? -4 * p * (1 - p) : 0);
  }
  if (sequence % 2) return 2 + 13 * p;
  constexpr float pi = 3.14159265358979323846f;
  const unsigned variant = sequence % 3;
  const float phase = variant == 1 ? 85 : variant == 2 ? 171 : 0;
  return 9 + (variant == 0 ? -1 : variant == 1 ? 0 : 1) - 2.5f * std::sin(pi * p)
      + .25f * std::sin(4 * pi * p + phase * 2 * pi / 256);
}
}  // namespace subpixel
