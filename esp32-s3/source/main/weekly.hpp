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
// One authoritative NVS blob prevents a power cut splitting totals/revisions.
struct RankingStore {
  Store week;
  Store total;
  uint32_t crc = 0;
};
inline uint32_t checksum(const RankingStore &store) {
  const auto *bytes = reinterpret_cast<const uint8_t *>(&store);
  uint32_t crc = UINT32_MAX;
  for (size_t i = 0; i < offsetof(RankingStore, crc); ++i) {
    crc ^= bytes[i];
    for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
  }
  return ~crc;
}
inline bool valid(const RankingStore &store) {
  return store.crc == checksum(store) && valid(store.week) && valid(store.total) && !store.total.week;
}
constexpr uint64_t maxRevision = (1ULL << 48) - 1;
inline bool v2(const Entry &entry) { return entry.reserved[6] == 2; }
inline uint64_t revision(const Entry &entry) {
  uint64_t value = 0;
  for (unsigned i = 0; i < 6; ++i) value |= uint64_t(entry.reserved[i]) << (8 * i);
  return value;
}
inline void setRevision(Entry &entry, uint64_t value) {
  for (unsigned i = 0; i < 6; ++i) entry.reserved[i] = uint8_t(value >> (8 * i));
  entry.reserved[6] = 2;
}
inline Entry *slot(Store &store, uint32_t clientId) {
  for (auto &entry : store.entries) if (entry.clientId == clientId) return &entry;
  for (auto &entry : store.entries) if (!entry.clientId) return &entry;
  return nullptr;
}
// Preflight both bounded stores before publishing either update. Lifetime retains
// revision authority across weekly rollovers; it is never seeded from LFT1.
inline bool mergeV2(Store &weekStore, Store &lifetime, uint32_t week,
                    const Entry &incoming, uint64_t lifetimeTokens,
                    bool &weeklyChanged, bool &lifetimeChanged) {
  weeklyChanged = lifetimeChanged = false;
  if (!week || week < weekStore.week || lifetime.week || !incoming.clientId ||
      !v2(incoming) || !revision(incoming) || incoming.tokens > maximum ||
      lifetimeTokens > maximum || incoming.tokens > lifetimeTokens) return false;
  Entry *all = slot(lifetime, incoming.clientId);
  if (!all) return false;
  if (all->clientId && v2(*all)) {
    if (revision(incoming) < revision(*all)) return true;
    if (revision(incoming) == revision(*all)) {
      // An equal revision must describe exactly the accepted snapshot.
      if (all->tokens != lifetimeTokens || all->rgb != incoming.rgb || std::strcmp(all->name, incoming.name)) return false;
      for (const auto &entry : weekStore.entries)
        if (entry.clientId == incoming.clientId && week == weekStore.week)
          return v2(entry) && revision(entry) == revision(incoming) && entry.tokens == incoming.tokens &&
              entry.rgb == incoming.rgb && !std::strcmp(entry.name, incoming.name);
      return false;
    }
  }
  // Rollover has an empty weekly table, so it always has room.
  Entry *weeklyEntry = week == weekStore.week ? slot(weekStore, incoming.clientId) : nullptr;
  if (week == weekStore.week && !weeklyEntry) return false;
  if (week != weekStore.week) {
    advance(weekStore, week);
    weeklyEntry = &weekStore.entries[0];
  }
  *weeklyEntry = incoming;
  *all = incoming;
  all->tokens = lifetimeTokens;
  weeklyChanged = lifetimeChanged = true;
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
  if (selected->clientId && (v2(*selected) || incoming.tokens < selected->tokens)) return true;
  changed = selected->clientId != incoming.clientId || selected->tokens != incoming.tokens ||
      selected->rgb != incoming.rgb || std::strcmp(selected->name, incoming.name);
  if (changed) *selected = incoming;
  return true;
}
} // namespace weekly
