#pragma once
#include <cstddef>
#include <cstdint>

// Portable LFD1 daily totals. Allocate Store in PSRAM, never on a task stack.
// Caller synchronizes all access and owns stable agent-slot identity + NVS keys.
// No allocation, I/O, inferred history, clock source, or raw client identifiers.
namespace daily {
constexpr std::size_t kAgents = 12;
constexpr std::size_t kDays = 366;
constexpr uint64_t kMaxRevision = (uint64_t{1} << 48) - 1;
constexpr std::size_t kAgentBlobBytes = 24 + kDays * 18 + 4; // 6616 bytes

enum class Result : uint8_t {
  Applied, Unchanged, Stale, Conflict, InvalidDate, InvalidClient,
  InvalidRevision, Future, TooOld, Overflow
};
struct Record { uint64_t tokens = 0; uint64_t revision = 0; uint32_t ordinal = 0; };
using AgentStore = Record[kDays];
struct Day {
  uint32_t date = 0;
  uint64_t tokens = 0;
  uint16_t agents = 0; // Presence mask: recorded zero differs from missing.
};
inline bool leap(uint32_t year) noexcept {
  return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}
inline uint32_t yearStart(uint32_t year) noexcept {
  const uint32_t y = year - 1;
  return y * 365 + y / 4 - y / 100 + y / 400 + 1;
}
// Ordinal 1 = 0001-01-01; zero is the empty record sentinel.
inline uint32_t ordinal(uint32_t date) noexcept {
  const uint32_t year = date / 10000, month = date / 100 % 100, day = date % 100;
  if (!year || year > 9999 || !month || month > 12 || !day) return 0;
  constexpr uint32_t lengths[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  if (day > lengths[month - 1] + (month == 2 && leap(year) ? 1u : 0u)) return 0;
  uint32_t value = yearStart(year) + day - 1;
  for (uint32_t m = 1; m < month; ++m)
    value += lengths[m - 1] + (m == 2 && leap(year) ? 1u : 0u);
  return value;
}
inline uint32_t dateFromOrdinal(uint32_t value) noexcept {
  if (!value || value > ordinal(99991231)) return 0;
  uint32_t low = 1, high = 10000;
  while (low + 1 < high) {
    const uint32_t middle = (low + high) / 2;
    if (yearStart(middle) <= value) low = middle; else high = middle;
  }
  uint32_t month = 1;
  while (month < 12 && ordinal(low * 10000 + (month + 1) * 100 + 1) <= value) ++month;
  return low * 10000 + month * 100 + value - ordinal(low * 10000 + month * 100 + 1) + 1;
}

class Store {
 public:
  Store() noexcept = default;
  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;
  // Bind stable IDs once; no eviction, including on a thirteenth identity.
  // Persist mapping or restore each blob via decodeAgent before accepting data.
  bool findAgent(uint64_t identity, std::size_t& client) const noexcept {
    for (std::size_t i = 0; i < kAgents; ++i)
      if (bound_[i] && identities_[i] == identity) { client = i; return true; }
    return false;
  }
  bool bindAgent(uint64_t identity, std::size_t& client) noexcept {
    if (findAgent(identity, client)) return true;
    for (std::size_t i = 0; i < kAgents; ++i) {
      if (bound_[i]) continue;
      bound_[i] = true; identities_[i] = identity; client = i;
      dirty_[i] = true;
      return true;
    }
    return false;
  }
  // today and day are local calendar YYYYMMDD in the SAME caller-chosen zone.
  // A slot is replaced only by a newer date; backward clock jumps cannot erase
  // future data. Caller must supply a trusted date (not a packet's date).
  Result merge(uint32_t day, std::size_t client, uint64_t tokens,
               uint64_t revision, uint32_t today) noexcept {
    if (client >= kAgents) return Result::InvalidClient;
    const uint32_t when = ordinal(day), current = ordinal(today);
    if (!when || !current) return Result::InvalidDate;
    if (revision > kMaxRevision) return Result::InvalidRevision;
    if (when > current) return Result::Future;
    if (current - when >= kDays) return Result::TooOld;
    Record& record = records_[client][when % kDays];
    if (record.ordinal > when) return Result::TooOld;
    if (record.ordinal == when) {
      if (revision < record.revision) return Result::Stale;
      if (revision == record.revision)
        return tokens == record.tokens ? Result::Unchanged : Result::Conflict;
    }
    uint64_t total = tokens;
    for (std::size_t agent = 0; agent < kAgents; ++agent) {
      if (agent == client) continue;
      const Record& other = records_[agent][when % kDays];
      if (other.ordinal != when) continue;
      if (other.tokens > UINT64_MAX - total) return Result::Overflow;
      total += other.tokens;
    }
    record = Record{tokens, revision, when};
    dirty_[client] = true;
    return Result::Applied;
  }
  bool readAgent(uint32_t day, std::size_t client, Record& out) const noexcept {
    out = Record{};
    const uint32_t when = ordinal(day);
    if (!when || client >= kAgents) return false;
    const Record& found = records_[client][when % kDays];
    if (found.ordinal != when) return false;
    out = found;
    return true;
  }
  bool readDay(uint32_t day, Day& out) const noexcept {
    out = Day{};
    const uint32_t when = ordinal(day);
    if (!when) return false;
    out.date = day;
    for (std::size_t agent = 0; agent < kAgents; ++agent) {
      const Record& record = records_[agent][when % kDays];
      if (record.ordinal != when) continue;
      if (record.tokens > UINT64_MAX - out.tokens) { out = Day{}; return false; }
      out.tokens += record.tokens;
      out.agents |= static_cast<uint16_t>(1u << agent);
    }
    return true; // agents == 0 means unknown, not a measured zero.
  }
  // Inclusive, at most 366 days. Unknown dates contribute no observed tokens;
  // observed_days makes incomplete totals explicit. Overflow returns false.
  bool total(uint32_t first, uint32_t last, uint64_t& tokens,
             uint32_t& observed_days) const noexcept {
    tokens = 0; observed_days = 0;
    const uint32_t begin = ordinal(first), end = ordinal(last);
    if (!begin || end < begin || end - begin >= kDays) return false;
    for (uint32_t when = begin; when <= end; ++when) {
      Day day;
      if (!readDay(dateFromOrdinal(when), day) || day.tokens > UINT64_MAX - tokens) {
        tokens = 0; observed_days = 0; return false;
      }
      tokens += day.tokens;
      if (day.agents) ++observed_days;
    }
    return true;
  }
  bool weekly(uint32_t last, uint64_t& tokens, uint32_t& observed_days) const noexcept {
    const uint32_t end = ordinal(last);
    if (end < 7) { tokens = 0; observed_days = 0; return false; }
    return total(dateFromOrdinal(end - 6), last, tokens, observed_days);
  }
  bool dirty(std::size_t client) const noexcept { return client < kAgents && dirty_[client]; }
  // Call ONLY after nvs_set_blob AND nvs_commit succeed, under the same external
  // synchronization as encode/merge (or verify no merges occurred meanwhile).
  void markSaved(std::size_t client) noexcept { if (client < kAgents) dirty_[client] = false; }

  // Exact little-endian format, no ABI padding: 24-byte header, 366 records
  // {ordinal:u32, tokens:u64, revision:u48}, CRC32:u32. Header includes a stable
  // numeric agent identity. Restore from stable per-slot keys by extracting the
  // tentative identity at offset 8 and passing it to decodeAgent for validation.
  bool encodeAgent(std::size_t client, uint64_t identity, void* buffer,
                   std::size_t bytes) const noexcept {
    if (client >= kAgents || !buffer || bytes != kAgentBlobBytes) return false;
    if (bound_[client] && identities_[client] != identity) return false;
    auto* out = static_cast<uint8_t*>(buffer);
    put(out, 0x3144464c, 4); put(out + 4, 1, 4);
    put(out + 8, identity, 8); put(out + 16, kDays, 4); put(out + 20, 0, 4);
    for (std::size_t i = 0; i < kDays; ++i) {
      const Record& r = records_[client][i];
      put(out + 24 + i * 18, r.ordinal, 4);
      put(out + 28 + i * 18, r.tokens, 8);
      put(out + 36 + i * 18, r.revision, 6);
    }
    put(out + bytes - 4, crc(out, bytes - 4), 4);
    return true;
  }
  // Validate everything BEFORE changing this agent. On any failure all state
  // remains unchanged. Buffer must not alias Store; caller owns a ~6.6KB buffer.
  bool decodeAgent(std::size_t client, uint64_t identity, const void* buffer,
                   std::size_t bytes) noexcept {
    if (client >= kAgents || !buffer || bytes != kAgentBlobBytes) return false;
    if (bound_[client] && identities_[client] != identity) return false;
    std::size_t existing = 0;
    if (findAgent(identity, existing) && existing != client) return false;
    const auto* in = static_cast<const uint8_t*>(buffer);
    if (get(in,4) != 0x3144464c || get(in+4,4) != 1 || get(in+8,8) != identity ||
        get(in+16,4) != kDays || get(in+20,4) != 0 ||
        get(in+bytes-4,4) != crc(in, bytes-4)) return false;
    for (std::size_t i = 0; i < kDays; ++i) {
      const uint64_t tokens = get(in+28+i*18,8), revision = get(in+36+i*18,6);
      const uint32_t when = static_cast<uint32_t>(get(in+24+i*18,4));
      if (!when) { if (tokens || revision) return false; continue; }
      if (when > ordinal(99991231) || when % kDays != i || revision > kMaxRevision) return false;
      uint64_t sum = tokens;
      for (std::size_t agent = 0; agent < kAgents; ++agent) {
        if (agent == client || records_[agent][i].ordinal != when) continue;
        if (records_[agent][i].tokens > UINT64_MAX - sum) return false;
        sum += records_[agent][i].tokens;
      }
    }
    // Sparse histories may retain stale slots across long idle periods; each
    // slot still holds its newest date. Merge's trusted today bounds backfill.
    for (std::size_t i = 0; i < kDays; ++i)
      records_[client][i] = Record{get(in+28+i*18,8), get(in+36+i*18,6),
          static_cast<uint32_t>(get(in+24+i*18,4))};
    dirty_[client] = false;
    bound_[client] = true;
    identities_[client] = identity;
    return true;
  }
 private:
  static void put(uint8_t* out, uint64_t value, unsigned bytes) noexcept {
    for (unsigned i = 0; i < bytes; ++i) { out[i] = static_cast<uint8_t>(value); value >>= 8; }
  }
  static uint64_t get(const uint8_t* in, unsigned bytes) noexcept {
    uint64_t value = 0;
    for (unsigned i = 0; i < bytes; ++i) value |= uint64_t(in[i]) << (i * 8);
    return value;
  }
  static uint32_t crc(const uint8_t* in, std::size_t bytes) noexcept {
    uint32_t value = UINT32_MAX;
    for (std::size_t i = 0; i < bytes; ++i) {
      value ^= in[i];
      for (unsigned bit = 0; bit < 8; ++bit)
        value = (value >> 1) ^ (0xedb88320u & (0u - (value & 1u)));
    }
    return ~value;
  }
  AgentStore records_[kAgents]{};
  bool dirty_[kAgents]{};
  uint64_t identities_[kAgents]{};
  bool bound_[kAgents]{};
};
static_assert(sizeof(Store) <= 106000, "Daily store PSRAM budget exceeded");
static_assert(kAgentBlobBytes <= 6700, "Per-agent NVS blob budget exceeded");
} // namespace daily
