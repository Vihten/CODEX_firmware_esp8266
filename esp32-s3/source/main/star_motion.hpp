#pragma once
#include <cstdint>

namespace star_motion {
struct Pixel {
  uint8_t r, g, b, type;
  bool changed;
};

namespace detail {
inline uint32_t seed(int x, int y) {
  uint32_t value = uint32_t(x) * 0x9e3779b9U ^ uint32_t(y) * 0x85ebca6bU ^ 17U;
  value ^= value >> 16;
  value *= 0x7feb352dU;
  value ^= value >> 15;
  return value;
}

// Q8 smoothstep has zero slope at both ends. All intermediates fit uint32_t.
inline unsigned ease(unsigned value) {
  return (value * value * (768U - 2U * value) + 32768U) >> 16;
}

inline unsigned phase(uint64_t time, unsigned periodTicks, uint32_t offset) {
  const unsigned period = periodTicks * 256U;
  return unsigned((time % period + offset % period) % period);
}

inline int wave(uint64_t time, unsigned periodTicks, uint32_t offset) {
  const unsigned period = periodTicks * 256U;
  const unsigned position = phase(time, periodTicks, offset);
  const unsigned triangle = position < period / 2
      ? position * 512U / period : (period - position) * 512U / period;
  return int(ease(triangle)) * 2 - 256;
}

inline unsigned gain(uint64_t time, uint32_t identity) {
  const unsigned cycleTicks = 240U + identity % 301U;  // 8–18 seconds at 30 Hz.
  const unsigned dark = (30U + (identity >> 9) % 61U) * 256U;
  const unsigned ramp = 30U * 256U;
  const unsigned position = phase(time, cycleTicks, identity * 97U);
  unsigned visibility = 256;
  if (position < dark) visibility = 0;
  else if (position < dark + ramp) visibility = ease((position - dark) * 256U / ramp);
  else if (position > cycleTicks * 256U - ramp)
    visibility = ease((cycleTicks * 256U - position) * 256U / ramp);

  const int twinkle = wave(time, 45U + (identity >> 3) % 76U, identity * 31U);
  const unsigned base = unsigned(218 + twinkle * 37 / 256);
  const unsigned flarePhase = phase(time, 420U + (identity >> 13) % 331U, identity * 53U);
  const unsigned flareDuration = 36U * 256U;
  unsigned flare = 0;
  if (flarePhase < flareDuration) {
    const unsigned triangle = flarePhase < flareDuration / 2
        ? flarePhase * 512U / flareDuration
        : (flareDuration - flarePhase) * 512U / flareDuration;
    flare = ease(triangle) * 96U / 256U;
  }
  return (base + flare) * visibility / 256U;
}

template <typename PixelReader>
Pixel background(int x, int y, int width, int height, PixelReader &reader) {
  // Nearest clear sky along the four cardinal rays, bounded to twelve reads.
  for (int distance = 1; distance <= 3; ++distance) {
    for (int direction = 0; direction < 4; ++direction) {
      const int px = x + (direction == 0 ? -distance : direction == 1 ? distance : 0);
      const int py = y + (direction == 2 ? -distance : direction == 3 ? distance : 0);
      if (px < 0 || py < 0 || px >= width || py >= height) continue;
      const auto pixel = reader(px, py);
      if (pixel.type == 0) return {pixel.r, pixel.g, pixel.b, 0, false};
    }
  }
  return {0, 0, 0, 0, false};
}

inline uint64_t component(uint8_t color, uint8_t sky, unsigned weight, unsigned gain) {
  return color > sky ? uint64_t(color - sky) * weight * gain : 0;
}
inline uint8_t channel(uint8_t sky, uint64_t light) {
  const uint64_t value = unsigned(sky) + (light + 8355840U) / 16711680U;
  return uint8_t(value > 255U ? 255U : value);
}
}  // namespace detail

// timeQ8 is a 30 Hz effect tick with eight fractional bits. Readers are immutable
// artwork samples for one scene frame. Callers exclude fixed foreground pixels.
template <typename PixelReader>
Pixel sample(int x, int y, int width, int height, uint64_t timeQ8, PixelReader reader) {
  if (x < 0 || y < 0 || x >= width || y >= height) return {0, 0, 0, 0, false};
  const auto original = reader(x, y);
  Pixel result{original.r, original.g, original.b, original.type, false};
  if (original.type != 0 && original.type != 1) return result;
  const Pixel sky = original.type == 1
      ? detail::background(x, y, width, height, reader)
      : Pixel{original.r, original.g, original.b, 0, false};
  uint64_t red = 0, green = 0, blue = 0;
  for (int sy = y - 1; sy <= y + 1; ++sy) {
    if (sy < 0 || sy >= height) continue;
    for (int sx = x - 1; sx <= x + 1; ++sx) {
      if (sx < 0 || sx >= width) continue;
      const auto source = reader(sx, sy);
      if (source.type != 1) continue;
      result.changed = true;
      const uint32_t identity = detail::seed(sx, sy);
      const int dx = detail::wave(timeQ8, 173U + identity % 107U, identity * 19U) * 166 / 256;
      const int dy = detail::wave(timeQ8, 211U + identity % 139U, identity * 43U) * 64 / 256;
      int wx = (x - sx) * 256 - dx;
      int wy = (y - sy) * 256 - dy;
      wx = wx < 0 ? -wx : wx;
      wy = wy < 0 ? -wy : wy;
      if (wx >= 256 || wy >= 256) continue;
      const unsigned weight = unsigned(256 - wx) * unsigned(256 - wy);
      const unsigned gain = detail::gain(timeQ8, identity);
      const auto background = detail::background(sx, sy, width, height, reader);
      red += detail::component(source.r, background.r, weight, gain);
      green += detail::component(source.g, background.g, weight, gain);
      blue += detail::component(source.b, background.b, weight, gain);
    }
  }
  if (!result.changed) return result;
  result.r = detail::channel(sky.r, red);
  result.g = detail::channel(sky.g, green);
  result.b = detail::channel(sky.b, blue);
  result.type = result.r != sky.r || result.g != sky.g || result.b != sky.b ? 1 : 0;
  return result;
}
}  // namespace star_motion
