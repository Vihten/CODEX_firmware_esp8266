#pragma once
#include "air_alert.hpp"

namespace city_presence {
constexpr uint32_t kLifetimeSeconds = 300;
struct Sound {
  uint32_t lastRealId = 0, lastDemoAt = 0, lastBeepAt = 0, startedAt = 0;
  uint8_t requests = 0;
  bool wasDemo = false, playing = false;

  bool update(uint32_t messageId, bool reported, bool demo, uint32_t demoAt,
              bool allowed, uint32_t now) {
    const bool realEvent = reported && messageId > lastRealId;
    const bool demoEvent = demo && (!wasDemo || demoAt != lastDemoAt);
    if (realEvent) lastRealId = messageId;
    if (demoEvent) lastDemoAt = demoAt;
    wasDemo = demo;
    const bool event = realEvent || demoEvent;
    // Consume events even while muted so re-enabling cannot replay them.
    if (!allowed || (!reported && !demo)) {
      playing = false;
      return false;
    }
    if (playing && uint32_t(now - startedAt) >= 60000) playing = false;
    // A report received during the minute is consumed without extending it.
    if (event && !playing) {
      playing = true;
      startedAt = now;
      requests = 0;
    }
    // Space late requests from the actual previous request, avoiding catch-up bursts.
    if (playing && requests < 60 && (!requests || uint32_t(now - lastBeepAt) >= 1000)) {
      lastBeepAt = now;
      ++requests;
      return true;
    }
    return false;
  }
};

struct State {
  uint32_t messageId = 0, checkedAt = 0, acceptedAt = 0;
  int64_t messageTime = 0;
  bool seen = false, failed = false, expired = true, demo = false;
  uint32_t demoAt = 0;
  void observe(const air_alert::PollResult &result, uint32_t now, int64_t epoch) {
    failed = !result.valid;
    if (!result.valid) return;
    seen = true; checkedAt = now;
    if (!result.messageId || result.messageId <= messageId ||
        !air_alert::active(result.level) || result.messageTime < 1577836800 ||
        result.messageTime > epoch + 120 || epoch - result.messageTime >= kLifetimeSeconds) return;
    messageId = result.messageId; messageTime = result.messageTime;
    acceptedAt = now; expired = false;
  }
  void expire(uint32_t now, int64_t epoch) {
    if (!expired && (uint32_t(now - acceptedAt) >= kLifetimeSeconds * 1000 ||
        epoch < messageTime - 120 || epoch - messageTime >= kLifetimeSeconds)) expired = true;
    if (demo && uint32_t(now - demoAt) >= 60000) demo = false;
  }
  bool active() const { return demo || (messageId && !expired); }
  void startDemo(uint32_t now) { demo = true; demoAt = now; }
  void stopDemo() { demo = false; }
  bool fresh(uint32_t now) const { return seen && !failed && uint32_t(now - checkedAt) < 120000; }
};
}  // namespace city_presence
