#include "web_files.hpp"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_app_desc.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psa/crypto.h"

namespace {
constexpr char kTag[] = "web-files";
constexpr size_t kMaxPath = 127, kMaxFile = 256 * 1024, kMaxBundle = 1024 * 1024;
extern const uint8_t bundleStart[] asm("_binary_web_bundle_bin_start");
extern const uint8_t bundleEnd[] asm("_binary_web_bundle_bin_end");
char root[32] = {}, version[65] = {};
bool ready = false;

struct CachedAsset { const char *path; char etag[136]; };
CachedAsset cachedAssets[] = {
  {"assets/css/portal.css", {}}, {"assets/css/preview.css", {}},
  {"assets/js/portal.js", {}}, {"assets/js/preview.js", {}},
  {"images/device.png", {}}, {"images/favicon.svg", {}},
  {"assets/js/boot.js", {}}
};
void initializeAssetEtags() {
  char firmware[65];
  const auto *app = esp_app_get_description();
  for (size_t i = 0; i < sizeof(app->app_elf_sha256); ++i)
    std::snprintf(firmware + i * 2, 3, "%02x", app->app_elf_sha256[i]);
  for (size_t i = 0; i < sizeof(cachedAssets) / sizeof(cachedAssets[0]); ++i)
    std::snprintf(cachedAssets[i].etag, sizeof(cachedAssets[i].etag), "\"%s-%s-%u\"", firmware, version, unsigned(i));
}
const char *assetEtag(const char *path) {
  if (ready) for (const auto &asset : cachedAssets)
    if (!std::strcmp(path, asset.path)) return asset.etag;
  return nullptr;
}
bool matchesAssetEtag(const char *header, const char *etag) {
  const auto skipSpace = [](const char *&p) { while (*p == ' ' || *p == '\t') ++p; };
  const char *p = header;
  skipSpace(p);
  if (*p == '*') { ++p; skipSpace(p); return !*p; }
  bool matched = false;
  const size_t length = std::strlen(etag);
  for (;;) {
    if (p[0] == 'W' && p[1] == '/') p += 2;
    const char *start = p;
    if (*p++ != '"') return false;
    while (*p && *p != '"') {
      const unsigned char c = static_cast<unsigned char>(*p++);
      if (c < 0x21 || c == 0x7f) return false;
    }
    if (!*p) return false;
    ++p;
    matched = matched || (size_t(p - start) == length && !std::memcmp(start, etag, length));
    skipSpace(p);
    if (!*p) return matched;
    if (*p++ != ',') return false;
    skipSpace(p);
  }
}
bool assetNotModified(httpd_req_t *request, const char *etag) {
  // Oversized or malformed validators fall back to a full response, never allocation.
  char header[256];
  const size_t length = httpd_req_get_hdr_value_len(request, "If-None-Match");
  return length && length < sizeof(header) &&
      httpd_req_get_hdr_value_str(request, "If-None-Match", header, sizeof(header)) == ESP_OK &&
      matchesAssetEtag(header, etag);
}

uint32_t u32(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
bool digest(const void *bytes, size_t size, uint8_t (&output)[32]) {
  size_t written = 0;
  return psa_hash_compute(PSA_ALG_SHA_256, static_cast<const uint8_t *>(bytes), size,
                          output, sizeof(output), &written) == PSA_SUCCESS && written == sizeof(output);
}
bool validPath(const char *path) {
  if (!path || !*path || std::strlen(path) > kMaxPath || *path == '/') return false;
  const char *part = path;
  for (const char *p = path;; ++p) {
    if (!*p || *p == '/') {
      const size_t size = p - part;
      if (!size || (size == 1 && part[0] == '.') || (size == 2 && part[0] == '.' && part[1] == '.')) return false;
      if (!*p) return true;
      part = p + 1;
    } else if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                 (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' || *p == '-')) return false;
  }
}
struct Entry { char path[kMaxPath + 1]; const uint8_t *hash; const uint8_t *data; uint32_t size; };
bool nextEntry(const uint8_t *&cursor, Entry &entry) {
  if (bundleEnd - cursor < 38) return false;
  const size_t length = size_t(cursor[0]) | size_t(cursor[1]) << 8;
  entry.size = u32(cursor + 2); entry.hash = cursor + 6; cursor += 38;
  if (!length || length > kMaxPath || entry.size > kMaxFile || size_t(bundleEnd - cursor) < length + entry.size) return false;
  std::memcpy(entry.path, cursor, length); entry.path[length] = 0;
  if (std::strlen(entry.path) != length || !validPath(entry.path)) return false;
  entry.data = cursor + length; cursor += length + entry.size;
  return true;
}
bool validateBundle(uint32_t &count) {
  const size_t size = bundleEnd - bundleStart;
  if (size < 44 || size > kMaxBundle + 44 || std::memcmp(bundleStart, "LFWEB01\0", 8)) return false;
  count = u32(bundleStart + 8);
  if (!count || count > 256) return false;
  uint8_t hash[32];
  if (!digest(bundleStart + 44, size - 44, hash) || std::memcmp(hash, bundleStart + 12, 32)) return false;
  for (size_t i = 0; i < 32; ++i) std::snprintf(version + i * 2, 3, "%02x", hash[i]);
  const uint8_t *cursor = bundleStart + 44;
  char previous[kMaxPath + 1] = {};
  for (uint32_t i = 0; i < count; ++i) {
    Entry entry;
    if (!nextEntry(cursor, entry) || std::strcmp(previous, entry.path) >= 0 ||
        !digest(entry.data, entry.size, hash) || std::memcmp(hash, entry.hash, 32)) return false;
    std::strcpy(previous, entry.path);
  }
  return cursor == bundleEnd;
}
bool fileMatches(const Entry &entry) {
  char path[192]; std::snprintf(path, sizeof(path), "%s/%s", root, entry.path);
  FILE *file = std::fopen(path, "rb");
  if (!file) return false;
  psa_hash_operation_t operation = PSA_HASH_OPERATION_INIT;
  bool ok = psa_hash_setup(&operation, PSA_ALG_SHA_256) == PSA_SUCCESS;
  uint8_t bytes[1024], hash[32]; size_t total = 0, size;
  while (ok && (size = std::fread(bytes, 1, sizeof(bytes), file))) {
    total += size;
    ok = total <= entry.size && psa_hash_update(&operation, bytes, size) == PSA_SUCCESS;
  }
  size_t written = 0;
  ok = ok && !std::ferror(file) && total == entry.size &&
       psa_hash_finish(&operation, hash, sizeof(hash), &written) == PSA_SUCCESS && written == sizeof(hash) &&
       !std::memcmp(hash, entry.hash, sizeof(hash));
  psa_hash_abort(&operation); std::fclose(file);
  return ok;
}
bool installed(uint32_t count) {
  const uint8_t *cursor = bundleStart + 44;
  for (uint32_t i = 0; i < count; ++i) { Entry entry; if (!nextEntry(cursor, entry) || !fileMatches(entry)) return false; }
  return true;
}
bool removeTree(const char *path, unsigned depth = 0) {
  if (depth > 8) return false;
  DIR *directory = opendir(path);
  if (!directory) return errno == ENOENT;
  bool ok = true;
  while (auto *entry = readdir(directory)) {
    if (!std::strcmp(entry->d_name, ".") || !std::strcmp(entry->d_name, "..")) continue;
    char child[224];
    if (std::snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) >= int(sizeof(child))) { ok = false; break; }
    struct stat info{};
    if (stat(child, &info) || (S_ISDIR(info.st_mode) ? !removeTree(child, depth + 1) : unlink(child) != 0)) { ok = false; break; }
  }
  closedir(directory);
  return ok && rmdir(path) == 0;
}
bool makeParents(char *path) {
  for (char *p = path + 1; *p; ++p) if (*p == '/') {
    *p = 0;
    const int result = mkdir(path, 0755); const int saved = errno; *p = '/';
    if (result && saved != EEXIST) return false;
  }
  return true;
}
bool install(uint32_t count) {
  // Only the running slot's web tree is replaced; the other slot survives rollback.
  if (!removeTree(root)) return false;
  const uint8_t *cursor = bundleStart + 44;
  for (uint32_t i = 0; i < count; ++i) {
    Entry entry; if (!nextEntry(cursor, entry)) return false;
    char path[192]; std::snprintf(path, sizeof(path), "%s/%s", root, entry.path);
    if (!makeParents(path)) return false;
    FILE *file = std::fopen(path, "wb"); if (!file) return false;
    bool ok = true;
    for (size_t offset = 0; offset < entry.size;) {
      const size_t remaining = entry.size - offset, size = remaining > 1024 ? 1024 : remaining;
      if (std::fwrite(entry.data + offset, 1, size, file) != size) { ok = false; break; }
      offset += size; vTaskDelay(1);
    }
    if (std::fflush(file) || fsync(fileno(file))) ok = false;
    if (std::fclose(file)) ok = false;
    if (!ok || !fileMatches(entry)) return false;
  }
  return true;
}
bool erased(const esp_partition_t *partition) {
  uint8_t bytes[1024];
  for (size_t offset = 0; offset < partition->size; offset += sizeof(bytes)) {
    const size_t size = partition->size - offset < sizeof(bytes) ? partition->size - offset : sizeof(bytes);
    if (esp_partition_read(partition, offset, bytes, size) != ESP_OK) return false;
    for (size_t i = 0; i < size; ++i) if (bytes[i] != 0xff) return false;
    if (!(offset % (64 * 1024))) vTaskDelay(1);
  }
  return true;
}
}

bool initializeWebFiles() {
  uint32_t count = 0;
  if (psa_crypto_init() != PSA_SUCCESS || !validateBundle(count)) { ESP_LOGE(kTag, "Invalid web bundle"); return false; }
  const esp_partition_t *partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "storage");
  const esp_partition_t *app = esp_ota_get_running_partition();
  if (!partition || partition->address != 0xC30000 || partition->size != 0x3D0000 || partition->encrypted ||
      !app || (app->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_0 && app->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_1)) return false;
  std::snprintf(root, sizeof(root), "/www/ota_%u", unsigned(app->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_0));
  esp_vfs_littlefs_conf_t config{}; config.base_path = "/www"; config.partition_label = "storage";
  // Never auto-format unknown data or erase NVS / either application partition.
  config.format_if_mount_failed = false;
  config.read_only = true;
  esp_err_t result = esp_vfs_littlefs_register(&config);
  if (result != ESP_OK) {
    if (!erased(partition)) { ESP_LOGE(kTag, "Storage is not blank; refusing destructive recovery"); return false; }
    config.format_if_mount_failed = true;
    config.read_only = false;
    result = esp_vfs_littlefs_register(&config);
    if (result != ESP_OK) return false;
  }
  bool ok = installed(count);
  if (!ok) {
    if (config.read_only) {
      if (esp_vfs_littlefs_unregister("storage") != ESP_OK) return false;
      config.read_only = false;
      if (esp_vfs_littlefs_register(&config) != ESP_OK) return false;
    }
    ok = install(count);
  }
  if (esp_vfs_littlefs_unregister("storage") != ESP_OK || !ok) return false;
  config.format_if_mount_failed = false; config.read_only = true;
  ready = esp_vfs_littlefs_register(&config) == ESP_OK;
  if (ready) initializeAssetEtags();
  if (ready) ESP_LOGI(kTag, "Serving %lu verified files from %s (%.12s), read-only", (unsigned long)count, root, version);
  return ready;
}

FILE *openWebFile(const char *relativePath) {
  if (!ready || !validPath(relativePath)) return nullptr;
  char path[192]; std::snprintf(path, sizeof(path), "%s/%s", root, relativePath);
  return std::fopen(path, "rb");
}
const char *webFilesVersion() { return ready ? version : "unavailable"; }

esp_err_t serveWebFile(httpd_req_t *request, const char *relativePath, const char *mime) {
  FILE *file = openWebFile(relativePath);
  if (!file) {
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "Web file unavailable");
  }
  httpd_resp_set_type(request, mime);
  httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
  const char *etag = assetEtag(relativePath);
  httpd_resp_set_hdr(request, "Cache-Control", etag ? "private, no-cache" : "no-store");
  if (etag) {
    // Stable URLs must revalidate even immediately after firmware replacement.
    httpd_resp_set_hdr(request, "ETag", etag);
    if (assetNotModified(request, etag)) {
      std::fclose(file);
      httpd_resp_set_status(request, "304 Not Modified");
      return httpd_resp_send(request, nullptr, 0);
    }
  }
  char bytes[1024]; size_t size; esp_err_t result = ESP_OK;
  while ((size = std::fread(bytes, 1, sizeof(bytes), file)) != 0) {
    result = httpd_resp_send_chunk(request, bytes, size); if (result != ESP_OK) break;
  }
  const bool failed = std::ferror(file); std::fclose(file);
  if (result != ESP_OK || failed) return ESP_FAIL;
  return httpd_resp_send_chunk(request, nullptr, 0);
}
