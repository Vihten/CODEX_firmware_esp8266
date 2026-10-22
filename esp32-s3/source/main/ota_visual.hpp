#pragma once

#include <cstdint>

namespace ota_visual {

struct Pixel { uint8_t red, green, blue; };

// Renderer-owned storage: place the instance in internal DRAM, not on the heap.
template <unsigned Count>
class Fill {
  static_assert(Count > 0, "Fill needs a nonempty perimeter");
 public:
  void reset(uint32_t now) {
    previous_ = now;
    percent_ = 0;
    for (unsigned i = 0; i < Count; ++i) {
      levels_[i] = starts_[i] = 0;
      ages_[i] = 0;
    }
  }

  void update(uint8_t percent, uint32_t now) {
    const unsigned progress = percent > 100 ? 100 : percent;
    const uint32_t elapsed = now - previous_;  // Unsigned subtraction survives wrap.
    const unsigned dt = elapsed > 34 ? 34 : unsigned(elapsed);
    previous_ = now;
    for (unsigned i = 0; i < Count; ++i) {
      const uint16_t target = targetAt(i, progress);
      if (progress == 100) {
        // Verified completion stays immediate for the existing one-shot barrier.
        levels_[i] = starts_[i] = 65535;
        ages_[i] = 180;
      } else if (target != targetAt(i, percent_)) {
        if (levels_[i] > target) levels_[i] = target;
        starts_[i] = levels_[i];
        ages_[i] = 0;  // New progress was observed now, not during the preceding dt.
      } else {
        const unsigned age = unsigned(ages_[i]) + dt;
        ages_[i] = uint8_t(age > 180 ? 180 : age);
        levels_[i] = uint16_t(starts_[i] +
            uint32_t(target - starts_[i]) * ages_[i] / 180);
      }
    }
    percent_ = uint8_t(progress);
  }

  uint16_t coverage(unsigned index) const {
    return index < Count ? levels_[index] : 0;
  }

 private:
  static uint16_t targetAt(unsigned index, unsigned percent) {
    const uint64_t units = uint64_t(Count) * percent;
    const uint64_t offset = uint64_t(index) * 100;
    if (units <= offset) return 0;
    const uint64_t fraction = units - offset;
    return fraction >= 100 ? 65535 : uint16_t(fraction * 65535 / 100);
  }

  uint16_t levels_[Count] = {};
  uint16_t starts_[Count] = {};
  uint8_t ages_[Count] = {};
  uint32_t previous_ = 0;
  uint8_t percent_ = 0;
};

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
                   uint8_t percent, uint32_t now, uint16_t coverage) {
  const int index = position(x, y, width, height);
  if (index < 0) return {};
  const unsigned count = perimeter(width, height);
  const unsigned progress = percent > 100 ? 100 : percent;
  if (progress == 100) return {24, 190, 92};
  Pixel color{uint8_t(uint32_t(12) * coverage / 65535),
              uint8_t(uint32_t(128) * coverage / 65535),
              uint8_t(uint32_t(190) * coverage / 65535)};
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
