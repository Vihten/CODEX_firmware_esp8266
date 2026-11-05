#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>

namespace diagnostic_log {
constexpr std::size_t kMaxEntries = 512;
constexpr std::size_t kTextBytes = 140;  // Includes trailing NUL.
constexpr uint32_t kDefaultRetentionMs = 0;  // Disabled: preserve rare errors.
constexpr uint32_t kDefaultRepeatWindowMs = 30000;
constexpr uint32_t kMaxRetentionMs = 0x7fffffffu;

enum class Module : uint8_t { System, Wifi, Http, Ota, Alert, Output };
enum class Severity : uint8_t { Info, Warning, Error };
enum class Code : uint16_t {
  Boot, WifiDisconnected, WifiConnected, RequestFailed, RequestRecovered,
  UpdateFailed, AlertSourceFailed, OutputFailed
};

// Numeric, curated context only. Never pass pointers, credentials, URLs,
// SSIDs, response bodies, authorization headers or raw ESP_LOG output.
struct Snapshot {
  int32_t reason = 0;       // Driver/error reason or HTTP status.
  uint32_t attempts = 0;    // Retry count.
  uint32_t elapsed_ms = 0;  // Operation duration.
  uint32_t heap_free = 0;   // Recent health observation; see context_uptime_ms.
  uint32_t heap_min = 0;    // Minimum free heap bytes since boot.
  uint32_t largest_block = 0;  // Largest allocatable block, bytes.
  uint32_t network_stage = 0;  // Adapter-defined stable stage identifier.
  uint32_t network_age_ms = 0;
  uint32_t render_frames = 0;
  int16_t wifi_rssi = 0;  // dBm; interpret only when wifi_connected is true.
  uint8_t agents = 0;
  bool wifi_connected = false;
  int32_t detail = 0;  // Secondary error/failure enum; attempts remains retry count.
  uint32_t context_uptime_ms = 0; // Health observation time; not the event time.
  bool view_valid = false;        // Wi-Fi/render fields were read successfully.
};
struct Entry {
  uint64_t sequence = 0;
  uint32_t uptime_ms = 0;
  uint32_t unix_epoch_s = 0;  // 0 = NTP/wall clock unknown; export uptime instead.
  uint32_t last_uptime_ms = 0;
  uint32_t last_unix_epoch_s = 0;
  uint32_t repeat_count = 1;
  Snapshot snapshot{};
  uint16_t code = static_cast<uint16_t>(Code::Boot);
  Module module = Module::System;
  Severity severity = Severity::Info;
  char text[kTextBytes]{};
  bool text_truncated = false;
};
static_assert(sizeof(Entry) <= 256, "Diagnostic entry RAM budget exceeded");

// Bounded ASCII copy: controls/non-ASCII become '?'; always NUL-terminated.
// source_bytes is readable length, not a request to scan an unbounded string.
inline bool copyAscii(char (&out)[kTextBytes], const char* source,
                      std::size_t source_bytes) noexcept {
  for (std::size_t i = 0; i < kTextBytes; ++i) out[i] = 0;
  if (!source) return false;
  std::size_t i = 0;
  while (i < source_bytes && source[i] && i < kTextBytes - 1) {
    const auto c = static_cast<unsigned char>(source[i]);
    out[i++] = c >= 32 && c <= 126 ? static_cast<char>(c) : '?';
  }
  return i < source_bytes && source[i] != 0;
}

struct Counters {
  uint64_t overwritten = 0;
  uint64_t expired = 0;
  uint64_t rejected = 0;
  uint64_t coalesced = 0;
};
// Value cursor, independent of physical ring slots. Do not reuse across reboot
// or a different Ring. Treat fields as opaque and serialize uint64 as strings
// for JavaScript clients. until is inclusive; new writes cannot extend a page.
struct Cursor { uint64_t next = 1; uint64_t until = 0; };
struct Page {
  std::size_t count = 0;
  uint64_t lost = 0;  // Missing sequences in THIS call; sum across pages.
  bool done = true;
};

class Ring {
 public:
  // Storage must outlive this object, be exclusive, and contain Entry objects.
  // Invalid storage/capacity/retention disables writes; never allocates.
  explicit Ring(Entry* storage, std::size_t capacity,
                uint32_t retention_ms = kDefaultRetentionMs,
                uint32_t repeat_window_ms = kDefaultRepeatWindowMs) noexcept
      : storage_(storage), capacity_(capacity), retention_(retention_ms),
        repeat_window_(repeat_window_ms) {
    if (!storage || !capacity || capacity > kMaxEntries ||
        retention_ms > kMaxRetentionMs || repeat_window_ms > kMaxRetentionMs) capacity_ = 0;
  }
  Ring(const Ring&) = delete;
  Ring& operator=(const Ring&) = delete;
  bool valid() const noexcept { return capacity_ != 0; }
  std::size_t size() const noexcept { return size_; }
  Counters counters() const noexcept { return counters_; }

  // now: nondecreasing uptime modulo 2^32, including across wrap. Service at
  // least once per 2^31 ms (24.8 days); reboot requires a new Ring instance.
  // If enabled, at last-occurrence age == retention an entry expires. Call even when no events arrive.
  void prune(uint32_t now) noexcept {
    while (retention_ && size_ &&
           uint32_t(now - storage_[head_].last_uptime_ms) >= retention_) {
      removeOldest();
      ++counters_.expired;
    }
  }

  bool append(uint32_t now, Code code, Snapshot snapshot = {},
              uint32_t unix_epoch_s = 0) noexcept {
    Module module;
    Severity severity;
    const char* message;
    switch (code) {
      case Code::Boot: module = Module::System; severity = Severity::Info;
        message = "Controller started"; break;
      case Code::WifiDisconnected: module = Module::Wifi; severity = Severity::Warning;
        message = "Wi-Fi disconnected"; break;
      case Code::WifiConnected: module = Module::Wifi; severity = Severity::Info;
        message = "Wi-Fi connected"; break;
      case Code::RequestFailed: module = Module::Http; severity = Severity::Error;
        message = "Request failed"; break;
      case Code::RequestRecovered: module = Module::Http; severity = Severity::Info;
        message = "Request recovered"; break;
      case Code::UpdateFailed: module = Module::Ota; severity = Severity::Error;
        message = "Firmware update failed"; break;
      case Code::AlertSourceFailed: module = Module::Alert; severity = Severity::Error;
        message = "Alert source failed"; break;
      case Code::OutputFailed: module = Module::Output; severity = Severity::Error;
        message = "Output failed"; break;
      default: ++counters_.rejected; return false;
    }
    return append(now, severity, module, static_cast<uint16_t>(code), message,
                  kTextBytes - 1, snapshot, unix_epoch_s);
  }

  // Caller supplies ONLY curated, secret-free messages and a readable byte
  // bound (a char[140] can pass sizeof(message)); no printf formatting/scanning.
  // Known wall-clock epoch is captured per entry, never inferred retrospectively.
  bool append(uint32_t now, Severity severity, Module module, uint16_t code,
              const char* message, std::size_t message_bytes,
              Snapshot snapshot = {}, uint32_t unix_epoch_s = 0) noexcept {
    prune(now);
    if (static_cast<uint8_t>(severity) > static_cast<uint8_t>(Severity::Error) ||
        static_cast<uint8_t>(module) > static_cast<uint8_t>(Module::Output)) {
      ++counters_.rejected;
      return false;
    }
    // Reserve UINT64_MAX as exhausted sentinel; never wrap sequence numbers.
    if (!valid() || next_ == std::numeric_limits<uint64_t>::max()) {
      ++counters_.rejected;
      return false;
    }
    char text[kTextBytes];
    const bool truncated = copyAscii(text, message, message_bytes);
    if (size_ && repeat_window_ && !truncated) {
      Entry& tail = storage_[(head_ + size_ - 1) % capacity_];
      bool sameText = true;
      for (std::size_t i = 0; i < kTextBytes; ++i) {
        if (tail.text[i] != text[i]) { sameText = false; break; }
      }
      if (!tail.text_truncated && tail.module == module && tail.code == code && tail.severity == severity &&
          sameText && tail.snapshot.reason == snapshot.reason && tail.snapshot.detail == snapshot.detail &&
          uint32_t(now - tail.last_uptime_ms) <= repeat_window_ &&
          tail.repeat_count != std::numeric_limits<uint32_t>::max()) {
        ++tail.repeat_count;
        tail.snapshot = snapshot;  // Latest health context; reason is identity.
        tail.last_uptime_ms = now;
        tail.last_unix_epoch_s = unix_epoch_s;
        ++counters_.coalesced;
        return true;
      }
    }
    if (size_ == capacity_) { removeOldest(); ++counters_.overwritten; }
    Entry& entry = storage_[(head_ + size_) % capacity_];
    entry = Entry{};
    entry.sequence = next_++;
    entry.uptime_ms = now;
    entry.unix_epoch_s = unix_epoch_s;
    entry.last_uptime_ms = now;
    entry.last_unix_epoch_s = unix_epoch_s;
    entry.snapshot = snapshot;
    entry.text_truncated = truncated;
    entry.code = code;
    entry.module = module;
    entry.severity = severity;
    for (std::size_t i = 0; i < kTextBytes; ++i) entry.text[i] = text[i];
    ++size_;
    return true;
  }

  // after=0 starts at oldest available without claiming historical loss.
  // A nonzero after resumes an earlier sequence and reports any missing range.
  Cursor begin(uint32_t now, uint64_t after = 0) noexcept {
    prune(now);
    const uint64_t end = next_ - 1;
    if (after >= end) return Cursor{next_, end};
    return Cursor{after ? after + 1 : (size_ ? storage_[head_].sequence : next_), end};
  }

  // Copies into caller-owned storage (must not overlap ring storage). Lock
  // around the entire call. nullptr/zero output makes no cursor progress.
  Page read(uint32_t now, Cursor& cursor, Entry* out,
            std::size_t output_capacity) noexcept {
    prune(now);
    Page page;
    if (cursor.next > cursor.until) return page;
    page.done = false;
    if (!out || !output_capacity) return page;
    const uint64_t oldest = size_ ? storage_[head_].sequence : next_;
    const uint64_t boundary = cursor.until < oldest ? cursor.until + 1 : oldest;
    if (cursor.next < boundary) {
      page.lost = boundary - cursor.next;
      cursor.next = boundary;
    }
    for (std::size_t i = 0; i < size_ && page.count < output_capacity; ++i) {
      const Entry& entry = storage_[(head_ + i) % capacity_];
      if (entry.sequence < cursor.next) continue;
      if (entry.sequence > cursor.until) break;
      out[page.count++] = entry;
      cursor.next = entry.sequence + 1;
    }
    page.done = cursor.next > cursor.until;
    return page;
  }

 private:
  void removeOldest() noexcept { head_ = (head_ + 1) % capacity_; --size_; }
  Entry* storage_;
  std::size_t capacity_;
  uint32_t retention_;
  uint32_t repeat_window_;
  std::size_t head_ = 0;
  std::size_t size_ = 0;
  uint64_t next_ = 1;
  Counters counters_{};
};

// IDF integration contract (outside this portable core):
// - Allocate external Entry storage[256] in RAM/PSRAM, then Ring(storage,256).
//   Maximum supported capacity is 512; sizeof(Entry)<=256 (256 slots <=64 KiB). No flash/NVS writes.
// - Default age retention is disabled: rare errors survive until overwritten.
//   Optional ten-minute policy: Ring(storage,256,600000).
// - One external task mutex protects ALL calls, including size/counters/begin.
//   No ISR calls; queue typed events from ISR to the owning task if needed.
// - Feed uint32 millisecond uptime. prune periodically even without reads.
// - Wrapper selects warnings/errors only; no periodic informational snapshots.
// - healthContext fills all Snapshot fields at failure time. reason identifies
//   the primary HTTP/driver error; detail carries a secondary failure/TLS enum.
//   Export every Snapshot state field numerically (wifi_connected as 0/1).
//   attempts always means retry count; elapsed_ms is operation duration.
// - Emit on curated failures, recovery and state transitions; rate-limit storms
//   upstream. Caller messages must be curated and secret-free. No ESP_LOG hook.
// - Export unix_epoch_s when nonzero, otherwise uptime_ms with an explicit
//   uptime time basis; never format unknown uptime as a Unix date.
// - Authenticated routes call begin then read into a small caller-owned page
//   under the mutex; release mutex BEFORE JSON encoding or network I/O.
// - Return cursor, entries, page.lost and cumulative counters. Keep until fixed
//   on subsequent requests. Missing overwritten/expired entries are reported,
//   not retained by a snapshot. Copying a page makes that page stable.
// - Adjacent equal module/code/severity/text/reason coalesce when
//   within 30 s of last occurrence (constructor argument 4; zero disables).
//   Truncated messages never coalesce, avoiding false prefix matches.
//   Latest Snapshot replaces prior context, including attempts and elapsed_ms.
//   Sequence and first times stay fixed, count and last times update. Refresh
//   from before the tail sequence to see repeats; sequence cursors track entries,
//   not mutations. Cursor upper bound freezes membership, not entry contents.
// - No restart persistence. A boot/session identifier belongs in the adapter
//   to reject old cursors. Controller logs explain only curated captured events.
}  // namespace diagnostic_log
