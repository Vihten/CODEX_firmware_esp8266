#include "diagnostics.hpp"
#include "diagnostic_persistence.hpp"

#include <atomic>
#include <new>
#include <cstddef>
#include <type_traits>
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace diagnostics {
namespace {
constexpr uint32_t kMagic = 0x44494147;
constexpr uint32_t kSchema = 1;
struct Retained {
  uint32_t magic;
  uint32_t schema;
  uint32_t generation;
  uint32_t version_low;
  uint32_t version_high;
  uint32_t reset_cause;
  uint32_t uptime_ms;
  uint32_t heap_min;
  uint32_t network_stage;
  uint32_t crc;
};
static_assert(sizeof(Retained) == 40, "RTC summary must stay small and padding-free");
static_assert(std::is_trivially_default_constructible<Retained>::value,
              "RTC memory must not have a startup constructor");
RTC_NOINIT_ATTR volatile Retained retained[2];
uint32_t generation = 0;
unsigned retainedSlot = 0;
uint64_t firmwareVersion = 0;
portMUX_TYPE contextMux = portMUX_INITIALIZER_UNLOCKED;
StaticSemaphore_t mutexStorage;
SemaphoreHandle_t mutex = nullptr;
std::atomic<uint32_t> state{0}; // 0=new, 1=initializing, 2=ready, 3=failed
std::atomic<uint32_t> dropped{0};
alignas(diagnostic_log::Ring) unsigned char ringStorage[sizeof(diagnostic_log::Ring)];
diagnostic_log::Ring* ring = nullptr;
Metadata boot;
HealthContext latest;
bool networkWarned = false;
bool heapWarned = false;
uint32_t networkWarningAt = 0;
uint32_t heapWarningAt = 0;

uint32_t now() noexcept { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }
uint32_t crc(const Retained& value) noexcept {
  uint32_t result = 0xffffffffu;
  const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
  for (std::size_t i = 0; i < offsetof(Retained, crc); ++i) {
    result ^= bytes[i];
    for (unsigned bit = 0; bit < 8; ++bit)
      result = (result >> 1) ^ (0xedb88320u & (0u - (result & 1u)));
  }
  return ~result;
}
void drop() noexcept {
  uint32_t previous = dropped.load(std::memory_order_relaxed);
  while (previous != UINT32_MAX && !dropped.compare_exchange_weak(
      previous, previous + 1, std::memory_order_relaxed)) {}
}
bool take() noexcept {
  if (xPortInIsrContext() || state.load(std::memory_order_acquire) != 2) return false;
  // Floor conversion ensures the requested timeout never exceeds 5ms. On a
  // coarse tick configuration this is a nonblocking try-take.
  return xSemaphoreTake(mutex, pdMS_TO_TICKS(5)) == pdTRUE;
}
const char* message(uint16_t code) noexcept {
  switch (code) {
    case 0x0100: return "Task creation failed";
    case 0x0101: return "Storage failed";
    case 0x0102: return "Maintenance timed out";
    case 0x0103: return "Socket initialization failed";
    case 0x0200: return "Wi-Fi event queue overflow";
    case 0x0201: return "Wi-Fi connection loss reconciled";
    case 0x0202: return "Wi-Fi initialization failed";
    case 0x0203: return "Wi-Fi connect request failed";
    case 0x0204: return "Wi-Fi connection timed out";
    case 0x0300: return "Authentication rejected";
    case 0x0301: return "Authentication throttled";
    case 0x0302: return "Authentication form invalid";
    case 0x0303: return "Authentication crypto failed";
    case 0x0500: return "Wi-Fi station recovery started";
    case 0x0501: return "Wi-Fi station recovery failed";
    case 0x0502: return "Automatic recovery restart queued";
    case 0x0503: return "Recovery restart budget exhausted; setup AP retained";
    case 0x0504: return "Network stalled; independent recovery restart";
    case 0x0505: return "Network recovery completed";
    case static_cast<uint16_t>(Code::WifiDisconnected): return "Wi-Fi disconnected";
    case static_cast<uint16_t>(Code::RequestFailed): return "Request failed";
    case static_cast<uint16_t>(Code::UpdateFailed): return "Firmware update failed";
    case static_cast<uint16_t>(Code::AlertSourceFailed): return "Alert source failed";
    case static_cast<uint16_t>(Code::OutputFailed): return "Output failed";
    case kNetworkStalled: return "Network progress stalled";
    case kHeapPressure: return "Heap pressure";
    default: return "Diagnostic failure";
  }
}
} // namespace

bool initialize(uint64_t fwVersion, uint32_t resetReason) noexcept {
  if (xPortInIsrContext()) return false;
  uint32_t expected = 0;
  if (!state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel))
    return expected == 2;
  firmwareVersion = fwVersion;
  boot.firmware_version = fwVersion;
  boot.reset_cause = resetReason == UINT32_MAX ? static_cast<uint32_t>(esp_reset_reason()) : resetReason;
  boot.boot_id = esp_random();
  for (unsigned slot = 0; slot < 2; ++slot) {
    Retained saved{};
    const volatile auto* source = reinterpret_cast<const volatile unsigned char*>(&retained[slot]);
    auto* destination = reinterpret_cast<unsigned char*>(&saved);
    for (std::size_t i = 0; i < sizeof(saved); ++i) destination[i] = source[i];
    if (saved.magic != kMagic || saved.schema != kSchema || saved.crc != crc(saved)) continue;
    if (boot.previous_boot_valid && uint32_t(saved.generation - generation) >= 0x80000000u) continue;
    boot.previous_boot_valid = true;
    generation = saved.generation;
    retainedSlot = slot;
    boot.previous_boot = {uint64_t(saved.version_low) | (uint64_t(saved.version_high) << 32),
        saved.reset_cause, saved.uptime_ms, saved.heap_min, saved.network_stage};
  }
  mutex = xSemaphoreCreateMutexStatic(&mutexStorage);
  auto* entries = mutex ? static_cast<Entry*>(heap_caps_malloc(
      sizeof(Entry) * kCapacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) : nullptr;
  if (entries) {
    for (std::size_t i = 0; i < kCapacity; ++i) new (entries + i) Entry{};
    ring = new (ringStorage) diagnostic_log::Ring(entries, kCapacity, 0);
  }
  state.store(entries ? 2 : 3, std::memory_order_release);
  Summary initial{};
  initial.version = fwVersion;
  initial.reset_cause = boot.reset_cause;
  initial.uptime_ms = now();
  initial.heap_min = static_cast<uint32_t>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  Context initialContext{};
  initialContext.summary = initial;
  setContext(initialContext);
  checkpoint(initial);
  return entries != nullptr;
}

bool event(Level level, Module module, uint16_t code,
           diagnostic_log::Snapshot context) noexcept {
  if (level == Level::Info) return false;
  Context captured{};
  if (!getContext(captured)) { drop(); return false; }
  context.context_uptime_ms = captured.summary.uptime_ms;
  context.view_valid = captured.view_valid;
  context.heap_free = captured.heap_free;
  context.heap_min = captured.summary.heap_min;
  context.largest_block = captured.largest_block;
  context.network_stage = captured.summary.network_stage;
  context.network_age_ms = captured.network_age_ms;
  context.render_frames = captured.render_frames;
  context.wifi_rssi = static_cast<int16_t>(captured.wifi_rssi);
  context.agents = static_cast<uint8_t>(captured.agents > 255 ? 255 : captured.agents);
  context.wifi_connected = captured.wifi_connected;
  if (!take()) { drop(); return false; }
  const char* text = message(code);
  const bool result = ring->append(now(), level, module, code, text,
                                  diagnostic_log::kTextBytes - 1, context, captured.unix_epoch_s);
  xSemaphoreGive(mutex);
  if (result) diagnostic_persistence::changed();
  else drop();
  return result;
}
bool event(Level level, Module module, uint16_t code, const char*, ...) noexcept {
  return event(level, module, code, diagnostic_log::Snapshot{});
}
bool event(Level level, Module module, Code code, const char*, ...) noexcept {
  return event(level, module, static_cast<uint16_t>(code), diagnostic_log::Snapshot{});
}

bool getMetadata(Metadata& out) noexcept {
  out = Metadata{};
  if (xPortInIsrContext()) return false;
  const uint32_t current = state.load(std::memory_order_acquire);
  if (current < 2) return false;
  out = boot;
  out.dropped = dropped.load(std::memory_order_relaxed);
  portENTER_CRITICAL(&contextMux);
  out.latest = latest;
  portEXIT_CRITICAL(&contextMux);
  if (!take()) return false;
  out.available = true;
  out.count = ring->size();
  out.counters = ring->counters();
  xSemaphoreGive(mutex);
  return true;
}
bool beginRead(Cursor& cursor, uint64_t after) noexcept {
  if (!take()) return false;
  cursor = ring->begin(now(), after);
  xSemaphoreGive(mutex);
  return true;
}
bool readAfter(Cursor& cursor, Entry& out, Page& page) noexcept {
  out = Entry{};
  page = Page{};
  if (!take()) return false;
  page = ring->read(now(), cursor, &out, 1);
  xSemaphoreGive(mutex);
  return true;
}

void checkpoint(const Summary& summary) noexcept {
  if (xPortInIsrContext() || state.load(std::memory_order_acquire) < 2) return;
  portENTER_CRITICAL(&contextMux);
  Retained next{kMagic, kSchema, generation + 1,
      static_cast<uint32_t>(firmwareVersion), static_cast<uint32_t>(firmwareVersion >> 32),
      boot.reset_cause, summary.uptime_ms, summary.heap_min, summary.network_stage, 0};
  next.crc = crc(next);
  const unsigned slot = retainedSlot ^ 1u;
  // Commit alternate slot last. A mid-update reset leaves the old slot intact.
  retained[slot].magic = 0;
  const auto* source = reinterpret_cast<const unsigned char*>(&next);
  volatile auto* destination = reinterpret_cast<volatile unsigned char*>(&retained[slot]);
  for (std::size_t i = sizeof(next.magic); i < sizeof(next); ++i) destination[i] = source[i];
  std::atomic_thread_fence(std::memory_order_seq_cst);
  retained[slot].magic = kMagic;
  generation = next.generation;
  retainedSlot = slot;
  portEXIT_CRITICAL(&contextMux);
}
void setContext(const Context& context) noexcept {
  if (xPortInIsrContext() || state.load(std::memory_order_acquire) < 2) return;
  Context fresh = context;
  fresh.summary.version = firmwareVersion;
  fresh.summary.reset_cause = boot.reset_cause;
  fresh.summary.uptime_ms = now();
  portENTER_CRITICAL(&contextMux);
  latest = fresh;
  portEXIT_CRITICAL(&contextMux);
}
bool getContext(Context& context) noexcept {
  context = Context{};
  if (xPortInIsrContext() || state.load(std::memory_order_acquire) < 2) return false;
  portENTER_CRITICAL(&contextMux);
  context = latest;
  portEXIT_CRITICAL(&contextMux);
  return true;
}
void healthCheck(const HealthContext& context, uint32_t heap_warn_bytes) noexcept {
  if (xPortInIsrContext() || state.load(std::memory_order_acquire) < 2) return;
  HealthContext fresh = context;
  fresh.summary.reset_cause = boot.reset_cause;
  fresh.summary.version = firmwareVersion;
  fresh.summary.uptime_ms = now();
  bool networkWarning = false;
  bool heapWarning = false;
  const bool stalled = fresh.network_started && fresh.network_age_ms >= 5000;
  const bool pressure = heap_warn_bytes && fresh.heap_free < heap_warn_bytes;
  portENTER_CRITICAL(&contextMux);
  latest = fresh;
  networkWarning = stalled && (!networkWarned ||
      uint32_t(fresh.summary.uptime_ms - networkWarningAt) >= 30000);
  heapWarning = pressure && (!heapWarned ||
      uint32_t(fresh.summary.uptime_ms - heapWarningAt) >= 30000);
  if (networkWarning) { networkWarned = true; networkWarningAt = fresh.summary.uptime_ms; }
  if (heapWarning) { heapWarned = true; heapWarningAt = fresh.summary.uptime_ms; }
  portEXIT_CRITICAL(&contextMux);
  diagnostic_log::Snapshot anomaly{};
  anomaly.elapsed_ms = fresh.network_age_ms;
  if (networkWarning) event(Level::Warning, Module::System, kNetworkStalled, anomaly);
  if (heapWarning) event(Level::Warning, Module::System, kHeapPressure, anomaly);
}
} // namespace diagnostics
