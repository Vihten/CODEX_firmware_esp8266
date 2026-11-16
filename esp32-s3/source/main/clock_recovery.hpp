#pragma once

#include <cstdint>

namespace clock_recovery {
inline constexpr const char *servers[] = {
    "pool.ntp.org", "time.cloudflare.com", "time.google.com"};
inline constexpr uint32_t retryMs = 60000;

struct State {
  uint32_t serverIndex = 0;
  // Counts retries after the initial start.
  uint32_t retries = 0;

  bool request(uint32_t now, bool connected, bool valid, bool enabled) {
    const bool reconnected = connected && !connected_;
    connected_ = connected;
    if (!connected || (valid && enabled)) return false;
    if (attempted_ && !(reconnected && !valid) &&
        uint32_t(now - requestedAt_) < retryMs) return false;

    if (attempted_) {
      serverIndex = (serverIndex + 1) % 3;
      if (retries != UINT32_MAX) ++retries;
    }
    attempted_ = true;
    requestedAt_ = now;
    return true;
  }

 private:
  uint32_t requestedAt_ = 0;
  bool connected_ = false;
  bool attempted_ = false;
};
} // namespace clock_recovery
