#include "daily_runtime.hpp"
#include "daily.hpp"
#include <cstdio>
#include <cstring>
#include <new>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "project_config.hpp"

namespace daily_runtime {
namespace {
daily::Store *store = nullptr;
uint8_t *blob = nullptr;
StaticSemaphore_t mutexStorage;
SemaphoreHandle_t mutex = nullptr;
bool available = false, saving = false, saveFailed = false;
uint64_t identities[daily::kAgents] = {};
bool bound[daily::kAgents] = {};
uint32_t savedAt = 0;
struct Lock {
  bool held = mutex && xSemaphoreTake(mutex, pdMS_TO_TICKS(20)) == pdTRUE;
  ~Lock() { if (held) xSemaphoreGive(mutex); }
};
bool anyDirty() {
  if (!available) return false;
  for (size_t i = 0; i < daily::kAgents; ++i) if (store->dirty(i)) return true;
  return false;
}
bool number(const char *text, unsigned base, uint64_t maximum, uint64_t &out) {
  out = 0;
  if (!*text) return false;
  for (; *text; ++text) {
    const unsigned digit = *text >= '0' && *text <= '9' ? unsigned(*text - '0') :
        *text >= 'a' && *text <= 'f' ? unsigned(*text - 'a' + 10) :
        *text >= 'A' && *text <= 'F' ? unsigned(*text - 'A' + 10) : 255;
    if (digit >= base || digit > maximum || out > (maximum - digit) / base) return false;
    out = out * base + digit;
  }
  return true;
}
esp_err_t unavailable(httpd_req_t *request, const char *message) {
  httpd_resp_set_status(request, "503 Service Unavailable");
  httpd_resp_set_type(request, "text/plain; charset=utf-8");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(request, message, HTTPD_RESP_USE_STRLEN);
}
}

void initialize(bool nvsReady) {
  if (mutex) return;
  mutex = xSemaphoreCreateMutexStatic(&mutexStorage);
  if (!mutex || !nvsReady) return;
  void *memory = heap_caps_malloc(sizeof(daily::Store), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  blob = static_cast<uint8_t *>(heap_caps_malloc(daily::kAgentBlobBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!memory || !blob) { heap_caps_free(memory); heap_caps_free(blob); blob = nullptr; return; }
  store = new(memory) daily::Store;
  nvs_handle_t handle;
  const esp_err_t opened = nvs_open("daily_tokens", NVS_READONLY, &handle);
  if (opened == ESP_ERR_NVS_NOT_FOUND) { available = true; return; }
  if (opened != ESP_OK) { saveFailed = true; return; }
  bool valid = true;
  for (size_t i = 0; i < daily::kAgents; ++i) {
    char key[8]; std::snprintf(key, sizeof(key), "d%02u", unsigned(i));
    size_t bytes = daily::kAgentBlobBytes;
    const esp_err_t result = nvs_get_blob(handle, key, blob, &bytes);
    if (result == ESP_ERR_NVS_NOT_FOUND) continue;
    uint64_t identity = 0;
    if (result == ESP_OK && bytes == daily::kAgentBlobBytes)
      for (unsigned j = 0; j < 8; ++j) identity |= uint64_t(blob[8 + j]) << (j * 8);
    if (result != ESP_OK || bytes != daily::kAgentBlobBytes || !identity || identity > UINT32_MAX ||
        !store->decodeAgent(i, identity, blob, bytes)) { valid = false; break; }
    // Persisted core blobs also support uint64 totals; this JSON runtime accepts
    // only exact JavaScript integers, including aggregates across agents.
    for (size_t day = 0; day < daily::kDays; ++day) {
      const size_t offset = 24 + day * 18;
      uint32_t when = 0;
      for (unsigned j = 0; j < 4; ++j) when |= uint32_t(blob[offset + j]) << (j * 8);
      if (!when) continue;
      daily::Day aggregate;
      if (!store->readDay(daily::dateFromOrdinal(when), aggregate) || aggregate.tokens > 9007199254740991ULL) {
        valid = false; break;
      }
    }
    if (!valid) break;
    identities[i] = identity; bound[i] = true;
  }
  nvs_close(handle);
  available = valid; saveFailed = !valid;
}

bool accept(const char *packet, uint32_t today, uint32_t deviceId, char (&ack)[64]) {
  ack[0] = 0;
  if (!packet) return false;
  size_t length = 0;
  while (length < 200 && packet[length]) ++length;
  if (!length || length >= 200) return false;
  char text[200]; std::memcpy(text, packet, length + 1);
  char *fields[8] = {text}; unsigned count = 1;
  for (char *p = text; *p; ++p) if (*p == ',') {
    if (count == 8) return false;
    *p = 0; fields[count++] = p + 1;
  }
  uint64_t account, client, date, tokens, revision, rgb;
  constexpr uint64_t maximum = 9007199254740991ULL;
  if (count != 8 || std::strcmp(fields[0], "LFD1") || std::strlen(fields[1]) != 8 ||
      std::strlen(fields[2]) != 8 || std::strlen(fields[3]) != 8 ||
      !number(fields[1], 16, UINT32_MAX, account) || account != PROJECT_ACCOUNT_FINGERPRINT ||
      !number(fields[2], 16, UINT32_MAX, client) || !client ||
      !number(fields[3], 10, 99991231, date) || !number(fields[4], 10, maximum, tokens) ||
      !number(fields[5], 10, daily::kMaxRevision, revision) || !revision ||
      std::strlen(fields[6]) != 6 || !number(fields[6], 16, 0xffffff, rgb)) return false;
  const size_t nameLength = std::strlen(fields[7]);
  if (nameLength > 96 || (nameLength & 1)) return false;
  char name[49] = {};
  for (size_t i = 0; i < nameLength / 2; ++i) {
    char hex[3] = {fields[7][2 * i], fields[7][2 * i + 1], 0}; uint64_t byte;
    if (!number(hex, 16, 255, byte) || !byte) return false;
    name[i] = char(byte);
  }
  for (size_t i = 0; i < nameLength / 2;) {
    uint32_t point = uint8_t(name[i++]); unsigned extra = 0; uint32_t minimum = 0;
    if (point < 0x80) { if (point < 0x20 || point == 0x7f) return false; continue; }
    if ((point & 0xe0) == 0xc0) { point &= 31; extra = 1; minimum = 0x80; }
    else if ((point & 0xf0) == 0xe0) { point &= 15; extra = 2; minimum = 0x800; }
    else if ((point & 0xf8) == 0xf0) { point &= 7; extra = 3; minimum = 0x10000; }
    else return false;
    if (i + extra > nameLength / 2) return false;
    while (extra--) { const uint8_t next = uint8_t(name[i++]); if ((next & 0xc0) != 0x80) return false; point = (point << 6) | (next & 63); }
    if (point < minimum || point > 0x10ffff || (point >= 0xd800 && point <= 0xdfff)) return false;
  }
  const uint32_t when = daily::ordinal(uint32_t(date)), current = daily::ordinal(today);
  if (!when || !current || when > current || current - when >= daily::kDays) return false;
  Lock lock;
  if (!lock.held || !available || saving) return false;
  size_t slot = 0;
  daily::Day aggregate;
  daily::Record previous;
  if (!store->readDay(uint32_t(date), aggregate)) return false;
  const bool existing = store->findAgent(client, slot);
  const uint64_t old = existing && store->readAgent(uint32_t(date), slot, previous) ? previous.tokens : 0;
  if (aggregate.tokens < old || aggregate.tokens - old > maximum - tokens) return false;
  if (!store->bindAgent(client, slot)) return false;
  identities[slot] = client; bound[slot] = true;
  const auto result = store->merge(uint32_t(date), slot, tokens, revision, today);
  if (result != daily::Result::Applied && result != daily::Result::Unchanged) return false;
  // Volatile acceptance only. Agents must cyclically replay unchanged snapshots;
  // flash persistence remains batched at five minutes / requested restart / OTA.
  std::snprintf(ack, sizeof(ack), "LFD1_RAM_ACK,%06lX,%08lX,%08lu,%llu", (unsigned long)deviceId,
      (unsigned long)client, (unsigned long)date, (unsigned long long)revision);
  return true;
}

bool dirty() { Lock lock; return !lock.held || anyDirty(); }
bool due(uint32_t now, bool flush) {
  Lock lock; return lock.held && anyDirty() && (flush || uint32_t(now - savedAt) >= 300000);
}
bool persist() {
  // Network owner only, during Persist maintenance. No owner merges during I/O.
  { Lock lock; if (!lock.held) return false; if (!anyDirty()) return true; saving = true; }
  nvs_handle_t handle;
  bool okay = nvs_open("daily_tokens", NVS_READWRITE, &handle) == ESP_OK;
  if (okay) {
    for (size_t i = 0; i < daily::kAgents; ++i) {
      bool pending = false;
      { Lock lock;
        if (!lock.held) { okay = false; break; }
        pending = bound[i] && store->dirty(i);
        if (pending && !store->encodeAgent(i, identities[i], blob, daily::kAgentBlobBytes)) { okay = false; break; }
      }
      if (!pending) continue;
      char key[8]; std::snprintf(key, sizeof(key), "d%02u", unsigned(i));
      if (nvs_set_blob(handle, key, blob, daily::kAgentBlobBytes) != ESP_OK || nvs_commit(handle) != ESP_OK) { okay = false; break; }
      Lock lock;
      if (!lock.held) { okay = false; break; }
      store->markSaved(i);
    }
    nvs_close(handle);
  }
  // Must publish completion even if a reader briefly holds the mutex.
  if (xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE) {
    saving = false; saveFailed = !okay;
    if (okay) savedAt = uint32_t(esp_timer_get_time() / 1000);
    xSemaphoreGive(mutex);
  }
  return okay;
}

esp_err_t get(httpd_req_t *request, uint32_t today) {
  // One HTTP task. Allocate only the bounded aggregate DTO, never Store/View.
  static_assert(sizeof(daily::Day) * daily::kDays <= 9000, "Aggregate snapshot memory cap");
  daily::Day *days = static_cast<daily::Day *>(heap_caps_calloc(daily::kDays, sizeof(daily::Day), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!days) return unavailable(request, "History memory unavailable");
  const uint32_t current = daily::ordinal(today);
  for (size_t i = 0; i < daily::kDays && i < current; ++i)
    days[i].date = daily::dateFromOrdinal(current - uint32_t(i));
  bool ready, pending, failed;
  { Lock lock;
    if (!lock.held) { heap_caps_free(days); return unavailable(request, "History busy"); }
    ready = available; pending = saving || anyDirty(); failed = saveFailed;
    // At most 366 direct-slot reads, each scanning 12 agents. Date conversion
    // and JSON construction happen outside this mutex; no I/O while held.
    if (ready && current) for (size_t i = 0; i < daily::kDays && i < current; ++i)
      store->readDay(days[i].date, days[i]);
  }
  constexpr size_t capacity = 20480;
  char *out = static_cast<char *>(heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!out) { heap_caps_free(days); return unavailable(request, "History memory unavailable"); }
  int size = std::snprintf(out, capacity, "{\"today\":%lu,\"available\":%s,\"saving\":%s,\"save_failed\":%s,\"days\":[",
      (unsigned long)today, ready ? "true" : "false", pending ? "true" : "false", failed ? "true" : "false");
  size_t used = size > 0 ? size_t(size) : capacity;
  bool okay = used < capacity;
  bool first = true;
  for (size_t i = daily::kDays; i > 0 && okay; --i) if (days[i - 1].agents) {
    size = std::snprintf(out + used, capacity - used, "%s{\"date\":%lu,\"tokens\":%llu}", first ? "" : ",",
        (unsigned long)days[i - 1].date, (unsigned long long)days[i - 1].tokens);
    okay = size >= 0 && size_t(size) < capacity - used;
    if (okay) used += size_t(size);
    first = false;
  }
  heap_caps_free(days);
  if (!okay || capacity - used < 3) { heap_caps_free(out); return unavailable(request, "History response too large"); }
  std::memcpy(out + used, "]}", 3); used += 2;
  httpd_resp_set_type(request, "application/json; charset=utf-8");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  const esp_err_t result = httpd_resp_send(request, out, int(used));
  heap_caps_free(out);
  return result;
}
}
