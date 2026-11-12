#pragma once
#include <cstdint>
#include "quota_provider.hpp"

namespace sky_palette {
constexpr uint32_t codexDefault = 0x5089cc;
constexpr uint32_t claudeDefault = 0xff8039;

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
  for (unsigned channel = 0; channel < 3; ++channel) {
    const unsigned component = (color >> (16 - channel * 8)) & 255;
    const unsigned value = base + (light * 3 * component + 255) / 510;
    rgb[channel] = uint8_t(value > 255 ? 255 : value);
  }
}

inline void apply(uint8_t *rgb, uint8_t type, bool fixed, uint8_t amount,
                  uint32_t codex, uint32_t claude) {
  if (codex == codexDefault && claude == claudeDefault) {
    quota_provider::tint(rgb, type, fixed, amount);
    return;
  }
  uint8_t first[3] = {rgb[0], rgb[1], rgb[2]};
  uint8_t second[3] = {rgb[0], rgb[1], rgb[2]};
  if (codex != codexDefault) colorize(first, type, fixed, codex);
  if (claude == claudeDefault) quota_provider::tint(second, type, fixed, 255);
  else colorize(second, type, fixed, claude);
  for (unsigned channel = 0; channel < 3; ++channel)
    rgb[channel] = uint8_t((unsigned(first[channel]) * (255 - amount) + unsigned(second[channel]) * amount + 127) / 255);
}
inline uint8_t nightBlue(uint8_t current, uint8_t minimum, uint8_t amount, uint32_t codex) {
  return codex == codexDefault ? quota_provider::nightBlue(current, minimum, amount) : current;
}
}  // namespace sky_palette
