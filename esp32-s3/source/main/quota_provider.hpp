#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "quota_filter.hpp"

namespace quota_provider {
enum class Provider : uint8_t { Codex, Claude };
inline const char *name(Provider provider) { return provider == Provider::Claude ? "claude" : "codex"; }
constexpr uint32_t codexFreshMs = 15000, claudeFreshMs = 90000, rotationMs = 300000;
struct Snapshot { int16_t remaining = -1; bool known = false, fresh = false; };
struct Sample {
  uint32_t clientId = 0, seenAt = 0;
  int16_t remaining = -1;
  uint32_t validForMs = claudeFreshMs;
  bool fresh(uint32_t now, uint32_t ttl) const {
    return clientId && remaining >= 0 && remaining <= 100 && uint32_t(now - seenAt) < (ttl < validForMs ? ttl : validForMs);
  }
};
struct State {
  quota::Filter filter;
  uint8_t remaining = 100;
  Snapshot snapshot;
  bool observe(int value, bool reset, uint32_t now) {
    const bool recovered = filter.observe(value, reset, now, remaining);
    snapshot = {filter.initialized ? int16_t(remaining) : int16_t(-1), filter.initialized, value >= 0 && value <= 100};
    return recovered;
  }
};
struct Rotation {
  Provider active = Provider::Codex;
  uint32_t since = 0;
  bool started = false, bothFresh = false;
  Provider update(bool codexFresh, bool claudeFresh, uint32_t now) {
    if (!started) { started = true; since = now; }
    const bool both = codexFresh && claudeFresh;
    if (both && !bothFresh) since = now;
    bothFresh = both;
    Provider next = active;
    if (codexFresh != claudeFresh) next = claudeFresh ? Provider::Claude : Provider::Codex;
    else if (codexFresh) {
      const uint32_t turns = uint32_t(now - since) / rotationMs;
      if (turns & 1U) next = active == Provider::Codex ? Provider::Claude : Provider::Codex;
      if (turns) since += turns * rotationMs;
    }
    if (next != active) { active = next; if (!both) since = now; }
    return active;
  }
};
struct Packet { uint32_t clientId = 0; int16_t remaining = -1; uint32_t validForMs = 0; };
inline bool hex(const char *text, uint32_t &value) {
  if (std::strlen(text) != 8) return false;
  value = 0;
  for (unsigned i = 0; i < 8; ++i) {
    const char c = text[i];
    unsigned digit;
    if (c >= '0' && c <= '9') digit = unsigned(c - '0');
    else if (c >= 'a' && c <= 'f') digit = unsigned(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') digit = unsigned(c - 'A' + 10);
    else return false;
    value = (value << 4) | digit;
  }
  return true;
}
inline bool parse(const char *packet, uint32_t expectedAccount, Packet &result) {
  const size_t length = std::strlen(packet);
  if (length >= 64) return false;
  char buffer[64]; std::memcpy(buffer, packet, length + 1);
  char *fields[6] = {buffer}; unsigned count = 1;
  for (size_t i = 0; i < length; ++i) if (buffer[i] == ',') {
    if (count == 6) return false;
    buffer[i] = 0; fields[count++] = buffer + i + 1;
  }
  uint32_t account = 0, quotaAccount = 0, client = 0;
  if (count != 6 || std::strcmp(fields[0], "LFC1") || !hex(fields[1], account) ||
      account != expectedAccount || !hex(fields[2], client) || !client ||
      !hex(fields[4], quotaAccount)) return false;
  int value = 0;
  if (!std::strcmp(fields[3], "-1")) value = -1;
  else {
    const size_t digits = std::strlen(fields[3]);
    if (!digits || digits > 3) return false;
    for (size_t i = 0; i < digits; ++i) {
      if (fields[3][i] < '0' || fields[3][i] > '9') return false;
      value = value * 10 + fields[3][i] - '0';
    }
    if (value > 100) return false;
  }
  if (quotaAccount != expectedAccount && !(value == -1 && quotaAccount == 0)) return false;
  const size_t validityDigits = std::strlen(fields[5]);
  if (!validityDigits || validityDigits > 5) return false;
  uint32_t validForMs = 0;
  for (size_t i = 0; i < validityDigits; ++i) {
    if (fields[5][i] < '0' || fields[5][i] > '9') return false;
    validForMs = validForMs * 10 + unsigned(fields[5][i] - '0');
  }
  if (validForMs > claudeFreshMs || (value == -1 && validForMs != 0)) return false;
  result = {client, int16_t(value), validForMs};
  return true;
}
// Correct the sky and cool spill in its antialiased artwork after modulation.
// Neutral cloud cores and warm rays keep their existing color and detail.
inline void tint(uint8_t *rgb, uint8_t type, bool fixed, uint8_t amount) {
  if (!amount) return;
  const unsigned cool = rgb[1] > rgb[2] ? rgb[1] : rgb[2];
  unsigned target[3];
  if (!type && (!fixed || cool > rgb[0])) {
    const unsigned light = (unsigned(rgb[0]) * 54 + unsigned(rgb[1]) * 183 + unsigned(rgb[2]) * 19 + 128) / 256;
    target[0] = light * 3 / 2; target[1] = light * 3 / 4; target[2] = light / 3;
  } else if (type && cool > rgb[0]) {
    const unsigned neutral = rgb[0] < rgb[1] ? rgb[0] : rgb[1];
    const unsigned base = neutral < rgb[2] ? neutral : rgb[2];
    const unsigned spill = cool - rgb[0];
    target[0] = base + spill * 3 / 2;
    target[1] = base + spill * 3 / 4;
    target[2] = base + spill / 3;
  } else return;
  for (unsigned i = 0; i < 3; ++i) {
    const unsigned warm = target[i] > 255 ? 255 : target[i];
    rgb[i] = uint8_t((unsigned(rgb[i]) * (255 - amount) + warm * amount + 127) / 255);
  }
}
// The blue visibility floor fades with the same provider mix as the artwork.
inline uint8_t nightBlue(uint8_t current, uint8_t minimum, uint8_t amount) {
  if (current >= minimum) return current;
  return uint8_t(current + (unsigned(minimum - current) * (255 - amount) + 127) / 255);
}
}  // namespace quota_provider
