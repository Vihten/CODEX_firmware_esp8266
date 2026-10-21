#include "air_alert.hpp"
#include "telegram_alert.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <initializer_list>
#include <cstring>
#include <ctime>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
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
constexpr size_t kBodyLimit = 256 * 1024;
constexpr size_t kWireLimit = 320 * 1024;
constexpr int64_t kDeadlineUs = 8000000;
constexpr uint32_t kMaximumRetryMs = 86400000;  // Fits main's signed wrap-safe due comparison.
constexpr char kHost[] = "t.me";

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
  char *body = static_cast<char *>(heap_caps_malloc(kBodyLimit + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  ~Response() { heap_caps_free(body); }
  Response() = default;
  Response(const Response &) = delete;
  Response &operator=(const Response &) = delete;
  size_t used = 0, fieldSize = 0, valueSize = 0;
  char field[64] = {}, value[256] = {};
  bool inValue = false, complete = false, bad = false;
  uint32_t retry = 60000;
  unsigned status = 0;

  void header() {
    if (!fieldSize) return;
    while (valueSize && whitespace(value[valueSize - 1])) value[--valueSize] = 0;
    if (sameHeader(field, "Retry-After")) {
      const uint32_t parsed = retryAfterMs(value);
      if (parsed > retry) retry = parsed;
    }
    if (sameHeader(field, "Age")) {
      const char *p = value;
      while (whitespace(*p)) ++p;
      unsigned seconds = 0;
      if (*p < '0' || *p > '9') bad = true;
      while (*p >= '0' && *p <= '9') {
        seconds = seconds > 120 ? 121 : seconds * 10 + unsigned(*p - '0');
        ++p;
      }
      while (whitespace(*p)) ++p;
      if (*p || seconds > 120) bad = true;
    }
    if (sameHeader(field, "Warning")) {
      // Split warning-values without treating commas inside quoted warning text as separators.
      bool quoted = false, escaped = false, start = true;
      for (const char *p = value; *p; ++p) {
        if (escaped) { escaped = false; continue; }
        if (quoted && *p == '\\') { escaped = true; continue; }
        if (*p == '"') { quoted = !quoted; continue; }
        if (!quoted && *p == ',') { start = true; continue; }
        if (start && !whitespace(*p)) {
          if (std::strlen(p) >= 4 && p[0] == '1' && p[1] == '1' &&
              (p[2] == '0' || p[2] == '1') && whitespace(p[3])) bad = true;
          start = false;
        }
      }
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

PollResult fetchPage(const PollJob &job, uint32_t before, telegram::Page &page, int64_t deadline) {
  PollResult result;
  result.generation = job.generation;
  const int64_t started = esp_timer_get_time();
  const auto finish = [&]() {
    result.elapsedMs = uint32_t((esp_timer_get_time() - started) / 1000);
    return result;
  };
  Response r;
  if (!r.body) { result.failure = Failure::Memory; return finish(); }
  char address[48] = {};
  if (!resolve(address, sizeof(address), deadline)) {
    result.failure = esp_timer_get_time() >= deadline ? Failure::Timeout : Failure::Dns;
    return finish();
  }
  esp_tls_t *tls = esp_tls_init();
  if (!tls) { result.failure = Failure::Memory; return finish(); }
  const auto captureTlsError = [&]() {
    esp_tls_error_handle_t handle = nullptr;
    if (esp_tls_get_error_handle(tls, &handle) == ESP_OK)
      result.tlsError = esp_tls_get_and_clear_last_error(handle, &result.tlsCode, &result.tlsFlags);
  };
  esp_tls_cfg_t config{};
  config.non_block = true;
  config.timeout_ms = 8000;
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
    result.failure = esp_timer_get_time() >= deadline ? Failure::Timeout : Failure::Tls;
    captureTlsError();
    esp_tls_conn_destroy(tls);
    return finish();
  }
  char path[64] = "/s/vmroficial";
  if (before) std::snprintf(path, sizeof(path), "/s/vmroficial?before=%lu", (unsigned long)before);
  char request[320];
  const int requestLength = std::snprintf(request, sizeof(request),
      "GET %s HTTP/1.1\r\nHost: t.me\r\nAccept: text/html\r\nAccept-Encoding: identity\r\n"
      "User-Agent: Codex-Matrix\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n", path);
  const size_t requestSize = size_t(requestLength);
  size_t sent = 0;
  while (sent < size_t(requestSize) && esp_timer_get_time() < deadline) {
    const int count = int(esp_tls_conn_write(tls, request + sent, requestSize - sent));
    if (count > 0) sent += size_t(count);
    else if (!wantsIo(count)) break;
    vTaskDelay(1);
  }
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
      if (wireBytes > kWireLimit) { parsed = false; break; }
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
  // Release internal TLS buffers before the bounded HTML scan.
  captureTlsError();
  esp_tls_conn_destroy(tls);
  if (parsed && r.complete && !r.bad && r.status == 200 && esp_timer_get_time() < deadline)
    result.valid = telegram::parsePage(r.body, r.used, page);
  if (result.valid && esp_timer_get_time() >= deadline) {
    result.valid = false;
    result.level = Level::Unknown;
  }
  if (result.valid) result.retryAfterMs = 15000;
  else result.failure = esp_timer_get_time() >= deadline ? Failure::Timeout :
      sent != requestSize ? Failure::Write : r.status && r.status != 200 ? Failure::Http :
      !parsed || !r.complete ? Failure::Read : Failure::Payload;
  return finish();
}

PollResult poll(const PollJob &job) {
  const int64_t started = esp_timer_get_time(), deadline = started + kDeadlineUs;
  PollResult result;
  result.generation = job.generation;
  uint32_t before = 0, head = 0;
  int64_t headTime = 0;
  for (unsigned attempt = 0; attempt < 3 && esp_timer_get_time() < deadline; ++attempt) {
    telegram::Page page;
    result = fetchPage(job, before, page, deadline);
    result.valid = result.valid && page.posts && page.newestId && page.oldestId && page.newestTime >= 1577836800 &&
        page.newestTime <= int64_t(std::time(nullptr)) + 120;
    if (!result.valid) break;
    if (!attempt) {
      head = page.newestId;
      headTime = page.newestTime;
      if (head < job.knownHeadId) { result.valid = false; break; }
    } else if (page.newestId >= before || page.newestTime > headTime) { result.valid = false; break; }
    result.headId = head;
    if (page.eventId) {
      result.messageId = page.eventId; result.messageTime = page.eventTime; result.level = page.level;
      result.valid = (result.level == Level::Clear || active(result.level)) &&
          page.eventTime <= headTime &&
          page.eventId >= job.knownEventId && page.eventTime >= job.knownEventTime &&
          (page.eventId != job.knownEventId ||
           (page.level == job.knownLevel && page.eventTime == job.knownEventTime));
      break;
    }
    if (job.knownEventId && page.oldestId <= job.knownHeadId) {
      result.messageId = job.knownEventId; result.messageTime = job.knownEventTime; result.level = job.knownLevel;
      result.valid = result.level == Level::Clear || active(result.level);
      break;
    }
    before = page.oldestId;
    result.valid = false;
  }
  if (!result.valid || esp_timer_get_time() >= deadline) {
    result.valid = false; result.level = Level::Unknown;
    if (result.failure == Failure::None) result.failure = esp_timer_get_time() >= deadline ? Failure::Timeout : Failure::Payload;
    if (result.retryAfterMs < 30000) result.retryAfterMs = 30000;
  } else { result.failure = Failure::None; result.retryAfterMs = 15000; }
  result.elapsedMs = uint32_t((esp_timer_get_time() - started) / 1000);
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

namespace {
// Validate grammar and depth before cJSON's recursive parser sees untrusted input.
class JsonPreflight {
 public:
  JsonPreflight(const char *data, size_t size) : cursor(data), end(data + size) {}
  bool valid() { skip(); return value(0) && (skip(), cursor == end); }
 private:
  const char *cursor, *end;
  void skip() { while (cursor != end && whitespace(*cursor)) ++cursor; }
  bool take(char c) {
    if (cursor == end || *cursor != c) return false;
    ++cursor; return true;
  }
  static bool digit(char c) { return c >= '0' && c <= '9'; }
  bool hex(unsigned &code) {
    code = 0;
    for (unsigned i = 0; i < 4; ++i) {
      if (cursor == end) return false;
      const char c = *cursor++;
      const int n = c >= '0' && c <= '9' ? c - '0' :
          c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
      if (n < 0) return false;
      code = code * 16 + unsigned(n);
    }
    return true;
  }
  bool string() {
    if (!take('"')) return false;
    while (cursor != end) {
      const auto c = static_cast<unsigned char>(*cursor++);
      if (c == '"') return true;
      if (c < 32) return false;
      if (c == '\\') {
        if (cursor == end) return false;
        const char escape = *cursor++;
        if (escape == 'u') {
          unsigned code = 0;
          if (!hex(code) || code == 0 || (code >= 0xdc00 && code <= 0xdfff)) return false;
          if (code >= 0xd800 && code <= 0xdbff) {
            unsigned low = 0;
            if (!take('\\') || !take('u') || !hex(low) || low < 0xdc00 || low > 0xdfff) return false;
          }
        } else if (!std::strchr("\"\\/bfnrt", escape)) return false;
      } else if (c >= 128) {
        unsigned count = 0, code = 0, minimum = 0;
        if (c >= 0xc2 && c <= 0xdf) { count = 1; code = c & 31; minimum = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { count = 2; code = c & 15; minimum = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { count = 3; code = c & 7; minimum = 0x10000; }
        else return false;
        while (count--) {
          if (cursor == end) return false;
          const auto next = static_cast<unsigned char>(*cursor++);
          if ((next & 0xc0) != 0x80) return false;
          code = (code << 6) | (next & 63);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
      }
    }
    return false;
  }
  bool value(unsigned depth) {
    skip();
    if (cursor == end) return false;
    if (*cursor == '{' || *cursor == '[') {
      if (depth == 16) return false;
      const bool object = *cursor++ == '{';
      const char close = object ? '}' : ']';
      skip();
      if (take(close)) return true;
      do {
        skip();
        if (object) { if (!string()) return false; skip(); if (!take(':')) return false; }
        if (!value(depth + 1)) return false;
        skip();
        if (take(close)) return true;
      } while (take(','));
      return false;
    }
    if (*cursor == '"') return string();
    for (const char *literal : {"true", "false", "null"}) {
      const size_t size = std::strlen(literal);
      if (size_t(end - cursor) >= size && !std::memcmp(cursor, literal, size)) {
        cursor += size; return true;
      }
    }
    take('-');
    if (cursor == end || !digit(*cursor)) return false;
    if (!take('0')) while (cursor != end && digit(*cursor)) ++cursor;
    if (take('.')) {
      if (cursor == end || !digit(*cursor)) return false;
      while (cursor != end && digit(*cursor)) ++cursor;
    }
    if (take('e') || take('E')) {
      if (!take('+')) take('-');
      if (cursor == end || !digit(*cursor)) return false;
      while (cursor != end && digit(*cursor)) ++cursor;
    }
    return true;
  }
};

bool uniqueKeys(const cJSON *object, const char *const *keys, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    bool seen = false;
    for (const cJSON *item = object->child; item; item = item->next) {
      if (item->string && !std::strcmp(item->string, keys[i])) {
        if (seen) return false;
        seen = true;
      }
    }
  }
  return true;
}
const cJSON *field(const cJSON *object, const char *key) {
  return cJSON_GetObjectItemCaseSensitive(object, key);
}
bool text(const cJSON *item) { return cJSON_IsString(item) && item->valuestring && *item->valuestring; }
bool equals(const cJSON *item, const char *value) {
  return cJSON_IsString(item) && item->valuestring && !std::strcmp(item->valuestring, value);
}
bool timestamp(const cJSON *item) {
  if (!text(item)) return false;
  const char *s = item->valuestring;
  const size_t size = std::strlen(s);
  if (size < 20 || s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' ||
      s[16] != ':' || s[size - 1] != 'Z') return false;
  for (size_t i = 0; i < 19; ++i) {
    if (i == 4 || i == 7 || i == 10 || i == 13 || i == 16) continue;
    if (s[i] < '0' || s[i] > '9') return false;
  }
  if (size != 20) {
    if (s[19] != '.' || size < 22 || size > 30) return false;
    for (size_t i = 20; i + 1 < size; ++i) if (s[i] < '0' || s[i] > '9') return false;
  }
  const auto number = [s](size_t i) { return unsigned((s[i] - '0') * 10 + s[i + 1] - '0'); };
  const unsigned year = number(0) * 100 + number(2), month = number(5), day = number(8);
  constexpr unsigned days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
  return year > 0 && month >= 1 && month <= 12 && day >= 1 &&
      day <= days[month - 1] + (month == 2 && leap ? 1u : 0u) &&
      number(11) <= 23 && number(14) <= 59 && number(17) <= 59;
}
bool records(const cJSON *array, bool district, Level &strongest) {
  constexpr const char *keys[] = {"key", "name", "oblast", "level", "reasons", "since"};
  constexpr char districtKey[] = "вінницький", districtName[] = "Вінницький район";
  constexpr char oblastKey[] = "вінницька", oblastName[] = "Вінницька область";
  for (const cJSON *entry = array->child; entry; entry = entry->next) {
    if (!cJSON_IsObject(entry) || !uniqueKeys(entry, keys, sizeof(keys) / sizeof(*keys))) return false;
    const auto *key = field(entry, "key"), *name = field(entry, "name"), *oblast = field(entry, "oblast");
    const auto *level = field(entry, "level"), *reasons = field(entry, "reasons"), *since = field(entry, "since");
    if (!text(key) || !text(name) || !cJSON_IsString(oblast) ||
        (district && !text(oblast)) || (!equals(level, "red") && !equals(level, "yellow")) ||
        (since && !timestamp(since)) || (reasons && !cJSON_IsArray(reasons))) return false;
    // A changed key format must not silently convert a matching name into all-clear.
    const bool keyMatches = equals(key, district ? districtKey : oblastKey);
    const bool nameMatches = equals(name, district ? districtName : oblastName);
    if (keyMatches != nameMatches) return false;
    if (district && keyMatches && !equals(oblast, oblastName)) return false;
    if (!district && keyMatches && !equals(oblast, "") && !equals(oblast, oblastName)) return false;
    const bool matches = keyMatches && (!district || equals(oblast, oblastName));
    bool missile = false, drone = false;
    if (reasons) for (const cJSON *reason = reasons->child; reason; reason = reason->next) {
      if (!text(reason)) return false;
      // Explicit cause only: red/yellow are provider severity, not weapon types.
      missile |= equals(reason, "Ракетна загроза") ||
          equals(reason, "Ракетна загроза (червоний рівень)");
      drone |= equals(reason, "Дронова загроза") ||
          equals(reason, "Дронова загроза (жовтий рівень)");
    }
    if (matches) {
      const Level cause = missile ? Level::Red : drone ? Level::Yellow : Level::Unspecified;
      if (priority(cause) > priority(strongest)) strongest = cause;
    }
  }
  return true;
}
}  // namespace

bool parseBody(const char *body, size_t length, Level &level) {
  level = Level::Unknown;
  if (!body || !length || length > 64 * 1024 || std::memchr(body, 0, length) ||
      !JsonPreflight(body, length).valid()) return false;
  const char *end = nullptr;
  cJSON *root = cJSON_ParseWithLengthOpts(body, length, &end, false);
  if (!root) return false;
  constexpr const char *keys[] = {"version", "updatedAt", "raions", "oblasts"};
  const auto *version = field(root, "version"), *updated = field(root, "updatedAt");
  const auto *raions = field(root, "raions"), *oblasts = field(root, "oblasts");
  bool valid = cJSON_IsObject(root) && uniqueKeys(root, keys, sizeof(keys) / sizeof(*keys)) &&
      cJSON_IsNumber(version) && std::isfinite(version->valuedouble) && version->valuedouble > 0 &&
      version->valuedouble <= 9007199254740991.0 && std::floor(version->valuedouble) == version->valuedouble &&
      timestamp(updated) && cJSON_IsArray(raions) && cJSON_IsArray(oblasts);
  Level strongest = Level::Clear;
  if (valid) valid = records(raions, true, strongest) && records(oblasts, false, strongest);
  if (valid) level = strongest;
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
  if (!ready.load(std::memory_order_acquire)) return false;
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
