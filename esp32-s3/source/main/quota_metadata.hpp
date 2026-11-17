#pragma once
#include "quota_provider.hpp"
#include <cstdint>
#include <cstring>

namespace quota_metadata {
struct Packet {
  uint32_t clientId = 0, resetAt = 0, ageSeconds = 0;
  quota_provider::Provider provider = quota_provider::Provider::Codex;
};
inline bool decimal(const char *s, uint32_t limit, uint32_t &out) {
  if (!s || !*s) return false;
  uint32_t value = 0;
  for (; *s; ++s) {
    if (*s < '0' || *s > '9') return false;
    const unsigned digit = unsigned(*s - '0');
    if (digit > limit || value > (limit - digit) / 10) return false;
    value = value * 10 + digit;
  }
  out = value; return true;
}
inline bool parse(const char *text, uint32_t pairedAccount, Packet &out) {
  const size_t length = std::strlen(text);
  if (!length || length >= 100) return false;
  char buffer[100]; std::memcpy(buffer, text, length + 1);
  char *fields[7] = {buffer}; unsigned count = 1;
  for (size_t i = 0; i < length; ++i) if (buffer[i] == ',') {
    if (count == 7) return false;
    buffer[i] = 0; fields[count++] = buffer + i + 1;
  }
  Packet value;
  uint32_t account = 0, quotaAccount = 0;
  if (count != 7 || std::strcmp(fields[0], "LFR1") ||
      !quota_provider::hex(fields[1], account) || account != pairedAccount ||
      !quota_provider::hex(fields[2], value.clientId) || !value.clientId ||
      !quota_provider::hex(fields[4], quotaAccount) || quotaAccount != pairedAccount ||
      !decimal(fields[5], 4102444800U, value.resetAt) ||
      (value.resetAt && value.resetAt < 1700000000U) ||
      !decimal(fields[6], 86400, value.ageSeconds)) return false;
  if (!std::strcmp(fields[3], "codex")) value.provider = quota_provider::Provider::Codex;
  else if (!std::strcmp(fields[3], "claude")) value.provider = quota_provider::Provider::Claude;
  else return false;
  out = value; return true;
}
struct Entry {
  uint32_t clientId = 0, resetAt = 0, ageSeconds = 0, receivedAt = 0;
  void observe(const Packet &packet, uint32_t now) {
    clientId = packet.clientId; resetAt = packet.resetAt;
    ageSeconds = packet.ageSeconds; receivedAt = now;
  }
};
// Metadata is descriptive only: it never advances the sample TTL or quota filter.
// Select the same minimum fresh sample used for this provider's balance.
inline void apply(quota_provider::Snapshot &snapshot,
                  const quota_provider::Sample *samples, const Entry *entries,
                  size_t count, uint32_t now, uint32_t ttl) {
  size_t selected = count;
  for (size_t i = 0; i < count; ++i) {
    if (!samples[i].fresh(now, ttl)) continue;
    if (selected == count || samples[i].remaining < samples[selected].remaining)
      selected = i;
  }
  if (!snapshot.known) return;
  // The filter can deliberately hold a lower balance while a higher candidate
  // is being confirmed. Never describe that held value with another balance's
  // provenance, including during an offline fallback.
  if (selected == count || samples[selected].remaining != snapshot.remaining) {
    selected = count;
    for (size_t i = 0; i < count; ++i) {
      if (!samples[i].clientId || samples[i].remaining != snapshot.remaining) continue;
      if (selected == count || uint32_t(now - samples[i].seenAt) < uint32_t(now - samples[selected].seenAt))
        selected = i;
    }
  }
  if (selected == count) { snapshot.resetAt = 0; snapshot.ageSeconds = UINT32_MAX; return; }
  const auto &sample = samples[selected]; const auto &meta = entries[selected];
  snapshot.resetAt = 0;
  snapshot.ageSeconds = uint32_t(now - sample.seenAt) / 1000;
  if (meta.clientId == sample.clientId) {
    snapshot.resetAt = meta.resetAt;
    snapshot.ageSeconds = meta.ageSeconds + uint32_t(now - meta.receivedAt) / 1000;
  }
}
}  // namespace quota_metadata
