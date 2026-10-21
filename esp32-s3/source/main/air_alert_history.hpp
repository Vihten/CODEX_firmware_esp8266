#pragma once

#include "air_alert.hpp"

namespace air_alert {

struct FailureHistory {
  PollResult result{};
  uint32_t at = 0;
  bool seen = false;

  void observe(const PollResult &poll, bool accepted, uint32_t now) {
    if (accepted) return;
    result = poll;
    if (result.failure == Failure::None) result.failure = Failure::Payload;
    at = now;
    seen = true;
  }

  // Diagnostic uptime age is modulo one millis cycle; no wall-clock age is implied.
  uint32_t ageSeconds(uint32_t now) const {
    return seen ? uint32_t(now - at) / 1000 : 0;
  }
};

}  // namespace air_alert
