#pragma once
#include <cstdint>
#include <array>
#include <cmath>
#include "quota_provider.hpp"

namespace sky_palette {
constexpr uint32_t codexDefault = 0x5089cc;
constexpr uint32_t claudeDefault = 0xff8039;

// Upper half of the authored 50 x 20 rim: the ellipse starts darkening at
// radius .96 around (24.5, 20), using horizontal/vertical radii of 28 and 23.
inline bool upperSkyRim(unsigned x, unsigned y) {
  constexpr uint8_t lastLeft[] = {13, 10, 8, 7, 5, 4, 3, 2, 1, 1};
  return y < sizeof(lastLeft) && (x <= lastLeft[y] || x >= 49U - lastLeft[y]);
}

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
      const uint32_t distance = phase > 100000U ? phase - 100000U : 100000U - phase;
      // Fade the last sliver of daylight at the horizon into a black background.
      const float night = distance < 10000U ? float(distance) / 10000.0f : 1.0f;
      horizon_ = night * night * (3.0f - 2.0f * night);
      phase_ = phase;
    }
    float day = ((float(x) - 24.5f) * nx_ + (float(y) - 18.5f) * ny_ + 2.0f) * .25f;
    if (day <= 0.0f) return 0;
    if (day >= 1.0f) return uint8_t(horizon_ * 255.0f + 0.5f);
    day = day * day * (3.0f - 2.0f * day);
    return uint8_t(day * horizon_ * 255.0f + 0.5f);
  }

 private:
  uint32_t phase_ = UINT32_MAX;
  float nx_ = 0.0f, ny_ = 0.0f, horizon_ = 1.0f;
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

// Cache the minimum visible chromatic level once per color/brightness change.
// Two PWM counts retain a dim secondary channel under ordinary current scaling.
// Very small accent channels (< 1/8 peak) do not lift the whole sky.
class HueFloor {
 public:
  uint8_t at(uint32_t color, uint8_t brightness) {
    if (valid_ && color == color_ && brightness == brightness_) return peak_;
    valid_ = true; color_ = color; brightness_ = brightness; peak_ = 0;
    if (!brightness) return 0;
    const unsigned red = (color >> 16) & 255;
    const unsigned green = (color >> 8) & 255;
    const unsigned blue = color & 255;
    const unsigned maximum = red > green ? (red > blue ? red : blue)
                                          : (green > blue ? green : blue);
    if (!maximum) return 0;
    const unsigned target = brightness < 2 ? 1 : 2;
    for (unsigned peak = 1; peak <= 255; ++peak) {
      bool visible = true;
      for (unsigned channel = 0; channel < 3; ++channel) {
        const unsigned component = (color >> (16 - channel * 8)) & 255;
        if (!component || component * 8 < maximum) continue;
        const unsigned ratio = (component * 255 + maximum / 2) / maximum;
        const unsigned value = (peak * inverseGamma2[ratio] + 127) / 255;
        const unsigned staged = brightness == 255 ? value : value * (brightness + 1) / 256;
        const unsigned normalized = (staged * 255 + brightness / 2) / brightness;
        const unsigned curved = (normalized * normalized + 127) / 255;
        const unsigned output = (curved * brightness + 127) / 255;
        if (output < target) visible = false;
      }
      if (visible) { peak_ = uint8_t(peak); break; }
    }
    return peak_;
  }
 private:
  uint32_t color_ = 0;
  uint8_t brightness_ = 0, peak_ = 0;
  bool valid_ = false;
};

struct Palette {
  unsigned maximum = 0;
  uint8_t weights[3] = {};
  explicit Palette(uint32_t color) {
    const unsigned component[] = {(color >> 16) & 255, (color >> 8) & 255, color & 255};
    for (unsigned value : component) if (value > maximum) maximum = value;
    if (!maximum) return;
    for (unsigned i = 0; i < 3; ++i)
      weights[i] = inverseGamma2[(component[i] * 255 + maximum / 2) / maximum];
  }
};

class PaletteCache {
 public:
  const Palette &at(uint32_t color) {
    if (color != color_) { palette_ = Palette(color); color_ = color; }
    return palette_;
  }
 private:
  uint32_t color_ = 0;
  Palette palette_{0};
};

inline void colorize(uint8_t *rgb, uint8_t type, bool fixed, const Palette &palette,
                     uint8_t minimumPeak = 0, bool preserveSkyRim = false) {
  const unsigned cool = rgb[1] > rgb[2] ? rgb[1] : rgb[2];
  unsigned base = 0, light = 0;
  if (!type && (!fixed || cool > rgb[0])) {
    light = (unsigned(rgb[0]) * 54 + unsigned(rgb[1]) * 183 + unsigned(rgb[2]) * 19 + 128) / 256;
  } else if (type && cool > rgb[0]) {
    base = rgb[0] < rgb[1] ? rgb[0] : rgb[1];
    if (rgb[2] < base) base = rgb[2];
    light = cool - rgb[0];
  } else return;
  const unsigned maximum = palette.maximum;
  // Keep the authored peak envelope, including headroom for neutral cores.
  unsigned authoredPeak = (light * 3 * maximum + 255) / 510;
  // Only nonblack, nonfixed sky gets the hue floor. Objects and night stay intact.
  if (!type && preserveSkyRim) {
    // Hue compensation normally lifts dark daytime sky. At the rim use the
    // authored peak instead so the warm palette retains the clipped silhouette.
    const unsigned sourcePeak = rgb[0] > cool ? rgb[0] : cool;
    authoredPeak = (sourcePeak * maximum + 127) / 255;
  } else if (!type && !fixed && light && maximum && authoredPeak < minimumPeak)
    authoredPeak = minimumPeak;
  const unsigned peak = authoredPeak > 255 - base ? 255 - base : authoredPeak;
  for (unsigned channel = 0; channel < 3; ++channel) {
    rgb[channel] = uint8_t(base + (peak * palette.weights[channel] + 127) / 255);
  }
}

inline void colorize(uint8_t *rgb, uint8_t type, bool fixed, uint32_t color,
                     uint8_t minimumPeak = 0) {
  colorize(rgb, type, fixed, Palette(color), minimumPeak);
}

inline void apply(uint8_t *rgb, uint8_t type, bool fixed, uint8_t amount,
                  const Palette *codex, const Palette &claude, uint8_t daylight = 255,
                  uint8_t claudeMinimumPeak = 0, bool preserveSkyRim = false) {
  const bool sky = !type && !fixed;
  if (!type && !daylight) { rgb[0] = rgb[1] = rgb[2] = 0; return; }
  if (!amount) {
    if (codex) colorize(rgb, type, fixed, *codex);
    if (sky && daylight != 255)
      for (unsigned c = 0; c < 3; ++c) rgb[c] = uint8_t((unsigned(rgb[c]) * daylight + 127) / 255);
    return;
  }
  if (amount == 255 && daylight == 255) {
    colorize(rgb, type, fixed, claude, claudeMinimumPeak, preserveSkyRim);
    return;
  }
  uint8_t first[3] = {rgb[0], rgb[1], rgb[2]};
  uint8_t second[3] = {rgb[0], rgb[1], rgb[2]};
  if (codex && amount != 255) colorize(first, type, fixed, *codex);
  colorize(second, type, fixed, claude, claudeMinimumPeak, preserveSkyRim);
  for (unsigned channel = 0; channel < 3; ++channel) {
    const unsigned dayColor = (unsigned(rgb[channel]) * (255 - daylight) +
                               unsigned(second[channel]) * daylight + 127) / 255;
    const unsigned mixed = (unsigned(first[channel]) * (255 - amount) + dayColor * amount + 127) / 255;
    rgb[channel] = uint8_t(sky ? (mixed * daylight + 127) / 255 : mixed);
  }
}

inline void apply(uint8_t *rgb, uint8_t type, bool fixed, uint8_t amount,
                  uint32_t codex, uint32_t claude, uint8_t daylight = 255,
                  uint8_t claudeMinimumPeak = 0) {
  const Palette cool(codex), warm(claude);
  apply(rgb, type, fixed, amount, codex == codexDefault ? nullptr : &cool,
        warm, daylight, claudeMinimumPeak);
}
}  // namespace sky_palette
