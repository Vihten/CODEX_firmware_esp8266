#pragma once
#include <cstdint>
#include <array>
#include <cmath>
#include "quota_provider.hpp"

namespace sky_palette {
constexpr uint32_t codexDefault = 0x5089cc;
constexpr uint32_t claudeDefault = 0xff8039;

// Renderer-owned cache: exactly two trig evaluations when the phase changes,
// then only arithmetic for the fixed 50 x 20 generator coordinates.
class Daylight {
 public:
  uint8_t at(uint32_t phase, unsigned x, unsigned y) {
    phase %= 200000U;
    if (phase != phase_) {
      const float angle = -float(phase) * 6.28318530718f / 200000.0f;
      nx_ = std::sin(angle);
      ny_ = -1.85f * std::cos(angle);
      phase_ = phase;
    }
    float day = ((float(x) - 24.5f) * nx_ + (float(y) - 18.5f) * ny_ + 2.0f) * .25f;
    if (day <= 0.0f) return 0;
    if (day >= 1.0f) return 255;
    day = day * day * (3.0f - 2.0f * day);
    return uint8_t(day * 255.0f + 0.5f);
  }

 private:
  uint32_t phase_ = UINT32_MAX;
  float nx_ = 0.0f, ny_ = 0.0f;
};

// sqrt(ratio) precompensates the output driver's per-channel gamma 2.
// Compile-time, 256 bytes; no runtime sqrt, mutable cache or allocation.
constexpr std::array<uint8_t, 256> makeInverseGamma2() {
  std::array<uint8_t, 256> result{};
  for (unsigned i = 0; i < result.size(); ++i) {
    unsigned root = 0;
    while (root < 255 && (root + 1) * (root + 1) <= i * 255) ++root;
    if (root < 255 && i * 255 - root * root > root) ++root;
    result[i] = uint8_t(root);
  }
  return result;
}
inline constexpr auto inverseGamma2 = makeInverseGamma2();

inline void colorize(uint8_t *rgb, uint8_t type, bool fixed, uint32_t color) {
  const unsigned cool = rgb[1] > rgb[2] ? rgb[1] : rgb[2];
  unsigned base = 0, light = 0;
  if (!type && (!fixed || cool > rgb[0])) {
    light = (unsigned(rgb[0]) * 54 + unsigned(rgb[1]) * 183 + unsigned(rgb[2]) * 19 + 128) / 256;
  } else if (type && cool > rgb[0]) {
    base = rgb[0] < rgb[1] ? rgb[0] : rgb[1];
    if (rgb[2] < base) base = rgb[2];
    light = cool - rgb[0];
  } else return;
  const unsigned red = (color >> 16) & 255;
  const unsigned green = (color >> 8) & 255;
  const unsigned blue = color & 255;
  const unsigned maximum = red > green ? (red > blue ? red : blue)
                                        : (green > blue ? green : blue);
  // Keep the authored peak envelope, including headroom for neutral cores.
  const unsigned authoredPeak = (light * 3 * maximum + 255) / 510;
  const unsigned peak = authoredPeak > 255 - base ? 255 - base : authoredPeak;
  for (unsigned channel = 0; channel < 3; ++channel) {
    const unsigned component = (color >> (16 - channel * 8)) & 255;
    const unsigned ratio = maximum ? (component * 255 + maximum / 2) / maximum : 0;
    const unsigned value = base + (peak * inverseGamma2[ratio] + 127) / 255;
    rgb[channel] = uint8_t(value > 255 ? 255 : value);
  }
}

inline void apply(uint8_t *rgb, uint8_t type, bool fixed, uint8_t amount,
                  uint32_t codex, uint32_t claude, uint8_t daylight = 255) {
  uint8_t first[3] = {rgb[0], rgb[1], rgb[2]};
  uint8_t second[3] = {rgb[0], rgb[1], rgb[2]};
  if (codex != codexDefault) colorize(first, type, fixed, codex);
  colorize(second, type, fixed, claude);
  for (unsigned channel = 0; channel < 3; ++channel) {
    const unsigned dayColor = (unsigned(rgb[channel]) * (255 - daylight) +
                               unsigned(second[channel]) * daylight + 127) / 255;
    const unsigned mixed = (unsigned(first[channel]) * (255 - amount) + dayColor * amount + 127) / 255;
    rgb[channel] = uint8_t(!type && !fixed ? (mixed * daylight + 127) / 255 : mixed);
  }
}
inline uint8_t nightBlue(uint8_t current, uint8_t minimum, uint8_t amount, uint32_t codex,
                         uint8_t daylight = 255) {
  const uint8_t warm = uint8_t((unsigned(amount) * daylight + 127) / 255);
  return codex == codexDefault ? quota_provider::nightBlue(current, minimum, warm) : current;
}
}  // namespace sky_palette
