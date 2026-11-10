#include "diagnostic_http.hpp"
#include "diagnostics.hpp"
#include "diagnostic_persistence.hpp"
#include "network_progress.hpp"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace {
// Match IDF's plain HTTP sender; the public override API has no getter/reset.
int socketError() {
  switch (errno) {
    case EAGAIN: case EINTR: return HTTPD_SOCK_ERR_TIMEOUT;
    case EINVAL: case EBADF: case EFAULT: case ENOTSOCK: return HTTPD_SOCK_ERR_INVALID;
    default: return HTTPD_SOCK_ERR_FAIL;
  }
}
int plainSend(httpd_handle_t, int socket, const char *bytes, size_t size, int flags) {
  if (!bytes) return HTTPD_SOCK_ERR_INVALID;
  const int sent = send(socket, bytes, size, flags);
  return sent < 0 ? socketError() : sent;
}
struct Deadline { int64_t until; };
int boundedSend(httpd_handle_t handle, int socket, const char *bytes, size_t size, int flags) {
  const auto *deadline = static_cast<const Deadline *>(httpd_sess_get_transport_ctx(handle, socket));
  if (!deadline || !bytes) return HTTPD_SOCK_ERR_INVALID;
  for (;;) {
    if (esp_timer_get_time() >= deadline->until) return HTTPD_SOCK_ERR_TIMEOUT;
    // IDF retries partial sends. Every retry reuses this same absolute deadline,
    // including headers, chunk framing, trailers, and the final empty chunk.
    const int sent = send(socket, bytes, size, flags | MSG_DONTWAIT);
    if (sent >= 0) return sent ? sent : HTTPD_SOCK_ERR_FAIL;
    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return socketError();
    // Yield one scheduler tick, never a blocking socket send. Time is checked
    // again before every retry; socket progress cannot extend the deadline.
    vTaskDelay(1);
  }
}
void keepStackContext(void *) {}
class ExportTransport {
  httpd_handle_t handle_;
  int socket_;
  Deadline deadline_;
  bool active_ = false;
public:
  ExportTransport(httpd_req_t *request, int64_t until)
      : handle_(request->handle), socket_(httpd_req_to_sockfd(request)), deadline_{until} {
    // Never replace TLS/custom transport state or touch the authentication ctx.
    if (socket_ < 0 || httpd_sess_get_transport_ctx(handle_, socket_)) return;
    httpd_sess_set_transport_ctx(handle_, socket_, &deadline_, keepStackContext);
    if (httpd_sess_get_transport_ctx(handle_, socket_) != &deadline_) return;
    if (httpd_sess_set_send_override(handle_, socket_, boundedSend) != ESP_OK) {
      httpd_sess_set_transport_ctx(handle_, socket_, nullptr, nullptr);
      return;
    }
    active_ = true;
  }
  ~ExportTransport() {
    if (!active_) return;
    (void)httpd_sess_set_send_override(handle_, socket_, plainSend);
    httpd_sess_set_transport_ctx(handle_, socket_, nullptr, nullptr);
  }
  ExportTransport(const ExportTransport &) = delete;
  ExportTransport &operator=(const ExportTransport &) = delete;
  bool ready() const { return active_; }
};
const char *moduleName(diagnostics::Module value) {
  switch (value) {
    case diagnostics::Module::Wifi: return "wifi";
    case diagnostics::Module::Http: return "http/auth";
    case diagnostics::Module::Ota: return "ota";
    case diagnostics::Module::Alert: return "air-alert";
    case diagnostics::Module::Output: return "led-output";
    default: return "system";
  }
}
void timestamp(uint32_t epoch, char (&text)[32]) {
  if (!epoch) { std::snprintf(text, sizeof(text), "clock-not-synchronized"); return; }
  const time_t raw = epoch;
  tm utc{};
  gmtime_r(&raw, &utc);
  std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc);
}
}

esp_err_t diagnosticJournal(httpd_req_t *request) {
  const int64_t started = esp_timer_get_time();
  ExportTransport transport(request, started + 3000000);
  if (!transport.ready()) return ESP_FAIL;
  diagnostics::Metadata metadata;
  diagnostics::Cursor cursor;
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
  httpd_resp_set_type(request, "text/plain; charset=utf-8");
  if (!diagnostics::getMetadata(metadata) || !metadata.available || !diagnostics::beginRead(cursor)) {
    httpd_resp_set_status(request, "503 Service Unavailable");
    return httpd_resp_send(request, "Journal unavailable; retry.", HTTPD_RESP_USE_STRLEN);
  }
  if (!std::strcmp(request->uri, "/logs.txt"))
    httpd_resp_set_hdr(request, "Content-Disposition", "attachment; filename=\"codex-diagnostics.txt\"");
  diagnostic_persistence::Metadata archive;
  const bool archiveOkay = diagnostic_persistence::metadata(archive);
  char line[1024];
  const int header = std::snprintf(line, sizeof(line),
      "Codex diagnostic journal\nfirmware=%llu boot=%lu reset_reason=%lu entries=%u capacity=%u\n"
      "Warnings/errors and archived boot markers. RAM journal: lost on reboot/power loss. No credentials or raw requests.\n"
      "persistence_available=%u pending=%u failed=%u cached=%u newest=%llu last_saved_ms=%lu\n"
      "LittleFS archive: 4 x 64 KiB; latest 256 unique cached records plus current RAM (256).\n"
      "Saved archive survives power loss; unflushed records may be lost. Batches ~30s when maintenance is available; no fixed guarantee.\n"
      "%s"
      "overwritten=%llu coalesced=%llu dropped=%lu\n",
      (unsigned long long)metadata.firmware_version, (unsigned long)metadata.boot_id,
      (unsigned long)metadata.reset_cause, unsigned(metadata.count), unsigned(metadata.capacity),
      unsigned(archiveOkay && archive.available), unsigned(archive.pending), unsigned(archive.failed),
      unsigned(archive.count), (unsigned long long)archive.newest, (unsigned long)archive.last_saved_ms,
      !archiveOkay || !archive.available ? "Archive unavailable; RAM fallback.\n" :
          archive.failed ? "Archive failure detected; history may be incomplete. RAM fallback available.\n" : "",
      (unsigned long long)metadata.counters.overwritten, (unsigned long long)metadata.counters.coalesced,
      (unsigned long)metadata.dropped);
  if (header < 0 || header >= int(sizeof(line))) return ESP_FAIL;
  esp_err_t result = httpd_resp_send_chunk(request, line, header);
  if (result != ESP_OK) return result;
  if (metadata.previous_boot_valid) {
    const auto &previous = metadata.previous_boot;
    const int count = std::snprintf(line, sizeof(line),
        "previous_boot_summary: firmware=%llu uptime_ms=%lu heap_min=%lu network_stage=%lu\n"
        "RTC summary only; not the previous journal. Not retained across power loss.\n",
        (unsigned long long)previous.version, (unsigned long)previous.uptime_ms,
        (unsigned long)previous.heap_min, (unsigned long)previous.network_stage);
    if (count < 0 || count >= int(sizeof(line))) return ESP_FAIL;
    if ((result = httpd_resp_send_chunk(request, line, count)) != ESP_OK) return result;
  }
  // Both adapters copy values before formatting or network I/O.
  const auto sendEntry = [&](const diagnostics::Entry& entry) -> esp_err_t {
    char time[32]; timestamp(entry.unix_epoch_s, time);
    char lastTime[32]; timestamp(entry.last_unix_epoch_s, lastTime);
    const auto &s = entry.snapshot;
    const int count = std::snprintf(line, sizeof(line),
        "\n#%llu %s uptime_ms=%lu last_ms=%lu repeats=%lu %s %s code=%u\n"
        "%s\nlast_time=%s reason=%ld detail=%ld retries=%lu elapsed_ms=%lu heap=%lu heap_min=%lu largest=%lu "
        "stage=%s/%lu stage_age_ms=%lu frames=%lu wifi=%u rssi=%d agents=%u context_uptime_ms=%lu view_valid=%u\n",
        (unsigned long long)entry.sequence, time, (unsigned long)entry.uptime_ms,
        (unsigned long)entry.last_uptime_ms, (unsigned long)entry.repeat_count,
        entry.severity == diagnostics::Level::Error ? "ERROR" :
            entry.severity == diagnostics::Level::Info ? "INFO" : "WARNING", moduleName(entry.module), unsigned(entry.code),
        entry.text, lastTime, (long)s.reason, (long)s.detail, (unsigned long)s.attempts, (unsigned long)s.elapsed_ms,
        (unsigned long)s.heap_free, (unsigned long)s.heap_min, (unsigned long)s.largest_block,
        network_progress::name(static_cast<network_progress::Stage>(s.network_stage)),
        (unsigned long)s.network_stage, (unsigned long)s.network_age_ms, (unsigned long)s.render_frames,
        unsigned(s.wifi_connected), int(s.wifi_rssi), unsigned(s.agents),
        (unsigned long)s.context_uptime_ms, unsigned(s.view_valid));
    if (count < 0 || count >= int(sizeof(line))) return ESP_FAIL;
    return httpd_resp_send_chunk(request, line, count);
  };
  uint64_t after = 0;
  bool archiveInterrupted = false;
  if (archiveOkay && archive.available) {
    for (unsigned read = 0; read < 256 && after < archive.newest; ++read) {
      if (esp_timer_get_time() - started >= 3000000) return ESP_FAIL;
      diagnostic_archive::Record record;
      if (!diagnostic_persistence::readAfter(after, archive.newest, record) ||
          record.serial <= after || record.serial > archive.newest) {
        archiveInterrupted = true;
        result = httpd_resp_send_chunk(request,
            "\nArchive export interrupted: unavailable/busy or history changed; retry.\n", HTTPD_RESP_USE_STRLEN);
        break;
      }
      after = record.serial;
      if (record.boot_id == metadata.boot_id &&
          record.entry.sequence >= cursor.next && record.entry.sequence <= cursor.until) continue;
      const int count = std::snprintf(line, sizeof(line),
          "\narchive serial=%llu firmware=%llu boot=%lu reset_reason=%lu\n",
          (unsigned long long)record.serial, (unsigned long long)record.firmware_version,
          (unsigned long)record.boot_id, (unsigned long)record.reset_cause);
      if (count < 0 || count >= int(sizeof(line))) return ESP_FAIL;
      if ((result = httpd_resp_send_chunk(request, line, count)) != ESP_OK) return result;
      if ((result = sendEntry(record.entry)) != ESP_OK) return result;
    }
    if (result != ESP_OK) return result;
    if (!archiveInterrupted && after < archive.newest) {
      result = httpd_resp_send_chunk(request,
          "\nArchive export interrupted: bounded history may be incomplete.\n", HTTPD_RESP_USE_STRLEN);
      if (result != ESP_OK) return result;
    }
  }
  if (!metadata.count && (result = httpd_resp_send_chunk(request,
      "\nNo warnings or errors recorded in this boot.\n", HTTPD_RESP_USE_STRLEN)) != ESP_OK) return result;
  // A frozen upper sequence bounds this export even when events arrive. Copy
  // one entry at a time: no ring mutex is held during formatting or socket I/O.
  for (unsigned read = 0; read < diagnostics::kCapacity; ++read) {
    // Downloads must not monopolize the single HTTP dispatcher for a slow peer.
    if (esp_timer_get_time() - started >= 3000000) return ESP_FAIL;
    diagnostics::Entry entry;
    diagnostics::Page page;
    if (!diagnostics::readAfter(cursor, entry, page)) {
      result = httpd_resp_send_chunk(request, "\nExport interrupted: journal busy; retry.\n", HTTPD_RESP_USE_STRLEN);
      break;
    }
    if (page.lost) {
      const int count = std::snprintf(line, sizeof(line), "\n%llu entries overwritten during export.\n", (unsigned long long)page.lost);
      if ((result = httpd_resp_send_chunk(request, line, count)) != ESP_OK) return result;
    }
    if (page.count) {
      if ((result = sendEntry(entry)) != ESP_OK) return result;
    }
    if (page.done) break;
  }
  return result == ESP_OK ? httpd_resp_send_chunk(request, nullptr, 0) : result;
}
