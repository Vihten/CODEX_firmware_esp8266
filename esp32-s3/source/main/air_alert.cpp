#include "air_alert.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "http_parser.h"
#include "lwip/dns.h"
#include "lwip/tcpip.h"

namespace air_alert {
namespace {
constexpr size_t kBodyLimit = 8192;
constexpr int64_t kDeadlineUs = 4000000;
constexpr uint32_t kMaximumRetryMs = 86400000;  // Fits main's signed wrap-safe due comparison.
constexpr char kHost[] = "api.alerts.in.ua";
constexpr char kRequestPrefix[] =
    "GET /v2/iot/active_air_raid_alerts.json HTTP/1.1\r\n"
    "Host: api.alerts.in.ua\r\nAccept: application/json\r\n"
    "Accept-Encoding: identity\r\nConnection: close\r\nAuthorization: Bearer ";

bool whitespace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

bool sameHeader(const char *a, const char *b) {
  while (*a && *b) {
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
    if (lower(*a++) != lower(*b++)) return false;
  }
  return !*a && !*b;
}

void erase(void *data, size_t length) {
  auto *bytes = static_cast<volatile unsigned char *>(data);
  while (length--) *bytes++ = 0;
}

bool tokenValid(const char (&token)[129]) {
  size_t length = 0;
  while (length < sizeof(token) && token[length]) {
    if (static_cast<unsigned char>(token[length]) < 33 ||
        static_cast<unsigned char>(token[length]) > 126) return false;
    ++length;
  }
  return length > 0 && length < sizeof(token);
}

StaticQueue_t jobsStorage, resultsStorage;
uint8_t jobsBytes[sizeof(PollJob)], resultsBytes[sizeof(PollResult)];
QueueHandle_t jobs = nullptr, results = nullptr;
std::atomic<bool> running{false}, ready{false};

// DNS callbacks may arrive after the poll deadline. Persistent storage keeps them safe.
struct DnsState {
  std::atomic<bool> pending{false}, done{false};
  ip_addr_t address{};
  bool success = false;
} dns;

void dnsFinished(const char *, const ip_addr_t *address, void *) {
  dns.success = address != nullptr;
  if (address) dns.address = *address;
  dns.done.store(true, std::memory_order_release);
}

void dnsStart(void *) {
  ip_addr_t address{};
  const err_t status = dns_gethostbyname(kHost, &address, dnsFinished, nullptr);
  if (status == ERR_OK) dnsFinished(nullptr, &address, nullptr);
  else if (status != ERR_INPROGRESS) dnsFinished(nullptr, nullptr, nullptr);
}

bool resolve(char *address, size_t capacity, int64_t deadline) {
  if (dns.pending.load()) {
    if (!dns.done.load(std::memory_order_acquire)) return false;
    dns.pending.store(false);
  }
  dns.done.store(false);
  dns.pending.store(true);
  if (tcpip_try_callback(dnsStart, nullptr) != ERR_OK) {
    dns.pending.store(false);
    return false;
  }
  while (!dns.done.load(std::memory_order_acquire) && esp_timer_get_time() < deadline)
    vTaskDelay(1);
  if (!dns.done.load(std::memory_order_acquire)) return false;
  const bool okay = dns.success && ipaddr_ntoa_r(&dns.address, address, int(capacity));
  dns.pending.store(false);
  return okay;
}

struct Response {
  char body[kBodyLimit + 1] = {};
  size_t used = 0, fieldSize = 0, valueSize = 0;
  char field[64] = {}, value[256] = {};
  bool inValue = false, complete = false, bad = false;
  uint32_t retry = 60000;
  unsigned status = 0;

  void header() {
    if (!fieldSize) return;
    if (sameHeader(field, "Retry-After")) {
      const uint32_t parsed = retryAfterMs(value);
      if (parsed > retry) retry = parsed;
    }
    if (sameHeader(field, "Content-Encoding") && !sameHeader(value, "identity")) bad = true;
    fieldSize = valueSize = 0;
    field[0] = value[0] = 0;
    inValue = false;
  }
};

Response &response(http_parser *parser) { return *static_cast<Response *>(parser->data); }

int headerField(http_parser *parser, const char *data, size_t length) {
  auto &r = response(parser);
  if (r.inValue) r.header();
  if (length > sizeof(r.field) - r.fieldSize - 1) return 1;
  std::memcpy(r.field + r.fieldSize, data, length);
  r.fieldSize += length;
  r.field[r.fieldSize] = 0;
  return 0;
}

int headerValue(http_parser *parser, const char *data, size_t length) {
  auto &r = response(parser);
  r.inValue = true;
  if (length > sizeof(r.value) - r.valueSize - 1) return 1;
  std::memcpy(r.value + r.valueSize, data, length);
  r.valueSize += length;
  r.value[r.valueSize] = 0;
  return 0;
}

int headersComplete(http_parser *parser) {
  auto &r = response(parser);
  r.header();
  r.status = parser->status_code;
  // Redirects and errors are terminal; never follow Location or consume their bodies.
  if (r.status != 200) { r.complete = true; return 1; }
  return r.bad || ((parser->flags & F_CONTENTLENGTH) && parser->content_length > kBodyLimit) ? -1 : 0;
}

int bodyData(http_parser *parser, const char *data, size_t length) {
  auto &r = response(parser);
  if (length > kBodyLimit - r.used) return 1;
  std::memcpy(r.body + r.used, data, length);
  r.used += length;
  return 0;
}

int messageComplete(http_parser *parser) {
  response(parser).complete = true;
  http_parser_pause(parser, 1);
  return 0;
}

bool wantsIo(int result) {
  return result == ESP_TLS_ERR_SSL_WANT_READ || result == ESP_TLS_ERR_SSL_WANT_WRITE;
}

PollResult poll(const PollJob &job) {
  PollResult result;
  result.generation = job.generation;
  const int64_t deadline = esp_timer_get_time() + kDeadlineUs;
  char address[48] = {};
  if (!resolve(address, sizeof(address), deadline)) return result;
  esp_tls_t *tls = esp_tls_init();
  if (!tls) return result;
  esp_tls_cfg_t config{};
  config.non_block = true;
  config.timeout_ms = 4000;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.common_name = kHost;  // Certificate verification and SNI remain the fixed DNS host.
  config.skip_common_name = false;
  int connected = 0;
  while (esp_timer_get_time() < deadline) {
    config.timeout_ms = int((deadline - esp_timer_get_time() + 999) / 1000);
    if (config.timeout_ms <= 0) break;
    connected = esp_tls_conn_new_async(address, int(std::strlen(address)), 443, &config, tls);
    if (connected != 0) break;
    vTaskDelay(1);
  }
  if (connected != 1 || esp_timer_get_time() >= deadline) {
    esp_tls_conn_destroy(tls);
    return result;
  }
  char request[sizeof(kRequestPrefix) + 129 + 4] = {};
  const int requestSize = std::snprintf(request, sizeof(request), "%s%s\r\n\r\n", kRequestPrefix, job.token);
  size_t sent = 0;
  while (sent < size_t(requestSize) && esp_timer_get_time() < deadline) {
    const int count = int(esp_tls_conn_write(tls, request + sent, size_t(requestSize) - sent));
    if (count > 0) sent += size_t(count);
    else if (!wantsIo(count)) break;
    vTaskDelay(1);
  }
  erase(request, sizeof(request));
  Response r;
  http_parser parser{};
  http_parser_init(&parser, HTTP_RESPONSE);
  parser.data = &r;
  http_parser_settings settings{};
  settings.on_header_field = headerField;
  settings.on_header_value = headerValue;
  settings.on_headers_complete = headersComplete;
  settings.on_body = bodyData;
  settings.on_message_complete = messageComplete;
  bool parsed = sent == size_t(requestSize);
  size_t wireBytes = 0;
  while (parsed && !r.complete && esp_timer_get_time() < deadline) {
    char chunk[512];
    const int count = int(esp_tls_conn_read(tls, chunk, sizeof(chunk)));
    if (count > 0) {
      wireBytes += size_t(count);
      // Includes framing and headers, so endless tiny chunks cannot consume unbounded work.
      if (wireBytes > 32768) { parsed = false; break; }
      const size_t consumed = http_parser_execute(&parser, &settings, chunk, size_t(count));
      parsed = consumed == size_t(count) || (r.complete && HTTP_PARSER_ERRNO(&parser) == HPE_PAUSED);
    } else if (count == 0) {
      http_parser_execute(&parser, &settings, nullptr, 0);
      parsed = HTTP_PARSER_ERRNO(&parser) == HPE_OK || HTTP_PARSER_ERRNO(&parser) == HPE_PAUSED;
      break;
    } else if (!wantsIo(count)) { parsed = false; break; }
    vTaskDelay(1);
  }
  result.httpStatus = int(r.status);
  if (r.status == 429) result.retryAfterMs = r.retry;
  if (parsed && r.complete && !r.bad && r.status == 200 && esp_timer_get_time() < deadline)
    result.valid = parseBody(r.body, r.used, result.level);
  esp_tls_conn_destroy(tls);
  return result;
}

void worker(void *) {
  for (;;) {
    PollJob job;
    if (xQueueReceive(jobs, &job, portMAX_DELAY) != pdTRUE) continue;
    const PollResult result = poll(job);
    erase(&job, sizeof(job));
    xQueueOverwrite(results, &result);
    running.store(false, std::memory_order_release);
  }
}
}  // namespace

bool parseBody(const char *body, size_t length, Level &level) {
  level = Level::Unknown;
  if (!body || !length || length > kBodyLimit || std::memchr(body, 0, length)) return false;
  size_t begin = 0;
  while (begin < length && whitespace(body[begin])) ++begin;
  if (begin == length || body[begin++] != '"') return false;
  bool closed = false;
  for (size_t i = begin; i < length; ++i) {
    if (static_cast<unsigned char>(body[i]) < 32) return false;
    if (body[i] == '\\') {
      if (++i == length) return false;
      if (body[i] == 'u' && i + 4 < length && !std::memcmp(body + i + 1, "0000", 4)) return false;
    } else if (body[i] == '"') {
      closed = true;
      for (++i; i < length; ++i) if (!whitespace(body[i])) return false;
      break;
    }
  }
  if (!closed) return false;
  const char *end = nullptr;
  cJSON *root = cJSON_ParseWithLengthOpts(body, length, &end, false);
  if (!root) return false;
  bool valid = cJSON_IsString(root) && root->valuestring && std::strlen(root->valuestring) > 155;
  if (valid) {
    const char *statuses = root->valuestring;
    const char scopes[] = {statuses[155], statuses[36], statuses[4]};
    bool unknown = false;
    Level strongest = Level::Unknown;
    for (char status : scopes) {
      if (status == 'R') strongest = Level::Red;
      else if (status == 'Y' && strongest != Level::Red) strongest = Level::Yellow;
      else if (status != 'N' && status != 'P' && status != 'Y') unknown = true;
    }
    if (active(strongest)) level = strongest;
    else if (!unknown && scopes[0] == 'N') level = Level::Clear;
    else valid = false;
  }
  cJSON_Delete(root);
  return valid;
}

uint32_t retryAfterMs(const char *value) {
  if (!value) return 60000;
  while (whitespace(*value)) ++value;
  if (*value >= '0' && *value <= '9') {
    uint64_t seconds = 0;
    while (*value >= '0' && *value <= '9') {
      seconds = seconds * 10 + unsigned(*value++ - '0');
      if (seconds > kMaximumRetryMs / 1000) return kMaximumRetryMs;
    }
    while (whitespace(*value)) ++value;
    if (*value) return 60000;
    return seconds < 60 ? 60000 : uint32_t(seconds * 1000);
  }
  char weekday[4]{}, month[4]{}, zone[4]{}, trailing = 0;
  int day = 0, year = 0, hour = 0, minute = 0, second = 0;
  if (std::sscanf(value, "%3s, %d %3s %d %d:%d:%d %3s %c", weekday, &day, month,
                  &year, &hour, &minute, &second, zone, &trailing) != 8 ||
      std::strcmp(zone, "GMT") || year < 1970 || year > 9999 || day < 1 ||
      hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59)
    return 60000;
  constexpr char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  int monthIndex = -1;
  for (int i = 0; i < 12; ++i)
    if (!std::memcmp(month, months + i * 3, 3)) monthIndex = i;
  if (monthIndex < 0) return 60000;
  const auto leap = [](int y) { return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0); };
  constexpr int daysInMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (day > daysInMonth[monthIndex] + (monthIndex == 1 && leap(year) ? 1 : 0)) return 60000;
  int64_t days = 0;
  for (int y = 1970; y < year; ++y) days += leap(y) ? 366 : 365;
  for (int m = 0; m < monthIndex; ++m) days += daysInMonth[m] + (m == 1 && leap(year) ? 1 : 0);
  days += day - 1;
  const int64_t target = days * 86400 + hour * 3600 + minute * 60 + second;
  const int64_t now = int64_t(std::time(nullptr));
  if (now < 1577836800) return kMaximumRetryMs;
  const int64_t seconds = target - now;
  if (seconds <= 60) return 60000;
  return seconds > kMaximumRetryMs / 1000 ? kMaximumRetryMs : uint32_t(seconds * 1000);
}

bool startPolling() {
  if (ready.load(std::memory_order_acquire)) return true;
  if (!jobs) jobs = xQueueCreateStatic(1, sizeof(PollJob), jobsBytes, &jobsStorage);
  if (!results) results = xQueueCreateStatic(1, sizeof(PollResult), resultsBytes, &resultsStorage);
  if (!jobs || !results) return false;
  const bool started = xTaskCreatePinnedToCore(worker, "air-alert", 16384, nullptr, 1, nullptr, 0) == pdPASS;
  ready.store(started, std::memory_order_release);
  return started;
}

bool submit(const PollJob &job) {
  if (!ready.load(std::memory_order_acquire) || !tokenValid(job.token)) return false;
  bool expected = false;
  if (!running.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return false;
  if (xQueueSend(jobs, &job, 0) == pdTRUE) return true;
  running.store(false, std::memory_order_release);
  return false;
}

bool receive(PollResult &result) {
  return ready.load(std::memory_order_acquire) && xQueueReceive(results, &result, 0) == pdTRUE;
}

bool busy() { return running.load(std::memory_order_acquire); }

}  // namespace air_alert
