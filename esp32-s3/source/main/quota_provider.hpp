#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "quota_filter.hpp"

namespace quota_provider {
enum class Provider : uint8_t { Codex, Claude };
inline const char *name(Provider provider) { return provider == Provider::Claude ? "claude" : "codex"; }
constexpr uint32_t codexFreshMs = 15000, claudeFreshMs = 90000, rotationMs = 300000;
struct Snapshot {
  int16_t remaining = -1;
  bool known = false, fresh = false;
  uint32_t resetAt = 0;
  uint32_t ageSeconds = UINT32_MAX;
};
struct Sample {
  uint32_t clientId = 0;
  uint64_t seenAt = 0;
  int16_t remaining = -1;
  uint32_t validForMs = claudeFreshMs;
  bool fresh(uint64_t now, uint32_t ttl) const {
    return clientId && remaining >= 0 && remaining <= 100 && now >= seenAt &&
        now - seenAt < (ttl < validForMs ? ttl : validForMs);
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
  static constexpr uint32_t claudeGraceMs = 120000, exhaustedMs = 60000;
  uint8_t snapshotMode = 255;
  Provider active = Provider::Codex;
  uint32_t since = 0, claudeFreshAt = 0, interval = rotationMs;
  // Snapshot rotation also assigns a short duration to exhausted services.
  bool started = false, bothFresh = false;
  bool seenFresh = false;
  // Fresh known balances include zero. Only Claude's known positive balance
  // can bridge a transient polling failure, for <120 seconds since last fresh.
  // Call after update() so the current fresh Claude snapshot anchors its grace.
  bool available(Provider provider, const Snapshot &snapshot, uint32_t now) const {
    if (!snapshot.known || snapshot.remaining < 0 || snapshot.remaining > 100) return false;
    return snapshot.fresh || (provider == Provider::Claude && snapshot.remaining > 0 &&
        seenFresh && uint32_t(now - claudeFreshAt) < claudeGraceMs);
  }
  Provider update(const Snapshot &codex, const Snapshot &claude, uint32_t now, uint32_t intervalMs = rotationMs) {
    if (claude.known && claude.fresh && claude.remaining >= 0 && claude.remaining <= 100) {
      claudeFreshAt = now; seenFresh = true;
    } else if (seenFresh && uint32_t(now - claudeFreshAt) >= claudeGraceMs) {
      // Latch expiry so an old cache cannot revive on a later millis() wrap.
      seenFresh = false;
    }
    const bool codexAvailable = available(Provider::Codex, codex, now);
    const bool claudeAvailable = available(Provider::Claude, claude, now);
    const bool codexPositive = codexAvailable && codex.remaining > 0;
    const bool claudePositive = claudeAvailable && claude.remaining > 0;
    if (!intervalMs) intervalMs = rotationMs;
    // A fresh exhausted service is still valid data. Give it one minute of
    // night, then return to the positive service for the configured interval.
    // Modes encode eligibility and zero/positive state, never polling timestamps.
    const uint8_t mode = uint8_t((codexAvailable ? (codexPositive ? 2 : 1) : 0) |
        ((claudeAvailable ? (claudePositive ? 2 : 1) : 0) << 2));
    if (!started) {
      if (codexPositive != claudePositive) active = claudePositive ? Provider::Claude : Provider::Codex;
      else if (codexAvailable != claudeAvailable) active = claudeAvailable ? Provider::Claude : Provider::Codex;
    }
    const unsigned shift = active == Provider::Codex ? 0U : 2U;
    const unsigned otherShift = shift ^ 2U;
    const bool preservePositiveSlot = started &&
        ((snapshotMode >> shift) & 3U) == 2U && ((mode >> shift) & 3U) == 2U &&
        ((snapshotMode >> otherShift) & 3U) <= 1U && ((mode >> otherShift) & 3U) <= 1U;
    // A zero provider can expire between polls. Do not restart the positive
    // interval on each zero/missing toggle, or its recurring night is starved.
    if (!started || (snapshotMode != mode && !preservePositiveSlot) || interval != intervalMs) since = now;
    // If the exhausted service returns after its scheduled turn was missed,
    // start a complete one-minute slot now, not the tail of an imaginary slot.
    if (preservePositiveSlot && interval == intervalMs &&
        ((snapshotMode >> otherShift) & 3U) == 0U && ((mode >> otherShift) & 3U) == 1U &&
        uint32_t(now - since) >= intervalMs) since = now - intervalMs;
    started = true; snapshotMode = mode; interval = intervalMs;
    bothFresh = codexAvailable && claudeAvailable;
    if (codexAvailable != claudeAvailable) {
      active = claudeAvailable ? Provider::Claude : Provider::Codex;
      return active;
    }
    // Unknown data uses the existing no-data fallback. Both exhausted services
    // already show night, so retain identity rather than rotating identical skies.
    if (!codexAvailable || (!codexPositive && !claudePositive)) return active;
    const uint32_t codexDuration = codexPositive ? interval : exhaustedMs;
    const uint32_t claudeDuration = claudePositive ? interval : exhaustedMs;
    const uint32_t activeDuration = active == Provider::Codex ? codexDuration : claudeDuration;
    const uint64_t cycle = uint64_t(codexDuration) + claudeDuration;
    const uint32_t elapsed = uint32_t(now - since);
    // Bounded catch-up, including millis wrap and long pauses; never spin once
    // per missed turn and never allocate in the network owner.
    since += uint32_t(uint64_t(elapsed) / cycle * cycle);
    if (uint64_t(elapsed) % cycle >= activeDuration) {
      since += activeDuration;
      active = active == Provider::Codex ? Provider::Claude : Provider::Codex;
    }
    return active;
  }
  Provider update(bool codexFresh, bool claudeFresh, uint32_t now, uint32_t intervalMs = rotationMs) {
    snapshotMode = 255;
    if (!intervalMs) intervalMs = rotationMs;
    if (interval != intervalMs) { interval = intervalMs; since = now; }
    if (!started) { started = true; since = now; }
    const bool both = codexFresh && claudeFresh;
    if (both && !bothFresh) since = now;
    bothFresh = both;
    Provider next = active;
    if (codexFresh != claudeFresh) next = claudeFresh ? Provider::Claude : Provider::Codex;
    else if (codexFresh) {
      const uint32_t turns = uint32_t(now - since) / interval;
      if (turns & 1U) next = active == Provider::Codex ? Provider::Claude : Provider::Codex;
      if (turns) since += turns * interval;
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
}  // namespace quota_provider
