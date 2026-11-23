#include "diagnostic_persistence.hpp"
#include "diagnostics.hpp"

#include <atomic>
#include <cstring>
#include <new>
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifdef ESP_PLATFORM
extern "C" { extern volatile uint32_t ota_rejection_mailbox[32]; }
#else
static volatile uint32_t ota_rejection_mailbox[32]{};
#endif

namespace diagnostic_persistence {
namespace {
constexpr uint32_t kFlushIntervalMs = 30000;
constexpr uint32_t kRetryIntervalMs = 60000;
constexpr unsigned kBatchEntries = 16;
StaticSemaphore_t mutexStorage;
SemaphoreHandle_t mutex = nullptr;
alignas(diagnostic_archive::Store) unsigned char storeStorage[sizeof(diagnostic_archive::Store)];
diagnostic_archive::Store* store = nullptr;
std::atomic<bool> available{false}, failed{false};
std::atomic<bool> bootPending{false};
std::atomic<uint32_t> revision{0}, savedRevision{0}, lastSavedAt{0};
int64_t nextFlushAt = 0, retryAt = 0;
uint64_t firmwareVersion = 0, savedSequence = 0;
uint32_t bootId = 0, resetCause = 0, savedRepeats = 0, savedLastMs = 0;
char archiveDirectory[384]{};

// Fixed wire layout and address so the fallback image can recover the trial's reason.
struct Rejection {
  uint32_t magic, checksum, versionLow, versionHigh, boot, reset, uptime;
  int32_t reason, detail;
  char text[92];
};
static_assert(sizeof(Rejection) == 128 && offsetof(Rejection, text) == 36);
constexpr uint32_t kRejectionMagic = 0x4f544131; // OTA1
std::atomic<bool> rejectionPending{false};
uint32_t checksum(const Rejection& record) {
  const auto* bytes = reinterpret_cast<const unsigned char*>(&record);
  uint32_t hash = 2166136261u;
  for (size_t i = offsetof(Rejection, versionLow); i < sizeof(record); ++i)
    hash = (hash ^ bytes[i]) * 16777619u;
  return hash;
}

bool take() {
  return mutex && xSemaphoreTake(mutex, pdMS_TO_TICKS(5)) == pdTRUE;
}
bool write(diagnostic_archive::Record record, int64_t now) {
  if (!take()) return false;
  const bool result = store->append(record);
  xSemaphoreGive(mutex);
  if (result) lastSavedAt.store(static_cast<uint32_t>(now), std::memory_order_relaxed);
  return result;
}
diagnostic_archive::Record bootRecord() {
  diagnostic_archive::Record start;
  start.firmware_version = firmwareVersion;
  start.boot_id = bootId;
  start.reset_cause = resetCause;
  start.entry.severity = diagnostic_log::Severity::Info;
  start.entry.code = static_cast<uint16_t>(diagnostic_log::Code::Boot);
  diagnostic_log::copyAscii(start.entry.text, "Controller started", 18);
  return start;
}
bool importRejection(int64_t now) {
  if (!rejectionPending.load()) return true;
  Rejection saved{};
  const auto* src = reinterpret_cast<const volatile unsigned char*>(&ota_rejection_mailbox);
  auto* dst = reinterpret_cast<unsigned char*>(&saved);
  for (size_t i = 0; i < sizeof(saved); ++i) dst[i] = src[i];
  if (saved.magic == kRejectionMagic && saved.checksum == checksum(saved)) {
    diagnostic_archive::Record record;
    record.firmware_version = (uint64_t(saved.versionHigh) << 32) | saved.versionLow;
    record.boot_id = saved.boot;
    record.reset_cause = saved.reset;
    record.entry.sequence = UINT64_MAX; // Stable identity if reset interrupts import.
    record.entry.uptime_ms = record.entry.last_uptime_ms = saved.uptime;
    record.entry.severity = diagnostic_log::Severity::Error;
    record.entry.module = diagnostic_log::Module::Ota;
    record.entry.code = static_cast<uint16_t>(diagnostic_log::Code::UpdateFailed);
    record.entry.snapshot.reason = saved.reason;
    record.entry.snapshot.detail = saved.detail;
    diagnostic_log::copyAscii(record.entry.text, saved.text, sizeof(saved.text));
    if (!write(record, now)) return false;
  }
  ota_rejection_mailbox[0] = 0;
  rejectionPending.store(false);
  return true;
}
void retryLater(int64_t now) {
  failed.store(true, std::memory_order_relaxed);
  retryAt = now + kRetryIntervalMs;
}
} // namespace

void changed() noexcept {
  revision.fetch_add(1, std::memory_order_relaxed);
}

void retainOtaRejection(uint64_t firmware, const char* status, int32_t reason, int32_t detail) noexcept {
  Rejection saved{};
  saved.versionLow = uint32_t(firmware);
  saved.versionHigh = uint32_t(firmware >> 32);
  saved.boot = bootId ? bootId : esp_random();
  saved.reset = uint32_t(esp_reset_reason());
  saved.uptime = uint32_t(esp_timer_get_time() / 1000);
  saved.reason = reason;
  saved.detail = detail;
  for (size_t i = 0; i + 1 < sizeof(saved.text) && status[i]; ++i) saved.text[i] = status[i];
  saved.checksum = checksum(saved);
  ota_rejection_mailbox[0] = 0;
  const auto* src = reinterpret_cast<const unsigned char*>(&saved);
  auto* dst = reinterpret_cast<volatile unsigned char*>(&ota_rejection_mailbox);
  for (size_t i = sizeof(saved.magic); i < sizeof(saved); ++i) dst[i] = src[i];
  std::atomic_thread_fence(std::memory_order_seq_cst);
  ota_rejection_mailbox[0] = kRejectionMagic;
  std::atomic_thread_fence(std::memory_order_seq_cst);
  rejectionPending.store(true);
}

bool archiveOtaRejection(int64_t now) noexcept {
  return available.load(std::memory_order_acquire) && importRejection(now);
}

bool initialize(const char* directory, uint64_t firmware, uint32_t boot,
                uint32_t reset_cause) noexcept {
  firmwareVersion = firmware;
  bootId = boot;
  resetCause = reset_cause;
  rejectionPending.store(ota_rejection_mailbox[0] == kRejectionMagic);
  if (!directory || std::strlen(directory) >= sizeof(archiveDirectory)) return false;
  std::strcpy(archiveDirectory, directory);
  mutex = xSemaphoreCreateMutexStatic(&mutexStorage);
  auto* cache = static_cast<diagnostic_archive::Record*>(heap_caps_calloc(
      diagnostic_archive::kCapacity, sizeof(diagnostic_archive::Record),
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!mutex || !cache) { failed.store(true); return false; }
  for (std::size_t i = 0; i < diagnostic_archive::kCapacity; ++i)
    new (cache + i) diagnostic_archive::Record{};
  store = new (storeStorage) diagnostic_archive::Store(cache);
  bootPending.store(true, std::memory_order_relaxed);
  if (!store->open(archiveDirectory)) { retryLater(0); return false; }
  available.store(true, std::memory_order_release);
  if (!write(bootRecord(), 0)) { retryLater(0); return false; }
  bootPending.store(false, std::memory_order_relaxed);
  if (!importRejection(0)) { retryLater(0); return false; }
  nextFlushAt = kFlushIntervalMs;
  return true;
}

bool due(int64_t now, bool urgent) noexcept {
  if (!store || (!rejectionPending.load() && !bootPending.load(std::memory_order_relaxed) &&
      revision.load(std::memory_order_relaxed) == savedRevision.load(std::memory_order_relaxed))) return false;
  if (failed.load(std::memory_order_relaxed) && now < retryAt) return false;
  return urgent || now >= nextFlushAt;
}

bool flush(int64_t now) noexcept {
  if (!store) return false;
  if (!available.load(std::memory_order_acquire)) {
    if (!take()) return false;
    const bool opened = store->open(archiveDirectory);
    xSemaphoreGive(mutex);
    if (!opened) { retryLater(now); return false; }
    available.store(true, std::memory_order_release);
  }
  if (bootPending.load(std::memory_order_relaxed)) {
    if (!write(bootRecord(), now)) { retryLater(now); return false; }
    bootPending.store(false, std::memory_order_relaxed);
  }
  if (!importRejection(now)) { retryLater(now); return false; }
  const uint32_t targetRevision = revision.load(std::memory_order_relaxed);
  diagnostics::Cursor cursor;
  if (!diagnostics::beginRead(cursor, savedSequence ? savedSequence - 1 : 0)) return false;
  unsigned written = 0;
  bool finished = false;
  for (unsigned scanned = 0; scanned < diagnostics::kCapacity; ++scanned) {
    diagnostics::Entry entry;
    diagnostics::Page page;
    if (!diagnostics::readAfter(cursor, entry, page)) return false;
    finished = page.done;
    if (page.count && !(entry.sequence == savedSequence &&
        entry.repeat_count == savedRepeats && entry.last_uptime_ms == savedLastMs)) {
      diagnostic_archive::Record record;
      record.firmware_version = firmwareVersion;
      record.boot_id = bootId;
      record.reset_cause = resetCause;
      record.entry = entry;
      if (!write(record, now)) {
        retryLater(now);
        return false;
      }
      savedSequence = entry.sequence;
      savedRepeats = entry.repeat_count;
      savedLastMs = entry.last_uptime_ms;
      ++written;
    }
    if (page.done || written == kBatchEntries) break;
  }
  if (finished) savedRevision.store(targetRevision, std::memory_order_relaxed);
  failed.store(false, std::memory_order_relaxed);
  nextFlushAt = now + kFlushIntervalMs;
  return true;
}

bool metadata(Metadata& out) noexcept {
  out = Metadata{};
  out.available = available.load(std::memory_order_acquire);
  out.failed = failed.load(std::memory_order_relaxed);
  out.pending = rejectionPending.load() || bootPending.load(std::memory_order_relaxed) ||
      revision.load(std::memory_order_relaxed) != savedRevision.load(std::memory_order_relaxed);
  out.last_saved_ms = lastSavedAt.load(std::memory_order_relaxed);
  if (!out.available) return true;
  if (!take()) return false;
  out.count = store->size();
  out.newest = store->newest();
  xSemaphoreGive(mutex);
  return true;
}

bool readAfter(uint64_t after, uint64_t until,
               diagnostic_archive::Record& out) noexcept {
  if (!available.load(std::memory_order_acquire) || !take()) return false;
  const bool result = store->readAfter(after, until, out);
  xSemaphoreGive(mutex);
  return result;
}
} // namespace diagnostic_persistence
