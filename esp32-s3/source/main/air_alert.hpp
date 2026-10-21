#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>

namespace air_alert {

enum class Level : uint8_t { Clear = 0, Yellow = 1, Red = 2, Unknown = 3, Unspecified = 4 };
enum class Failure : uint8_t { None, Memory, Dns, Tls, Write, Read, Http, Payload, Timeout };

constexpr const char *failureName(Failure failure) {
  switch (failure) {
    case Failure::None: return "none";
    case Failure::Memory: return "memory";
    case Failure::Dns: return "dns";
    case Failure::Tls: return "tls";
    case Failure::Write: return "write";
    case Failure::Read: return "read";
    case Failure::Http: return "http";
    case Failure::Payload: return "payload";
    case Failure::Timeout: return "timeout";
  }
  return "unknown";
}

struct PollJob {
  uint32_t generation = 0;
  uint32_t knownHeadId = 0, knownEventId = 0;
  Level knownLevel = Level::Unknown;
  int64_t knownEventTime = 0;
};

struct PollResult {
  uint32_t generation = 0;
  uint32_t headId = 0, messageId = 0;
  int64_t messageTime = 0;
  Level level = Level::Unknown;
  int httpStatus = 0;
  bool valid = false;
  Failure failure = Failure::None;
  uint32_t elapsedMs = 0;
  int tlsError = 0, tlsCode = 0, tlsFlags = 0;
  uint32_t heapBeforeTls = 0, largestBeforeTls = 0;
  int64_t clockBeforeTls = 0;
  uint32_t retryAfterMs = 30000;
};

constexpr bool active(Level level) {
  return level == Level::Yellow || level == Level::Red || level == Level::Unspecified;
}

constexpr unsigned priority(Level level) {
  return level == Level::Red ? 3 : level == Level::Unspecified ? 2 : level == Level::Yellow ? 1 : 0;
}

struct State {
  uint32_t telegramHeadId = 0, telegramMessageId = 0;
  int64_t telegramMessageTime = 0;
  Level last = Level::Unknown;
  bool seen = false;
  bool freshnessExpired = true;
  uint32_t checkedAt = 0;
  uint8_t demoLevel = 0;
  uint32_t demoAt = 0;

  void accept(Level level, bool valid, uint32_t now) {
    // Only verified explicit states may replace a warning or refresh its age.
    if (!valid || (level != Level::Clear && !active(level))) return;
    last = level;
    seen = true;
    freshnessExpired = false;
    checkedAt = now;
  }

  bool accept(const PollResult &result, uint32_t generation, uint32_t now) {
    if (result.generation != generation) return false;
    if (!result.valid || !result.messageId || (result.level != Level::Clear && !active(result.level))) return false;
    if (result.valid && result.messageId &&
        (result.headId < result.messageId || result.messageTime < 1577836800 ||
         result.headId < telegramHeadId || result.messageId < telegramMessageId ||
         (result.messageId == telegramMessageId &&
          (result.level != last || result.messageTime != telegramMessageTime)))) return false;
    accept(result.level, result.valid, now);
    if (result.valid && result.messageId) {
      telegramHeadId = result.headId;
      telegramMessageId = result.messageId;
      telegramMessageTime = result.messageTime;
    }
    return result.valid && (result.level == Level::Clear || active(result.level));
  }

  bool fresh(uint32_t now, bool connected, bool configured) const {
    return seen && !freshnessExpired && connected && configured &&
        uint32_t(now - checkedAt) < 120000;
  }

  // Service every iteration so expired timestamps cannot revive after a full millis cycle.
  void expire(uint32_t now) {
    if (seen && uint32_t(now - checkedAt) >= 120000) freshnessExpired = true;
    if (active(static_cast<Level>(demoLevel)) && uint32_t(now - demoAt) >= 60000)
      stopDemo();
  }

  bool manual(uint32_t now) const {
    return (demoLevel == 1 || demoLevel == 2) && uint32_t(now - demoAt) < 60000;
  }

  uint32_t remainingSeconds(uint32_t now) const {
    return manual(now) ? (60000 - uint32_t(now - demoAt) + 999) / 1000 : 0;
  }

  Level effective(uint32_t now, bool enabled, bool connected, bool configured) const {
    // Losing freshness never turns an active warning into an all-clear.
    const Level real = !enabled ? Level::Clear :
        (fresh(now, connected, configured) || active(last) ? last : Level::Unknown);
    if (!manual(now)) return real;
    const Level demo = static_cast<Level>(demoLevel);
    return priority(real) > priority(demo) ? real : demo;
  }

  void startDemo(Level level, uint32_t now) {
    if (level != Level::Yellow && level != Level::Red) return;
    demoLevel = uint8_t(level);
    demoAt = now;
  }
  void stopDemo() { demoLevel = 0; demoAt = 0; }
  void clearDemo() { stopDemo(); }
  void reset() { *this = State{}; }
};

// Separate from the settings schema and former providers' runtime state.
struct TelegramRecord {
  uint32_t magic = 0x54470100, headId = 0, messageId = 0, messageTime = 0;
  uint32_t level = 3, crc = 0;
};
inline uint32_t checksum(const TelegramRecord &record) {
  uint32_t hash = 2166136261u;
  for (uint32_t word : {record.magic, record.headId, record.messageId, record.messageTime, record.level})
    for (unsigned shift = 0; shift < 32; shift += 8) { hash ^= (word >> shift) & 255; hash *= 16777619u; }
  return hash;
}
inline bool valid(const TelegramRecord &record) {
  return record.magic == 0x54470100 && record.messageId && record.headId >= record.messageId &&
      record.messageTime >= 1577836800 && (record.level <= 2 || record.level == 4) && record.crc == checksum(record);
}

// Pure bounded helpers, also exercised by the native contract tests.
bool parseBody(const char *body, size_t length, Level &level);
uint32_t retryAfterMs(const char *value);

bool startPolling();
bool submit(const PollJob &job);
bool receive(PollResult &result);
bool busy();
// Last worker stack high-water sample in bytes; initially zero.
uint32_t workerStackFree();

}  // namespace air_alert
