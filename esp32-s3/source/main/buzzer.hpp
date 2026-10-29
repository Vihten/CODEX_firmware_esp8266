#pragma once
#include <cstdint>

namespace buzzer {
constexpr int kPin = 6;
constexpr uint32_t kDurationMs = 250;

// Initialize in app_main before tasks. One task owns start/service thereafter.
// Initialization is silent; any driver error latches OFF until reboot.
bool initialize();
bool ready();
// now is monotonic milliseconds modulo 2^32. Active requests are rejected.
bool start(uint32_t now);
// Call regularly: stops on the first call at/after 250 ms, or when disallowed.
// The caller must also authorize start(); service never starts or resumes sound.
void service(uint32_t now, bool allowed);
}  // namespace buzzer
