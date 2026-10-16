#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace weekly {
constexpr size_t capacity = 12;
constexpr uint64_t maximum = 9007199254740991ULL;
struct Entry {
  uint64_t tokens = 0;
  uint32_t clientId = 0, rgb = 0;
  char name[49] = {};
  uint8_t reserved[7] = {};
};
struct Store {
  uint32_t magic = 0x574b5331, week = 0;
  Entry entries[capacity] = {};
  uint32_t crc = 0;
};
inline uint32_t checksum(const Store &store) {
  const auto *bytes = reinterpret_cast<const uint8_t *>(&store);
  uint32_t crc = UINT32_MAX;
  for (size_t i = 0; i < offsetof(Store, crc); ++i) {
    crc ^= bytes[i];
    for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
  }
  return ~crc;
}
inline bool valid(const Store &store) {
  if (store.magic != 0x574b5331 || store.crc != checksum(store) ||
      (store.week && (store.week < 20200101 || store.week > 21991231))) return false;
  for (size_t i = 0; i < capacity; ++i) {
    const auto &entry = store.entries[i];
    if (entry.tokens > maximum || entry.rgb > 0xffffff ||
        !std::memchr(entry.name, 0, sizeof(entry.name))) return false;
    for (size_t j = 0; j < i; ++j)
      if (entry.clientId && entry.clientId == store.entries[j].clientId) return false;
  }
  return true;
}
inline bool advance(Store &store, uint32_t week) {
  if (!week || week <= store.week) return false;
  store = Store{};
  store.week = week;
  return true;
}
// Cumulative high-water marks make retransmission and controller restarts idempotent.
inline bool merge(Store &store, uint32_t week, const Entry &incoming, bool &changed) {
  changed = false;
  if (!week || week != store.week || !incoming.clientId || incoming.tokens > maximum) return false;
  Entry *selected = nullptr;
  for (auto &entry : store.entries) if (entry.clientId == incoming.clientId) { selected = &entry; break; }
  if (!selected) for (auto &entry : store.entries) if (!entry.clientId) { selected = &entry; break; }
  if (!selected) return false; // Never evict an offline agent's accumulated week.
  if (selected->clientId && incoming.tokens < selected->tokens) return true;
  changed = selected->clientId != incoming.clientId || selected->tokens != incoming.tokens ||
      selected->rgb != incoming.rgb || std::strcmp(selected->name, incoming.name);
  if (changed) *selected = incoming;
  return true;
}
} // namespace weekly
