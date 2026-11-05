#pragma once

#include "diagnostic_log.hpp"

// Task context only. Call initialize after initializeRuntime, when assets and
// PSRAM are valid. No ESP_LOG interception, flash writes, or owned RTOS task.
namespace diagnostics {
using Level = diagnostic_log::Severity;
using Module = diagnostic_log::Module;
using Code = diagnostic_log::Code;
using Entry = diagnostic_log::Entry;
using Cursor = diagnostic_log::Cursor;
using Page = diagnostic_log::Page;
constexpr std::size_t kCapacity = 256;
constexpr uint16_t kNetworkStalled = 0xff00;
constexpr uint16_t kHeapPressure = 0xff01;

// Numeric fields only; version is the caller's firmware/build version number.
struct Summary {
  uint64_t version = 0;
  uint32_t reset_cause = 0;
  uint32_t uptime_ms = 0;
  uint32_t heap_min = 0;
  uint32_t network_stage = 0;
};
struct HealthContext {
  Summary summary{};
  uint32_t network_age_ms = 0;
  uint32_t heap_free = 0;
  uint32_t largest_block = 0;
  uint32_t render_frames = 0;
  uint32_t network_stack = 0;
  uint32_t unix_epoch_s = 0;
  int32_t wifi_rssi = 0;
  uint32_t agents = 0;
  bool wifi_connected = false;
  bool network_started = false;
  bool view_valid = false;
};
using Context = HealthContext;
struct Metadata {
  bool available = false;
  bool previous_boot_valid = false;
  uint32_t reset_cause = 0;
  uint32_t boot_id = 0; // Reject cursors from other boots at the route boundary.
  uint64_t firmware_version = 0;
  uint32_t dropped = 0; // Failed event writes; does not count filtered INFO.
  std::size_t capacity = kCapacity;
  std::size_t count = 0;
  diagnostic_log::Counters counters{};
  Summary previous_boot{};
  HealthContext latest{};
};

bool initialize(uint64_t fwVersion = 0, uint32_t resetReason = UINT32_MAX) noexcept;
void setContext(const Context& context) noexcept;
bool getContext(Context& context) noexcept;
bool event(Level level, Module module, uint16_t code,
           diagnostic_log::Snapshot context = {}) noexcept;
inline bool record(Level level, Module module, uint16_t code,
                   diagnostic_log::Snapshot context = {}) noexcept {
  return event(level, module, code, context);
}
inline bool record(Level level, Module module, Code code,
                   diagnostic_log::Snapshot context = {}) noexcept {
  return event(level, module, static_cast<uint16_t>(code), context);
}
inline bool event(Level level, Module module, Code code,
                  diagnostic_log::Snapshot context = {}) noexcept {
  return event(level, module, static_cast<uint16_t>(code), context);
}
// Compatibility signature. Caller text and varargs are deliberately NOT read:
// only the curated code's built-in message is stored. Use typed numeric context
// above. This prevents accidental URL/auth/client data ingestion via %s.
bool event(Level level, Module module, uint16_t code,
           const char* safeformat, ...) noexcept;
bool event(Level level, Module module, Code code,
           const char* safeformat, ...) noexcept;

// False = unavailable/busy/ISR. Output is initialized even on failure. Each
// read copies at most ONE entry, with no serialization or I/O under the lock.
bool getMetadata(Metadata& out) noexcept;
bool beginRead(Cursor& cursor, uint64_t after = 0) noexcept;
bool readAfter(Cursor& cursor, Entry& out, Page& page) noexcept;

// Independent monitor calls every ~2s using synchronized, fresh observations.
// network_age >= 5000ms and heap_free < nonzero heap_warn_bytes are anomalies.
// Repeated warnings are throttled to 30s per category; healthy calls append
// nothing. Context uses a separate short critical section, never ring lock.
// Caller invokes checkpoint separately (~10s); RTC survives some soft resets,
// not power loss. No flash persistence is provided.
void healthCheck(const HealthContext& context,
                 uint32_t heap_warn_bytes = 32768) noexcept;
void checkpoint(const Summary& summary) noexcept;
} // namespace diagnostics
