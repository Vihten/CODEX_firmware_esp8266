#pragma once
#include <cstdint>

namespace quota {
// Missing low-limit agents must not look like an account refill. Network owner only.
struct Filter {
  static constexpr uint32_t confirmMs = 15000;
  bool initialized = false, pending = false, reset = false;
  uint8_t candidate = 0;
  uint32_t since = 0;
  bool observe(int value, bool reportedReset, uint32_t now, uint8_t &remaining) {
    if (value < 0 || value > 100) { pending = reset = false; return false; }
    if (!initialized) {
      initialized = true; remaining = uint8_t(value); pending = reset = false;
      return false;
    }
    if (value <= remaining) {
      remaining = uint8_t(value); pending = reset = false; return false;
    }
    if (!pending || candidate != value) {
      pending = true; candidate = uint8_t(value); since = now; reset = reportedReset;
      return false;
    }
    reset |= reportedReset;
    if (uint32_t(now - since) < confirmMs) return false;
    const bool recovered = candidate == 100 && reset;
    remaining = candidate; pending = reset = false;
    return recovered;
  }
};
}
