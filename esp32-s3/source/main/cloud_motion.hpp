#pragma once
#include <cstdint>

namespace cloud_motion {
struct Pixel { uint8_t r, g, b, type; bool changed; };

// Readers use immutable, unshifted artwork and are called only in bounds.
// pixelReader(x,y) returns Pixel or any RGB value exposing .r/.g/.b.
// maskReader(x,y) supplies the authoritative type. Positive Q8 moves right.
// Caller owns timing, fixed-artwork selection, and brightness scaling.
template <class PixelReader, class MaskReader>
Pixel sample(int x, int y, int width, int height, int offsetQ8,
             PixelReader pixelReader, MaskReader maskReader) {
  if (width <= 0 || height <= 0 || x < 0 || x >= width || y < 0 || y >= height)
    return Pixel{0, 0, 0, 0, false};
  const auto rgb = pixelReader(x, y);
  const uint8_t type = maskReader(x, y);
  const Pixel original{rgb.r, rgb.g, rgb.b, type, false};
  if (y >= height - 4 || (type >= 1 && type <= 3)) return original;
  const int shift = offsetQ8 < -256 ? -256 : offsetQ8 > 256 ? 256 : offsetQ8;
  const auto cloud = [&](int sx) {
    return sx >= 0 && sx < width && maskReader(sx, y) >= 64;
  };
  if (!cloud(x) && !cloud(x - 1) && !cloud(x + 1)) return original;

  // At most nine mask reads. Never probe the fixed bottom band for sun safety.
  const auto nearSun = [&](int sx, int sy) {
    for (int dy = -1; dy <= 1; ++dy) {
      const int yy = sy + dy;
      if (yy < 0 || yy >= height - 4) continue;
      for (int dx = -1; dx <= 1; ++dx) {
        const int xx = sx + dx;
        if (xx >= 0 && xx < width && maskReader(xx, yy) == 2) return true;
      }
    }
    return false;
  };
  Pixel background = original;
  // Nearest eligible sky/halo on each side, bounded to four rows. Interpolate
  // between both samples to preserve vertical sky/fog gradients.
  Pixel upper = original, lower = original;
  int up = 0, down = 0;
  for (int distance = 1; type >= 64 && distance <= 4; ++distance) {
    for (int side = 0; side < 2; ++side) {
      if (side == 0 ? up != 0 : down != 0) continue;
      const int yy = y + (side == 0 ? -distance : distance);
      if (yy < 0 || yy >= height - 4) continue;
      const uint8_t mask = maskReader(x, yy);
      if ((mask != 0 && (mask < 4 || mask >= 64)) || nearSun(x, yy)) continue;
      const auto color = pixelReader(x, yy);
      const Pixel sky{color.r, color.g, color.b, mask, false};
      if (side == 0) { upper = sky; up = distance; }
      else { lower = sky; down = distance; }
    }
  }
  if (up && down) {
    const auto mix = [&](uint8_t a, uint8_t b) -> uint8_t {
      return (unsigned(a) * down + unsigned(b) * up + (up + down) / 2) / (up + down);
    };
    background = Pixel{mix(upper.r, lower.r), mix(upper.g, lower.g),
                       mix(upper.b, lower.b), up <= down ? upper.type : lower.type, false};
  } else if (up) background = upper;
  else if (down) background = lower;

  // Inverse mapping avoids negative division rounding and reads two taps only.
  const int magnitude = shift < 0 ? -shift : shift;
  const int sources[2] = {x, x + (shift > 0 ? -1 : 1)};
  const unsigned weights[2] = {unsigned(256 - magnitude), unsigned(magnitude)};
  unsigned red = 0, green = 0, blue = 0, alpha = 0, strongest = 0;
  uint8_t movedType = background.type;
  if (!nearSun(x, y)) {
    for (int tap = 0; tap < 2; ++tap) {
      const int sx = sources[tap];
      if (!weights[tap] || !cloud(sx) || nearSun(sx, y)) continue;
      const auto color = pixelReader(sx, y);
      const unsigned weight = weights[tap];
      red += unsigned(color.r) * weight;
      green += unsigned(color.g) * weight;
      blue += unsigned(color.b) * weight;
      alpha += weight;
      if (weight > strongest) { strongest = weight; movedType = maskReader(sx, y); }
    }
  }
  Pixel result{
    uint8_t((red + unsigned(background.r) * (256 - alpha) + 128) >> 8),
    uint8_t((green + unsigned(background.g) * (256 - alpha) + 128) >> 8),
    uint8_t((blue + unsigned(background.b) * (256 - alpha) + 128) >> 8),
    movedType, false};
  result.changed = result.r != original.r || result.g != original.g ||
                   result.b != original.b || result.type != original.type;
  return result;
}
}  // namespace cloud_motion

