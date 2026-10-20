#pragma once

#include <cstddef>
#include <cstdint>

namespace air_alert {

enum class Level : uint8_t { Clear = 0, Yellow = 1, Red = 2, Unknown = 3, Unspecified = 4 };

struct PollJob {
  uint32_t generation = 0;
};

struct PollResult {
  uint32_t generation = 0;
  Level level = Level::Unknown;
  int httpStatus = 0;
  bool valid = false;
  uint32_t retryAfterMs = 30000;
};

constexpr bool active(Level level) {
  return level == Level::Yellow || level == Level::Red || level == Level::Unspecified;
}

constexpr unsigned priority(Level level) {
  return level == Level::Red ? 3 : level == Level::Unspecified ? 2 : level == Level::Yellow ? 1 : 0;
}

struct State {
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
    accept(result.level, result.valid, now);
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

// Pure bounded helpers, also exercised by the native contract tests.
bool parseBody(const char *body, size_t length, Level &level);
uint32_t retryAfterMs(const char *value);

bool startPolling();
bool submit(const PollJob &job);
bool receive(PollResult &result);
bool busy();

}  // namespace air_alert
