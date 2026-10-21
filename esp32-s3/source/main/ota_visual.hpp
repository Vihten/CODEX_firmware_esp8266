#pragma once

#include <cstdint>

namespace ota_visual {

struct Pixel { uint8_t red, green, blue; };

constexpr unsigned perimeter(unsigned width, unsigned height) {
  return width < 2 || height < 2 ? 0 : 2 * (width + height) - 4;
}

// Clockwise, without counting the corner LEDs twice.
constexpr int position(unsigned x, unsigned y, unsigned width, unsigned height) {
  if (!perimeter(width, height) || x >= width || y >= height) return -1;
  if (y == 0) return int(x);
  if (x == width - 1) return int(width + y - 1);
  if (y == height - 1) return int(width + height - 2 + width - 1 - x);
  if (x == 0) return int(2 * width + height - 3 + height - 1 - y);
  return -1;
}

inline Pixel pixel(unsigned x, unsigned y, unsigned width, unsigned height,
                   uint8_t percent, uint32_t now) {
  const int index = position(x, y, width, height);
  if (index < 0) return {};
  const unsigned count = perimeter(width, height);
  const unsigned progress = percent > 100 ? 100 : percent;
  if (progress == 100) return {24, 190, 92};
  const unsigned filled = count * progress / 100;
  Pixel color = unsigned(index) < filled ? Pixel{12, 128, 190} : Pixel{5, 10, 17};
  const unsigned head = (now / 65) % count;
  const unsigned distance = (head + count - unsigned(index)) % count;
  if (distance < 5) {
    const unsigned strength = 5 - distance;
    color.red = uint8_t(color.red + strength * 8);
    color.green = uint8_t(color.green + strength * 10);
    color.blue = uint8_t(color.blue + strength * 10);
  }
  return color;
}

}  // namespace ota_visual
