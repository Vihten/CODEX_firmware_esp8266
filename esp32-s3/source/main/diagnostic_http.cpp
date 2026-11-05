#include "diagnostic_http.hpp"
#include "diagnostics.hpp"
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
  char line[1024];
  const int header = std::snprintf(line, sizeof(line),
      "Codex diagnostic journal\nfirmware=%llu boot=%lu reset_reason=%lu entries=%u capacity=%u\n"
      "Warnings/errors only. RAM journal: lost on reboot/power loss. No credentials or raw requests.\n"
      "overwritten=%llu coalesced=%llu dropped=%lu\n",
      (unsigned long long)metadata.firmware_version, (unsigned long)metadata.boot_id,
      (unsigned long)metadata.reset_cause, unsigned(metadata.count), unsigned(metadata.capacity),
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
      char time[32]; timestamp(entry.unix_epoch_s, time);
      const auto &s = entry.snapshot;
      const int count = std::snprintf(line, sizeof(line),
          "\n#%llu %s uptime_ms=%lu last_ms=%lu repeats=%lu %s %s code=%u\n"
          "%s\nreason=%ld detail=%ld retries=%lu elapsed_ms=%lu heap=%lu heap_min=%lu largest=%lu "
          "stage=%s/%lu stage_age_ms=%lu frames=%lu wifi=%u rssi=%d agents=%u context_uptime_ms=%lu view_valid=%u\n",
          (unsigned long long)entry.sequence, time, (unsigned long)entry.uptime_ms,
          (unsigned long)entry.last_uptime_ms, (unsigned long)entry.repeat_count,
          entry.severity == diagnostics::Level::Error ? "ERROR" : "WARNING", moduleName(entry.module), unsigned(entry.code),
          entry.text, (long)s.reason, (long)s.detail, (unsigned long)s.attempts, (unsigned long)s.elapsed_ms,
          (unsigned long)s.heap_free, (unsigned long)s.heap_min, (unsigned long)s.largest_block,
          network_progress::name(static_cast<network_progress::Stage>(s.network_stage)),
          (unsigned long)s.network_stage, (unsigned long)s.network_age_ms, (unsigned long)s.render_frames,
          unsigned(s.wifi_connected), int(s.wifi_rssi), unsigned(s.agents),
          (unsigned long)s.context_uptime_ms, unsigned(s.view_valid));
      if (count < 0 || count >= int(sizeof(line))) return ESP_FAIL;
      if ((result = httpd_resp_send_chunk(request, line, count)) != ESP_OK) return result;
    }
    if (page.done) break;
  }
  return result == ESP_OK ? httpd_resp_send_chunk(request, nullptr, 0) : result;
}
