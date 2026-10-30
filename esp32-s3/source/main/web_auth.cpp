#include "web_auth.hpp"
#include "web_files.hpp"
#include "password_sha256.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "psa/crypto.h"

namespace web_auth {
namespace {
constexpr uint32_t kMagic = 0x57415531, kIterations = 100000;
constexpr int64_t kSessionMs = 8LL * 60 * 60 * 1000;
constexpr char kMarker[] = "<!-- Legacy LAN discovery marker: <title>LED Forge Controller</title> -->";
struct Credential {
  uint32_t magic = kMagic, version = 1, iterations = kIterations;
  uint8_t salt[16] = {}, digest[32] = {};
  uint32_t checksum = 0;
};
static_assert(sizeof(Credential) == 64, "Stable NVS credential layout");
struct Session { uint8_t token[32] = {}; int64_t issued = 0; bool used = false; };
StaticSemaphore_t mutexStorage;
SemaphoreHandle_t mutex = nullptr;
Credential credential, staged;
Session sessions[4];
char bootToken[65] = {};
bool initialized = false, healthy = false, configured = false, pending = false;
bool saving = false, maintenance = false, saveFailed = false;
unsigned active = 0, failures = 0;
int64_t nextAttempt = 0;
uint32_t generation = 0;
std::atomic<uint32_t> revision{0};
std::atomic<uint32_t> lastCheckDuration{0}, lastHttpStackFree{0};
int64_t nowMs() { return esp_timer_get_time() / 1000; }
void erase(void *data, size_t size) {
  volatile uint8_t *bytes = static_cast<volatile uint8_t *>(data);
  while (size--) *bytes++ = 0;
}
struct Lock {
  bool held;
  Lock() : held(mutex && xSemaphoreTake(mutex, pdMS_TO_TICKS(50)) == pdTRUE) {}
  ~Lock() { if (held) xSemaphoreGive(mutex); }
};
bool equal(const void *a, const void *b, size_t size) {
  const auto *x = static_cast<const uint8_t *>(a), *y = static_cast<const uint8_t *>(b);
  uint8_t difference = 0;
  for (size_t i = 0; i < size; ++i) difference |= x[i] ^ y[i];
  return difference == 0;
}
uint32_t checksum(const Credential &value) {
  const auto *bytes = reinterpret_cast<const uint8_t *>(&value);
  uint32_t hash = 2166136261U;
  for (size_t i = 0; i < offsetof(Credential, checksum); ++i) hash = (hash ^ bytes[i]) * 16777619U;
  return hash;
}
void hex(const uint8_t *bytes, size_t size, char *out) {
  constexpr char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < size; ++i) { out[i * 2] = digits[bytes[i] >> 4]; out[i * 2 + 1] = digits[bytes[i] & 15]; }
  out[size * 2] = 0;
}
int digit(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }
bool tokenBytes(const char *text, uint8_t (&bytes)[32]) {
  if (!text || std::strlen(text) != 64) return false;
  for (unsigned i = 0; i < 32; ++i) {
    const int a = digit(text[i * 2]), b = digit(text[i * 2 + 1]);
    if (a < 0 || b < 0) return false;
    bytes[i] = uint8_t(a * 16 + b);
  }
  return true;
}
void invalidateSessions() { erase(sessions, sizeof(sessions)); revision.fetch_add(1); }
int findSession(const char *text) {
  uint8_t bytes[32];
  if (!tokenBytes(text, bytes)) return -1;
  int found = -1;
  const int64_t now = nowMs();
  for (unsigned i = 0; i < 4; ++i) {
    auto &entry = sessions[i];
    if (entry.used && (now < entry.issued || now - entry.issued >= kSessionMs)) {
      erase(&entry, sizeof(entry)); revision.fetch_add(1);
    }
    if (entry.used && equal(entry.token, bytes, sizeof(bytes))) found = int(i);
  }
  erase(bytes, sizeof(bytes));
  return found;
}
template<size_t N> bool header(httpd_req_t *request, const char *name, char (&out)[N]) {
  out[0] = 0;
  const size_t size = httpd_req_get_hdr_value_len(request, name);
  return size && size < N && httpd_req_get_hdr_value_str(request, name, out, N) == ESP_OK;
}
bool cookie(httpd_req_t *request, char (&out)[65]) {
  char text[1024]; out[0] = 0;
  if (!header(request, "Cookie", text)) return false;
  bool found = false;
  for (char *p = text; *p;) {
    while (*p == ' ' || *p == ';') ++p;
    char *end = std::strchr(p, ';');
    if (!end) end = p + std::strlen(p);
    if (size_t(end - p) >= 14 && !std::memcmp(p, "codex_session=", 14)) {
      if (found || end - p != 78) return false;
      std::memcpy(out, p + 14, 64); out[64] = 0; found = true;
    }
    p = *end ? end + 1 : end;
  }
  return found;
}
bool unsafe(httpd_req_t *request) { return request->method != HTTP_GET && request->method != HTTP_HEAD && request->method != HTTP_OPTIONS; }
bool sameOrigin(httpd_req_t *request) {
  char host[128], origin[160], referer[512], site[24], expected[144];
  if (!header(request, "Host", host)) return false;
  for (const char *p = host; *p; ++p)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
          (*p >= '0' && *p <= '9') || *p == '.' || *p == '-' || *p == ':' || *p == '[' || *p == ']')) return false;
  std::snprintf(expected, sizeof(expected), "http://%s", host);
  const size_t siteSize = httpd_req_get_hdr_value_len(request, "Sec-Fetch-Site");
  if (siteSize && (!header(request, "Sec-Fetch-Site", site) || std::strcmp(site, "same-origin"))) return false;
  const size_t originSize = httpd_req_get_hdr_value_len(request, "Origin");
  if (originSize) return header(request, "Origin", origin) && !std::strcmp(origin, expected);
  const size_t length = std::strlen(expected);
  return header(request, "Referer", referer) && !std::strncmp(referer, expected, length) && referer[length] == '/';
}
void responseHeaders(httpd_req_t *request) {
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
  httpd_resp_set_hdr(request, "Referrer-Policy", "same-origin");
}
esp_err_t reject(httpd_req_t *request, const char *status, const char *text) {
  responseHeaders(request); httpd_resp_set_status(request, status);
  httpd_resp_set_type(request, "text/plain; charset=utf-8");
  // Refusing a POST must not leave unread bytes on a reusable connection.
  httpd_resp_set_hdr(request, "Connection", "close");
  return httpd_resp_send(request, text, HTTPD_RESP_USE_STRLEN);
}
esp_err_t redirect(httpd_req_t *request, const char *path, bool marker = false) {
  responseHeaders(request); httpd_resp_set_status(request, "303 See Other");
  httpd_resp_set_hdr(request, "Location", path);
  httpd_resp_set_type(request, "text/html; charset=utf-8");
  return httpd_resp_send(request, marker ? kMarker : "", HTTPD_RESP_USE_STRLEN);
}
bool utf8(const char *text) {
  const auto *p = reinterpret_cast<const uint8_t *>(text);
  while (*p) {
    uint32_t value = *p++; unsigned count = 0; uint32_t minimum = 0;
    if (value < 128) continue;
    if (value >= 0xc2 && value <= 0xdf) { value &= 31; count = 1; minimum = 128; }
    else if (value >= 0xe0 && value <= 0xef) { value &= 15; count = 2; minimum = 2048; }
    else if (value >= 0xf0 && value <= 0xf4) { value &= 7; count = 3; minimum = 65536; }
    else return false;
    while (count--) { if ((*p & 0xc0) != 0x80) return false; value = value * 64 + (*p++ & 63); }
    if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
  }
  return true;
}
bool passwordValid(const char *text) {
  const size_t size = std::strlen(text);
  if (size > 128 || !utf8(text)) return false;
  size_t characters = 0;
  for (const auto *p = reinterpret_cast<const uint8_t *>(text); *p; ++p)
    if ((*p & 0xc0) != 0x80) ++characters;
  return characters >= 10;
}
bool decode(const char *text, size_t size, char *out, size_t capacity) {
  size_t used = 0;
  for (size_t i = 0; i < size; ++i) {
    unsigned char c = text[i];
    if (c == '%') {
      if (i + 2 >= size) return false;
      const auto unhex = [](char x) { return x >= 'A' && x <= 'F' ? x - 'A' + 10 : digit(x); };
      const int a = unhex(text[i + 1]), b = unhex(text[i + 2]);
      if (a < 0 || b < 0) return false;
      c = uint8_t(a * 16 + b); i += 2;
    } else if (c == '+') c = ' ';
    if (!c || used + 1 >= capacity) return false;
    out[used++] = char(c);
  }
  out[used] = 0; return true;
}
struct Form {
  char password[129] = {}, current[129] = {}, next[129] = {}, confirm[129] = {}, csrf[65] = {};
  unsigned fields = 0;
  ~Form() { erase(this, sizeof(*this)); }
};
bool formContentType(const char *type) {
  const auto space = [](char c) { return c == ' ' || c == '\t'; };
  const auto matches = [&](const char *first, const char *last, const char *expected) {
    while (first != last && space(*first)) ++first;
    while (first != last && space(last[-1])) --last;
    while (first != last && *expected) {
      const char c = *first++;
      if ((c >= 'A' && c <= 'Z' ? char(c + ('a' - 'A')) : c) != *expected++) return false;
    }
    return first == last && !*expected;
  };
  const char *end = type + std::strlen(type), *separator = std::strchr(type, ';');
  if (!matches(type, separator ? separator : end, "application/x-www-form-urlencoded")) return false;
  if (!separator) return true;
  const char *parameter = separator + 1, *equal = std::strchr(parameter, '=');
  // Only the optional UTF-8 charset is accepted; duplicate/ambiguous parameters fail.
  if (!equal || std::strchr(parameter, ';') || !matches(parameter, equal, "charset")) return false;
  const char *value = equal + 1;
  while (value != end && space(*value)) ++value;
  while (value != end && space(end[-1])) --end;
  if (value != end && *value == '"') {
    if (end - value < 2 || end[-1] != '"') return false;
    ++value; --end;
  }
  return matches(value, end, "utf-8");
}
bool readForm(httpd_req_t *request, Form &form) {
  char type[96];
  if (!header(request, "Content-Type", type) || !formContentType(type)) return false;
  struct Body { char bytes[2049] = {}; ~Body() { erase(bytes, sizeof(bytes)); } } body;
  if (!request->content_len || request->content_len >= sizeof(body.bytes)) return false;
  size_t used = 0; const int64_t started = nowMs();
  while (used < request->content_len) {
    if (nowMs() - started > 10000) return false;
    const int count = httpd_req_recv(request, body.bytes + used, request->content_len - used);
    if (count <= 0) return false;
    used += size_t(count);
  }
  if (std::memchr(body.bytes, 0, used)) return false;
  for (char *p = body.bytes; *p;) {
    char *end = std::strchr(p, '&'); if (!end) end = p + std::strlen(p);
    char *separator = static_cast<char *>(std::memchr(p, '=', size_t(end - p)));
    if (!separator) return false;
    char key[32]; if (!decode(p, size_t(separator - p), key, sizeof(key))) return false;
    char *target = nullptr; size_t capacity = 129; unsigned bit = 0;
    if (!std::strcmp(key, "password")) { target = form.password; bit = 1; }
    else if (!std::strcmp(key, "current_password")) { target = form.current; bit = 2; }
    else if (!std::strcmp(key, "new_password")) { target = form.next; bit = 4; }
    else if (!std::strcmp(key, "confirm_password")) { target = form.confirm; bit = 8; }
    else if (!std::strcmp(key, "csrf")) { target = form.csrf; capacity = 65; bit = 16; }
    else if (!std::strcmp(key, "__async")) {
      char value[2];
      if ((form.fields & 32) || !decode(separator + 1, size_t(end - separator - 1), value, sizeof(value)) || std::strcmp(value, "1")) return false;
      form.fields |= 32; p = *end ? end + 1 : end; continue;
    }
    else return false;
    if ((form.fields & bit) || !decode(separator + 1, size_t(end - separator - 1), target, capacity)) return false;
    form.fields |= bit; p = *end ? end + 1 : end;
  }
  return std::strlen(form.csrf) == 64 && equal(form.csrf, bootToken, 64);
}
struct PasswordHmac {
  password_sha256::Hash inner, outer, work;
  uint8_t pad[64] = {}, digest[32] = {};
  ~PasswordHmac() {
    erase(pad, sizeof(pad)); erase(digest, sizeof(digest));
  }
  bool setup(const char *password) {
    const size_t size = std::strlen(password);
    if (size > sizeof(pad)) {
      if (!password_sha256::Hash::compute(reinterpret_cast<const uint8_t *>(password),
              size, pad)) return false;
    } else std::memcpy(pad, password, size);
    for (auto &byte : pad) byte ^= 0x36;
    if (!inner.start() || !inner.update(pad, sizeof(pad))) return false;
    for (auto &byte : pad) byte ^= 0x36 ^ 0x5c;
    const bool okay = outer.start() && outer.update(pad, sizeof(pad));
    erase(pad, sizeof(pad)); return okay;
  }
  bool hash(const password_sha256::Hash &seed, const uint8_t *bytes, size_t size, uint8_t *out) {
    work.cloneFrom(seed);
    return work.update(bytes, size) && work.finish(out);
  }
  bool compute(const uint8_t *bytes, size_t size, uint8_t *out) {
    return hash(inner, bytes, size, digest) && hash(outer, digest, sizeof(digest), out);
  }
};
bool derive(const char *password, const uint8_t (&salt)[16], uint8_t (&out)[32]) {
  // RFC 8018, one SHA-256 output block. Reuse HMAC pad states instead of
  // hashing both pads again on every iteration. Local software SHA avoids
  // acquiring/toggling the shared SHA/AES hardware engine for each final block.
  PasswordHmac hmac;
  uint8_t first[20] = {}, current[32] = {};
  std::memcpy(first, salt, sizeof(salt)); first[19] = 1;
  bool okay = hmac.setup(password) && hmac.compute(first, sizeof(first), current);
  if (okay) std::memcpy(out, current, sizeof(out));
  int64_t yielded = nowMs();
  for (uint32_t iteration = 1; okay && iteration < kIterations; ++iteration) {
    okay = hmac.compute(current, sizeof(current), current);
    if (!okay) break;
    for (size_t i = 0; i < sizeof(out); ++i) out[i] ^= current[i];
    // Block periodically so idle0 can service its watchdog, without running
    // the entire KDF at idle priority. Never sleep once per KDF iteration.
    if ((iteration & 7U) == 0 && nowMs() - yielded >= 4) {
      vTaskDelay(1); yielded = nowMs();
    }
  }
  erase(first, sizeof(first)); erase(current, sizeof(current));
  if (!okay) erase(out, sizeof(out));
  return okay;
}
bool verify(const char *password, const Credential &record, bool &cryptoOkay) {
  uint8_t output[32]; cryptoOkay = derive(password, record.salt, output);
  const bool match = cryptoOkay && equal(output, record.digest, sizeof(output)); erase(output, sizeof(output)); return match;
}
struct Operation {
  bool admitted = false;
  Operation() { Lock lock; if (lock.held && initialized && healthy && !maintenance && !saving && active == 0) { ++active; admitted = true; } }
  ~Operation() {
    if (!admitted) return;
    // Admission count must never be lost on a transient lock timeout.
    if (xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE) { --active; xSemaphoreGive(mutex); }
  }
};
void failedAttempt() {
  failures = std::min(failures + 1, 6U);
  nextAttempt = nowMs() + std::min(60000U, 1000U << (failures - 1));
}
bool pathIs(httpd_req_t *request, const char *path) {
  const size_t size = std::strcspn(request->uri, "?");
  return std::strlen(path) == size && !std::memcmp(request->uri, path, size);
}
bool templateFile(httpd_req_t *request, const char *path) {
  FILE *file = openWebFile(path); if (!file) return false;
  char line[512]; bool okay = true;
  while (okay && std::fgets(line, sizeof(line), file)) {
    char *p = line;
    while (char *marker = std::strstr(p, "{{csrf}}")) {
      okay = (marker == p || httpd_resp_send_chunk(request, p, int(marker - p)) == ESP_OK) &&
          httpd_resp_send_chunk(request, bootToken, 64) == ESP_OK;
      p = marker + 8; if (!okay) break;
    }
    if (okay && *p) okay = httpd_resp_send_chunk(request, p, int(std::strlen(p))) == ESP_OK;
  }
  okay = okay && !std::ferror(file); std::fclose(file); return okay;
}
esp_err_t renderLoginPage(httpd_req_t *request, const char *notice = nullptr) {
  bool ready = false, setup = false, waiting = false, authenticated = false, failed = false, blocked = false;
  char token[65];
  {
    Lock lock;
    if (lock.held) {
      ready = initialized && healthy; setup = !configured; waiting = pending; failed = saveFailed; blocked = maintenance;
      authenticated = ready && cookie(request, token) && findSession(token) >= 0;
    }
  }
  responseHeaders(request); httpd_resp_set_type(request, "text/html; charset=utf-8");
  if (httpd_resp_set_hdr(request, "Content-Security-Policy", "default-src 'none'; script-src 'self'; connect-src 'self'; style-src 'unsafe-inline'; form-action 'self'; base-uri 'none'; frame-ancestors 'none'") != ESP_OK) return ESP_FAIL;
  if (!templateFile(request, "templates/auth/head.html")) return ESP_FAIL;
  const char *message = !ready ? "Доступ заблоковано: не вдалося прочитати налаштування захисту. Перезапустіть контролер або зверніться до адміністратора." :
      waiting ? (failed ? "Пароль ще не збережено. Попередній пароль залишається чинним. Контролер повторить спробу збереження." :
          "Пароль очікує збереження. Новий пароль набуде чинності лише після успішного запису.") :
      blocked ? "Контролер виконує технічне обслуговування. Вхід і зміна пароля тимчасово недоступні." : nullptr;
  if (message) {
    if (httpd_resp_send_chunk(request, "<p role='status'>", HTTPD_RESP_USE_STRLEN) != ESP_OK ||
        httpd_resp_send_chunk(request, message, HTTPD_RESP_USE_STRLEN) != ESP_OK ||
        httpd_resp_send_chunk(request, "</p><p><a href='/login'>Оновити стан</a></p>", HTTPD_RESP_USE_STRLEN) != ESP_OK) return ESP_FAIL;
  }
  if (notice && (httpd_resp_send_chunk(request, "<p role='alert'>", HTTPD_RESP_USE_STRLEN) != ESP_OK ||
      httpd_resp_send_chunk(request, notice, HTTPD_RESP_USE_STRLEN) != ESP_OK ||
      httpd_resp_send_chunk(request, "</p>", HTTPD_RESP_USE_STRLEN) != ESP_OK)) return ESP_FAIL;
  if (ready && !blocked && (!waiting || (!setup && !authenticated))) {
    const char *form = authenticated ? "templates/auth/account.html" : setup ? "templates/auth/setup.html" : "templates/auth/login.html";
    if (!templateFile(request, form)) return ESP_FAIL;
  }
  if (!templateFile(request, "templates/auth/foot.html")) return ESP_FAIL;
  return httpd_resp_send_chunk(request, nullptr, 0);
}
esp_err_t loginPage(httpd_req_t *request) { return renderLoginPage(request); }
esp_err_t loginScript(httpd_req_t *request) {
  return serveWebFile(request, "assets/js/login.js", "text/javascript; charset=utf-8");
}
bool wantsJson(httpd_req_t *request) {
  char accept[192];
  return header(request, "Accept", accept) && std::strstr(accept, "application/json");
}
esp_err_t authError(httpd_req_t *request, const char *status, const char *message, unsigned retry = 0) {
  httpd_resp_set_status(request, status);
  httpd_resp_set_hdr(request, "Connection", "close");
  char retryText[12];
  if (retry) { std::snprintf(retryText, sizeof(retryText), "%u", retry); httpd_resp_set_hdr(request, "Retry-After", retryText); }
  if (!wantsJson(request)) return renderLoginPage(request, message);
  responseHeaders(request);
  httpd_resp_set_type(request, "application/json; charset=utf-8");
  // Callers supply static messages only, never passwords or request fields.
  char body[768]; size_t used = 0;
  const char prefix[] = "{\"error\":\"";
  std::memcpy(body, prefix, sizeof(prefix) - 1); used = sizeof(prefix) - 1;
  for (const char *p = message; *p; ++p) {
    if (used + 40 >= sizeof(body)) return reject(request, "503 Service Unavailable", "Спробуйте ще раз.");
    if (*p == '\"' || *p == '\\') body[used++] = '\\';
    if (uint8_t(*p) < 0x20) body[used++] = ' ';
    else body[used++] = *p;
  }
  std::snprintf(body + used, sizeof(body) - used, "\",\"retry_after\":%u}", retry);
  return httpd_resp_send(request, body, HTTPD_RESP_USE_STRLEN);
}
esp_err_t state(httpd_req_t *request) {
  bool okay = false, setup = false, waiting = false, authenticated = false;
  char token[65];
  { Lock lock; if (lock.held) { okay = initialized && healthy; setup = okay && !configured; waiting = pending; authenticated = okay && cookie(request, token) && findSession(token) >= 0; } }
  responseHeaders(request); httpd_resp_set_type(request, "application/json");
  char body[112]; std::snprintf(body, sizeof(body), "{\"available\":%s,\"setup\":%s,\"pending\":%s,\"authenticated\":%s}",
      okay ? "true" : "false", setup ? "true" : "false", waiting ? "true" : "false", authenticated ? "true" : "false");
  return httpd_resp_send(request, body, HTTPD_RESP_USE_STRLEN);
}
esp_err_t mutate(httpd_req_t *request) {
  if (!sameOrigin(request)) return authError(request, "403 Forbidden", "Запит має надходити зі сторінки цього контролера.");
  Operation operation;
  if (!operation.admitted) return authError(request, "503 Service Unavailable", "Контролер зайнятий або доступ заблоковано. Спробуйте пізніше.", 1);
  Form form;
  if (!readForm(request, form)) return authError(request, "400 Bad Request", "Некоректна форма. Оновіть сторінку входу та повторіть спробу.");
  char token[65] = {};
  const bool hasCookie = cookie(request, token);
  const bool login = pathIs(request, "/login"), setup = pathIs(request, "/auth/setup"), logout = pathIs(request, "/auth/logout");
  Credential snapshot; uint32_t version = 0; bool admitted = false, limited = false;
  unsigned retrySeconds = 0;
  {
    Lock lock;
    if (lock.held && !maintenance && healthy && !saving) {
      const bool authenticated = hasCookie && findSession(token) >= 0;
      if (logout && authenticated) {
        invalidateSessions(); admitted = true;
      } else if ((!pending || login) && ((login && configured) || (setup && !configured) || (!login && !setup && !logout && authenticated && configured))) {
        limited = nowMs() < nextAttempt;
        if (limited) retrySeconds = unsigned((nextAttempt - nowMs() + 999) / 1000);
        if (!limited) { nextAttempt = nowMs() + 1000; snapshot = credential; version = generation; admitted = true; }
      }
    }
  }
  if (limited) return authError(request, "429 Too Many Requests", "Зачекайте завершення паузи та повторіть спробу.", std::max(1U, retrySeconds));
  if (!admitted) return authError(request, "403 Forbidden", "Дію відхилено. Оновіть сторінку входу та перевірте стан.");
  if (logout) {
    httpd_resp_set_hdr(request, "Set-Cookie", "codex_session=; Max-Age=0; HttpOnly; SameSite=Strict; Path=/");
    return redirect(request, "/login");
  }
  const char *password = login ? form.password : form.next;
  const bool valid = passwordValid(password) && (login || (passwordValid(form.current) || setup)) &&
      (login || !std::strcmp(form.next, form.confirm));
  if (!valid) return authError(request, "400 Bad Request", "Пароль має містити щонайменше 10 символів і не більше 128 байтів. Новий пароль і підтвердження мають збігатися.");
  bool cryptoOkay = true;
  const int64_t checkStarted = nowMs();
  const bool matched = setup || verify(login ? form.password : form.current, snapshot, cryptoOkay);
  lastCheckDuration.store(uint32_t(std::max<int64_t>(0, nowMs() - checkStarted)));
  lastHttpStackFree.store(uint32_t(uxTaskGetStackHighWaterMark(nullptr)));
  if (!matched) {
    { Lock lock; if (lock.held && cryptoOkay) { failedAttempt(); retrySeconds = unsigned((nextAttempt - nowMs() + 999) / 1000); } }
    erase(&snapshot, sizeof(snapshot));
    return authError(request, cryptoOkay ? "401 Unauthorized" : "503 Service Unavailable", cryptoOkay ? "Неправильний пароль. Спробуйте ще раз." : "Перевірка пароля недоступна. Спробуйте пізніше.", retrySeconds);
  }
  Credential replacement;
  if (!login) {
    esp_fill_random(replacement.salt, sizeof(replacement.salt));
    if (!derive(form.next, replacement.salt, replacement.digest)) return authError(request, "503 Service Unavailable", "Не вдалося підготувати пароль. Спробуйте пізніше.");
    replacement.checksum = checksum(replacement);
  }
  bool accepted = false;
  char sessionCookie[160] = {};
  {
    Lock lock;
    if (lock.held && !maintenance && !saving && (!pending || login) && healthy && generation == version &&
        (setup ? !configured : configured) && (login || setup || (hasCookie && findSession(token) >= 0))) {
      if (login) {
        unsigned slot = 0;
        for (unsigned i = 0; i < 4; ++i) {
          if (!sessions[i].used) { slot = i; break; }
          if (sessions[i].issued < sessions[slot].issued) slot = i;
        }
        auto &entry = sessions[slot]; esp_fill_random(entry.token, sizeof(entry.token)); entry.issued = nowMs(); entry.used = true;
        char value[65]; hex(entry.token, sizeof(entry.token), value);
        std::snprintf(sessionCookie, sizeof(sessionCookie), "codex_session=%s; Max-Age=28800; HttpOnly; SameSite=Strict; Path=/", value);
        erase(value, sizeof(value)); revision.fetch_add(1);
      } else { staged = replacement; pending = true; saveFailed = false; }
      failures = 0; accepted = true;
    }
  }
  erase(&snapshot, sizeof(snapshot)); erase(&replacement, sizeof(replacement)); erase(token, sizeof(token));
  if (!accepted) return authError(request, "409 Conflict", "Стан контролера змінився. Оновіть сторінку входу та повторіть дію.");
  if (login) httpd_resp_set_hdr(request, "Set-Cookie", sessionCookie);
  // IDF retains header pointers until the response is sent.
  esp_err_t result;
  if (login && wantsJson(request)) {
    responseHeaders(request); httpd_resp_set_type(request, "application/json; charset=utf-8");
    result = httpd_resp_send(request, "{\"authenticated\":true,\"redirect\":\"/\"}", HTTPD_RESP_USE_STRLEN);
  } else result = redirect(request, login ? "/" : "/login?pending=1");
  erase(sessionCookie, sizeof(sessionCookie));
  return result;
}
}  // namespace

void initialize() {
  if (initialized || mutex) return;
  mutex = xSemaphoreCreateMutexStatic(&mutexStorage); if (!mutex) return;
  uint8_t random[32]; esp_fill_random(random, sizeof(random)); hex(random, sizeof(random), bootToken); erase(random, sizeof(random));
  invalidateSessions(); initialized = true;
  if (psa_crypto_init() != PSA_SUCCESS) return;
  nvs_handle_t handle;
  const esp_err_t opened = nvs_open("web_auth", NVS_READONLY, &handle);
  if (opened == ESP_ERR_NVS_NOT_FOUND) { healthy = true; return; }
  if (opened != ESP_OK) return;
  Credential record; size_t size = sizeof(record);
  const esp_err_t result = nvs_get_blob(handle, "credential", &record, &size); nvs_close(handle);
  if (result == ESP_ERR_NVS_NOT_FOUND) { healthy = true; return; }
  if (result != ESP_OK || size != sizeof(record) || record.magic != kMagic || record.version != 1 ||
      record.iterations != kIterations || record.checksum != checksum(record)) return;
  credential = record; configured = healthy = true; erase(&record, sizeof(record));
}
bool registerRoutes(httpd_handle_t server) {
  struct Route { const char *uri; httpd_method_t method; esp_err_t (*handler)(httpd_req_t *); };
  const Route routes[] = {{"/login", HTTP_GET, loginPage}, {"/login", HTTP_POST, mutate},
      {"/auth/setup", HTTP_POST, mutate}, {"/auth/logout", HTTP_POST, mutate},
      {"/auth/password", HTTP_POST, mutate}, {"/auth/state", HTTP_GET, state},
      {"/auth/login.js", HTTP_GET, loginScript}};
  for (const auto &entry : routes) {
    httpd_uri_t route{}; route.uri = entry.uri; route.method = entry.method; route.handler = entry.handler;
    if (httpd_register_uri_handler(server, &route) != ESP_OK) return false;
  }
  return true;
}
bool captureAuthenticatedSession(httpd_req_t *request, char token[65]) {
  char value[65] = {}; token[0] = 0;
  if (!cookie(request, value)) return false;
  Lock lock;
  const bool valid = lock.held && initialized && healthy && configured && findSession(value) >= 0;
  if (valid) std::memcpy(token, value, sizeof(value));
  erase(value, sizeof(value));
  return valid;
}
bool sessionActive(const char *token) {
  Lock lock; return lock.held && initialized && healthy && configured && findSession(token) >= 0;
}
bool authorize(httpd_req_t *request) {
  if (unsafe(request) && !sameOrigin(request)) { reject(request, "403 Forbidden", "Запит має надходити зі сторінки цього контролера."); return false; }
  char token[65];
  if (captureAuthenticatedSession(request, token)) { erase(token, sizeof(token)); return true; }
  char accept[192];
  const bool page = request->method == HTTP_GET && (pathIs(request, "/") || pathIs(request, "/settings") ||
      pathIs(request, "/agents") || pathIs(request, "/weekly") || (header(request, "Accept", accept) && std::strstr(accept, "text/html")));
  if (page) redirect(request, "/login", pathIs(request, "/"));
  else reject(request, "401 Unauthorized", "Потрібен вхід: /login");
  return false;
}
bool pendingSave() { Lock lock; return !lock.held || pending || saving; }
bool busy() { Lock lock; return !lock.held || active != 0 || saving; }
void setMaintenance(bool enabled) {
  if (!mutex) return;
  if (xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE) { maintenance = enabled; xSemaphoreGive(mutex); }
}
uint32_t sessionRevision() { return revision.load(); }
uint32_t lastCheckMs() { return lastCheckDuration.load(); }
uint32_t httpStackFree() { return lastHttpStackFree.load(); }
bool tryMaintenance() {
  Lock lock;
  if (!lock.held || active || saving) return false;
  maintenance = true;
  return true;
}
bool persist() {
  Credential record; uint32_t version = 0;
  {
    Lock lock;
    if (!lock.held || !initialized || !healthy || saving || active) return false;
    if (!pending) return true;
    saving = true; record = staged; version = generation;
  }
  // Caller owns flash exclusion. No HTTP handler can stage a replacement while saving.
  nvs_handle_t handle; bool okay = nvs_open("web_auth", NVS_READWRITE, &handle) == ESP_OK;
  if (okay) { okay = nvs_set_blob(handle, "credential", &record, sizeof(record)) == ESP_OK && nvs_commit(handle) == ESP_OK; nvs_close(handle); }
  if (xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE) {
    if (okay && pending && version == generation) {
      credential = record; configured = true; pending = false; saveFailed = false; ++generation;
      erase(&staged, sizeof(staged)); invalidateSessions();
    } else { okay = false; saveFailed = true; }
    saving = false; xSemaphoreGive(mutex);
  } else okay = false;
  erase(&record, sizeof(record)); return okay;
}
}  // namespace web_auth
