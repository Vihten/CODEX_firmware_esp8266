#pragma once
#include <cstdint>
#include <cmath>
namespace meteor_style {
inline uint32_t ageTicks(bool claude, uint32_t ticks) {
  return claude ? uint32_t(uint64_t(ticks) * 4 / 5) : ticks;
}
inline float age(bool claude, float ticks) { return claude ? ticks * .8f : ticks; }
inline uint8_t tail(bool claude, uint8_t length) {
  return claude ? uint8_t((unsigned(length) * 2 + 1) / 3) : length;
}
inline uint8_t headOpacity(float age, uint16_t sequence) {
  return uint8_t(94 + 6 * std::sin(age * .34f + sequence * .71f));
}
}  // namespace meteor_style
