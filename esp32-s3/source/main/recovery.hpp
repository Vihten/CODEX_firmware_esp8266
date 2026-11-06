#pragma once
#include <atomic>
#include <cstdint>
#include <initializer_list>

namespace recovery {
constexpr uint32_t stackAfterMs = 90000, stackSpacingMs = 120000;
constexpr uint32_t rebootAfterMs = 600000, manualGraceMs = 120000;
constexpr uint32_t healthyRearmMs = 1800000, stallAfterMs = 300000;
constexpr uint16_t stackCode = 0x0500, failedCode = 0x0501, queuedCode = 0x0502;
constexpr uint16_t exhaustedCode = 0x0503, stalledCode = 0x0504, restoredCode = 0x0505;
enum class Action : uint8_t { None, RestartStation, QueueRestart, Fallback };

// RTC storage: no wall clock required. Two resets maximum until 30 minutes of
// continuous healthy operation. This is stricter than two per 30-minute window.
// A new boot never earns reset permission merely by being offline.
struct Retained {
  uint32_t magic, used, pending, reason, crc;
};
inline uint32_t checksum(const Retained &s) {
  uint32_t crc = UINT32_MAX;
  for (uint32_t value : {s.magic, s.used, s.pending, s.reason}) {
    for (unsigned byte = 0; byte < 4; ++byte) {
      crc ^= (value >> (byte * 8)) & 255;
      for (unsigned bit = 0; bit < 8; ++bit)
        crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
  }
  return ~crc;
}
inline void seal(Retained &s) { s.magic = 0x52454331; s.crc = checksum(s); }
inline bool valid(const Retained &s) {
  return s.magic == 0x52454331 && s.used <= 2 && s.pending <= 1 &&
      (s.reason == 0 || s.reason == queuedCode || s.reason == stalledCode) && s.crc == checksum(s);
}
class Budget {
 public:
  void boot(Retained &s, bool softwareReset) {
    if (!valid(s)) s = {};
    credit_ = softwareReset && valid(s) && s.pending == 1 && s.used < 2;
    s.pending = 0; seal(s);
    healthy_ = false;
  }
  void observe(Retained &s, bool connected, uint32_t now) {
    if (!connected) { healthy_ = false; return; }
    if (!healthy_) { healthy_ = true; healthyAt_ = now; }
    // Require a stable usable IP, not a transient GOT_IP event.
    if (uint32_t(now - healthyAt_) >= 30000) credit_ = true;
    if (uint32_t(now - healthyAt_) >= healthyRearmMs && s.used) {
      s.used = s.pending = 0; seal(s);
    }
  }
  bool available(const Retained &s) const { return credit_ && valid(s) && s.used < 2; }
  bool consume(Retained &s, uint16_t reason = queuedCode) {
    if (!available(s)) return false;
    ++s.used; s.pending = 1; s.reason = reason; seal(s); credit_ = false; return true;
  }
 private:
  uint32_t healthyAt_ = 0;
  bool credit_ = false, healthy_ = false;
};

struct Input {
  bool configured = false, connected = false, protectedWork = true;
  bool budget = false;
};
class Wifi {
 public:
  void manual(uint32_t now) {
    *this = Wifi{}; grace_ = true; graceAt_ = now;
  }
  void fault() { if (faults_ < 255) ++faults_; }
  uint8_t stage() const { return fallback_ ? 4 : grace_ ? 1 : stacks_ ? 3 : offline_ ? 2 : 0; }
  uint8_t stacks() const { return stacks_; }
  uint8_t faults() const { return faults_; }
  uint32_t offlineAge(uint32_t now) const { return offline_ ? uint32_t(now - offlineAt_) : 0; }
  Action step(uint32_t now, Input in) {
    if (!in.configured || in.connected) {
      offline_ = false; stacks_ = faults_ = 0; fallback_ = false;
      return Action::None;
    }
    if (!offline_) { offline_ = true; offlineAt_ = now; }
    if (grace_ && uint32_t(now - graceAt_) < manualGraceMs) return Action::None;
    grace_ = false;
    if (in.protectedWork || fallback_) return Action::None;
    const uint32_t age = now - offlineAt_;
    if (stacks_ < 3 && age >= stackAfterMs &&
        (!stacks_ || uint32_t(now - stackAt_) >= stackSpacingMs)) {
      ++stacks_; stackAt_ = now; return Action::RestartStation;
    }
    // Normal no-AP/auth/DHCP timeouts alone do not prove a driver fault.
    if (age >= rebootAfterMs && stacks_ >= 3 && faults_ >= 3) {
      fallback_ = true;
      return in.budget ? Action::QueueRestart : Action::Fallback;
    }
    return Action::None;
  }
 private:
  uint32_t offlineAt_ = 0, stackAt_ = 0, graceAt_ = 0;
  uint8_t stacks_ = 0, faults_ = 0;
  bool offline_ = false, grace_ = false, fallback_ = false;
};

// Shared exclusion between the independent watchdog and network-owned operations.
// A watchdog claim cannot race a flash/command operation, even with an old View.
class Gate {
 public:
  bool owner() {
    uint32_t expected = state_.load();
    while (expected == 0 || expected == 2) {
      // A retained block must never pass through a claimable state.
      const uint32_t owned = expected == 2 ? 4U : 1U;
      if (state_.compare_exchange_weak(expected, owned)) return true;
    }
    return false;
  }
  // Ownership alone is revocable. Protection and watchdog claim compete in
  // the same atomic word, before any command mutation or flash operation.
  bool protect() {
    uint32_t expected = 1;
    return state_.compare_exchange_strong(expected, 4) || expected == 4;
  }
  bool protectedOwner() const { return state_.load() == 4; }
  void release(bool blocked) {
    uint32_t expected = state_.load();
    while (expected == 1 || expected == 4) {
      if (state_.compare_exchange_weak(expected, blocked ? 2U : 0U)) return;
    }
  }
  bool claim() {
    uint32_t expected = state_.load();
    while (expected == 0 || expected == 1) {
      if (state_.compare_exchange_weak(expected, 3)) return true;
    }
    return false;
  }
  void cancel() { uint32_t expected = 3; state_.compare_exchange_strong(expected, 0); }
 private:
  std::atomic<uint32_t> state_{2}; // Boot is protected until the first owner release.
};
inline bool stalled(bool started, uint32_t stageAge, uint32_t uptime,
                    bool priorHealthy, bool safeView) {
  return started && priorHealthy && safeView && uptime >= stallAfterMs && stageAge >= stallAfterMs;
}
// Revalidate after claiming, using a synchronized snapshot followed by the clock.
// Any intervening progress cancels this attempt; a new stall gets a new sample.
template<class Heartbeat>
inline bool stillStalled(const Heartbeat &sample, const Heartbeat &current, uint32_t now) {
  return current.started && current.stage != decltype(current.stage)::Starting &&
      current.heartbeat == sample.heartbeat && current.stage == sample.stage &&
      current.enteredAtMs == sample.enteredAtMs && current.completedLoops == sample.completedLoops &&
      stalled(current.started, uint32_t(now - current.enteredAtMs), now, true, true);
}
} // namespace recovery
