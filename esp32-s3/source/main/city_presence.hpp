#pragma once
#include "air_alert.hpp"

namespace city_presence {
constexpr uint32_t kLifetimeSeconds = 300;
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
