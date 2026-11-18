// Private implementation unit; included only by network.cpp.
enum class OtaPhase : uint8_t { Manifest = 1, Image, Renderer, Recovery, BootConfirmation, Deadline };
void reportOtaFailure(OtaPhase phase, int32_t reason, uint32_t elapsed, unsigned retries = 0) {
  diagnostic_log::Snapshot failure;
  failure.reason = reason;
  failure.detail = static_cast<int32_t>(phase);
  failure.elapsed_ms = elapsed;
  failure.attempts = retries;
  diagnostics::event(diagnostics::Level::Warning, diagnostics::Module::Ota,
                     diagnostics::Code::UpdateFailed, failure);
}
uint32_t otaElapsed() { return uptimeMs() - (model.otaDeadline - kOtaCycleMs); }
bool otaExpired() { return due(uptimeMs(), model.otaDeadline); }

struct HttpHeaders { char location[1024] = {}; bool encoded = false; };
esp_err_t httpEvent(esp_http_client_event_t *event) {
  if (event->event_id != HTTP_EVENT_ON_HEADER || !event->user_data) return ESP_OK;
  auto &headers = *static_cast<HttpHeaders *>(event->user_data);
  if (!strcasecmp(event->header_key, "Location")) copy(headers.location, event->header_value);
  if (!strcasecmp(event->header_key, "Content-Encoding") && strcasecmp(event->header_value, "identity")) headers.encoded = true;
  return ESP_OK;
}
bool githubRequestUrl(const char *source, uint64_t cacheKey, char (&url)[1024]) {
  // Keep the published API URLs compatible with older installed firmware.
  constexpr char raw[] = "https://raw.githubusercontent.com/Vihten/CODEX_firmware_esp8266/refs/heads/main/esp32-s3";
  int length;
  if (!std::strcmp(source, kFirmwareUrl))
    length = std::snprintf(url, sizeof(url), "%s/firmware.bin?version=%llu", raw, (unsigned long long)cacheKey);
  else if (!std::strcmp(source, "https://api.github.com/repos/Vihten/CODEX_firmware_esp8266/contents/esp32-s3/manifest.json?ref=main"))
    length = std::snprintf(url, sizeof(url), "%s/manifest.json?check=%016llx", raw, (unsigned long long)cacheKey);
  else length = std::snprintf(url, sizeof(url), "%s", source);
  return length >= 0 && size_t(length) < sizeof(url);
}
bool githubHttps(const char *url) {
  if (std::strncmp(url, "https://", 8)) return false;
  const char *end = std::strchr(url + 8, '/'); if (!end) return false;
  const size_t length = end - (url + 8);
  for (const char *host : {"api.github.com", "raw.githubusercontent.com", "objects.githubusercontent.com", "github.com"})
    if (std::strlen(host) == length && !std::strncmp(url + 8, host, length)) return true;
  return false;
}
class Download {
  esp_http_client_handle_t client_ = nullptr;
  HttpHeaders headers_;
  const uint32_t deadline_;
  int32_t reason_ = ESP_FAIL;
  unsigned retries_ = 0;
  bool retryable_ = false;

  int timeoutMs() const {
    const uint32_t now = uptimeMs();
    return due(now, deadline_) ? 0 : int(std::min(uint32_t(8000), deadline_ - now));
  }
  bool openOnce(const char *source, uint64_t cacheKey) {
    char url[1024];
    if (!githubRequestUrl(source, cacheKey, url)) { reason_ = ESP_ERR_INVALID_ARG; return false; }
    for (unsigned redirects = 0; redirects <= 4; ++redirects) {
      const int timeout = timeoutMs();
      if (!timeout) { reason_ = ESP_ERR_TIMEOUT; return false; }
      if (!githubHttps(url)) { reason_ = ESP_ERR_INVALID_ARG; return false; }
      headers_ = {};
      esp_http_client_config_t config{}; config.url = url; config.crt_bundle_attach = esp_crt_bundle_attach;
      config.timeout_ms = timeout; config.buffer_size = 4096; config.buffer_size_tx = 1024;
      config.disable_auto_redirect = true; config.event_handler = httpEvent; config.user_data = &headers_;
      client_ = esp_http_client_init(&config);
      if (!client_) { reason_ = ESP_ERR_NO_MEM; return false; }
      esp_http_client_set_header(client_, "Accept", "application/vnd.github.raw+json");
      esp_http_client_set_header(client_, "Accept-Encoding", "identity");
      esp_http_client_set_header(client_, "Cache-Control", "no-cache");
      esp_http_client_set_header(client_, "User-Agent", "LED-Forge-ESP32-S3");
      reason_ = esp_http_client_open(client_, 0);
      if (reason_ != ESP_OK) { retryable_ = true; return false; }
      const int headerTimeout = timeoutMs();
      if (!headerTimeout) { reason_ = ESP_ERR_TIMEOUT; return false; }
      esp_http_client_set_timeout_ms(client_, headerTimeout);
      const int64_t length = esp_http_client_fetch_headers(client_);
      if (expired()) { reason_ = ESP_ERR_TIMEOUT; return false; }
      if (length < 0) { reason_ = int32_t(length); retryable_ = true; return false; }
      const int status = esp_http_client_get_status_code(client_);
      if (status == 200) {
        reason_ = headers_.encoded ? ESP_ERR_NOT_SUPPORTED : ESP_OK;
        return !headers_.encoded;
      }
      reason_ = status;
      if (status != 301 && status != 302 && status != 303 && status != 307 && status != 308) {
        retryable_ = status == 408 || status == 500 || status == 502 || status == 503 || status == 504;
        return false;
      }
      if (!headers_.location[0] || !githubHttps(headers_.location)) return false;
      copy(url, headers_.location); close();
    }
    return false;
  }
public:
  explicit Download(uint32_t deadline) : deadline_(deadline) {}
  ~Download() { close(); }
  void close() { if (client_) { esp_http_client_close(client_); esp_http_client_cleanup(client_); client_ = nullptr; } }
  bool expired() const { return due(uptimeMs(), deadline_); }
  int32_t reason() const { return expired() ? ESP_ERR_TIMEOUT : reason_; }
  unsigned retries() const { return retries_; }
  bool open(const char *source, uint64_t cacheKey) {
    if (openOnce(source, cacheKey)) return true;
    close();
    constexpr uint32_t retryDelay = 500;
    if (!retryable_ || timeoutMs() <= int(retryDelay)) return false;
    ESP_LOGW(kTag, "Retrying OTA HTTPS open (reason=%ld)", (long)reason_);
    vTaskDelay(pdMS_TO_TICKS(retryDelay));
    ++retries_;
    const bool opened = openOnce(source, cacheKey);
    if (!opened) close();
    return opened;
  }
  int read(void *data, size_t size) {
    const int timeout = timeoutMs();
    if (!timeout) return -ESP_ERR_TIMEOUT;
    esp_http_client_set_timeout_ms(client_, timeout);
    const int count = esp_http_client_read(client_, static_cast<char *>(data), int(size));
    return expired() ? -ESP_ERR_TIMEOUT : count;
  }
  int64_t length() const { return esp_http_client_get_content_length(client_); }
  bool complete() const { return esp_http_client_is_complete_data_received(client_); }
};
bool jsonInteger(cJSON *root, const char *key, uint64_t maximum, uint64_t &value) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
  if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) || item->valuedouble < 0 ||
      item->valuedouble > double(maximum) || std::floor(item->valuedouble) != item->valuedouble) return false;
  value = uint64_t(item->valuedouble); return true;
}
bool jsonEquals(cJSON *root, const char *key, const char *expected) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
  return cJSON_IsString(item) && item->valuestring && !std::strcmp(item->valuestring, expected);
}
ManifestResult fetchManifest(const ManifestJob &job) {
  ManifestResult result;
  Download download(job.deadline);
  const auto finish = [&](const char *status, esp_err_t reason = ESP_ERR_INVALID_RESPONSE) {
    copy(result.status, status);
    result.reason = reason;
    result.retries = download.retries();
    result.elapsed = uptimeMs() - (job.deadline - kManifestBudgetMs);
    return result;
  };
  // GitHub's raw CDN can serve cached content even with Cache-Control: no-cache.
  // Give each manifest check its own key, including immediately after a reboot.
  const uint64_t nonceHigh = esp_random();
  const uint64_t nonce = (nonceHigh << 32) | esp_random();
  if (!download.open(job.url, nonce))
    return finish(download.expired() ? "Manifest timed out" : "Manifest HTTPS request failed", download.reason());
  if (download.length() > 4096) return finish("Manifest exceeds 4096 bytes");
  char body[4097]; size_t used = 0;
  for (;;) {
    const int count = download.read(body + used, sizeof(body) - 1 - used);
    if (download.expired()) return finish("Manifest timed out", ESP_ERR_TIMEOUT);
    if (count < 0) return finish("Manifest read failed", count);
    if (!count) break;
    if (std::memchr(body + used, 0, count)) return finish("Manifest contains binary data");
    used += size_t(count);
    if (used == sizeof(body) - 1) return finish("Manifest too large");
  }
  if (!download.complete()) return finish("Manifest incomplete");
  // The body is complete and local: release TLS/socket allocations before
  // allocating the JSON tree. close() is idempotent with the RAII destructor.
  download.close();
  body[used] = 0;
  const char *end = nullptr; cJSON *json = cJSON_ParseWithLengthOpts(body, used + 1, &end, true);
  if (!json) return finish("Manifest JSON invalid");
  uint64_t version = 0, size = 0, pin = 0, fps = 0;
  const cJSON *hash = cJSON_GetObjectItemCaseSensitive(json, "sha256");
  bool valid = job.partitionSize && jsonEquals(json, "board", "esp32-s3-n16r8") && jsonEquals(json, "appProject", "led_forge_s3") &&
      jsonEquals(json, "compression", "none") && jsonEquals(json, "ledTransport", "rmt-dma") && jsonEquals(json, "firmwareUrl", kFirmwareUrl) &&
      jsonInteger(json, "ledDataPin", 48, pin) && pin == PROJECT_DATA_PIN && pin == 18 &&
      jsonInteger(json, "targetFps", 1000, fps) && fps == PROJECT_FPS && fps == 30 &&
      jsonInteger(json, "version", 9007199254740991ULL, version) &&
      jsonInteger(json, "size", job.partitionSize, size) && size >= sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t) &&
      cJSON_IsString(hash) && hash->valuestring && std::strlen(hash->valuestring) == 64;
  Manifest manifest;
  if (valid) for (size_t i = 0; i < 32; ++i) {
    const int high = hexDigit(hash->valuestring[i * 2]), low = hexDigit(hash->valuestring[i * 2 + 1]);
    if (high < 0 || low < 0) { valid = false; break; }
    manifest.sha[i] = uint8_t((high << 4) | low);
  }
  cJSON_Delete(json);
  if (download.expired()) return finish("Manifest timed out", ESP_ERR_TIMEOUT);
  if (!valid) return finish("Manifest does not match this S3 image");
  if (version < job.currentVersion) return finish("GitHub manifest older than installed firmware");
  if (version == job.currentVersion) { result.upToDate = true; return finish("Up to date", ESP_OK); }
  manifest.version = version; manifest.size = size;
  result.manifest = manifest; result.newer = true;
  return finish("New firmware verified; waiting for LEDs", ESP_OK);
}
void manifestTask(void *) {
  ManifestJob job;
  for (;;) {
    if (xQueueReceive(manifestJobs, &job, portMAX_DELAY) != pdTRUE) continue;
    const ManifestResult result = fetchManifest(job);
    xQueueOverwrite(manifestResults, &result);
  }
}
void serviceManifestResults() {
  ManifestResult result;
  if (xQueueReceive(manifestResults, &result, 0) != pdTRUE) return;
  model.manifestBusy = false; otaMessage(result.status);
  if (!result.newer && !result.upToDate)
    reportOtaFailure(OtaPhase::Manifest, result.reason, result.elapsed, result.retries);
  if (result.newer) { model.manifest = result.manifest; model.manifestReady = true; }
  else model.otaAt = uptimeMs() + (result.upToDate ? kOtaInterval : 60000);
}
void queueManifestCheck() {
  if (model.manifestBusy || model.manifestReady || model.restarting ||
      model.control.maintenance != Maintenance::None) return;
  if (air_alert::busy() || !backgroundTlsReady()) { model.otaAt = uptimeMs() + 5000; return; }
  if (!model.manifestWorkerReady) { otaMessage("Manifest worker unavailable"); model.otaAt = uptimeMs() + 60000; return; }
  const esp_partition_t *partition = esp_ota_get_next_update_partition(nullptr);
  if (!partition) { otaMessage("No OTA partition available"); model.otaAt = uptimeMs() + 60000; return; }
  ManifestJob job; copy(job.url, PROJECT_OTA_MANIFEST_URL); job.partitionSize = partition->size;
  const uint32_t started = uptimeMs();
  job.deadline = started + kManifestBudgetMs;
  if (xQueueSend(manifestJobs, &job, 0) != pdTRUE) { otaMessage("Manifest worker busy"); return; }
  model.otaDeadline = started + kOtaCycleMs;
  model.manifestBusy = true; model.updateRequested = false; model.otaAt = started + kOtaInterval;
  otaMessage("Checking S3 firmware manifest");
}
bool imageDescriptor(const uint8_t *data, size_t size) {
  constexpr size_t offset = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
  if (size < offset + sizeof(esp_app_desc_t)) return false;
  esp_image_header_t header; esp_app_desc_t app;
  std::memcpy(&header, data, sizeof(header)); std::memcpy(&app, data + offset, sizeof(app));
  if (header.magic != ESP_IMAGE_HEADER_MAGIC || header.chip_id != ESP_CHIP_ID_ESP32S3 || !header.segment_count ||
      app.magic_word != ESP_APP_DESC_MAGIC_WORD || !std::memchr(app.project_name, 0, sizeof(app.project_name)) ||
      std::strcmp(app.project_name, "led_forge_s3") || !std::memchr(app.version, 0, sizeof(app.version))) return false;
  uint64_t version;
  return unsignedValue(app.version, 9007199254740991ULL, version) && version == model.manifest.version;
}
bool otaVisualAcknowledged() {
  return model.control.maintenance == Maintenance::Ota && model.control.otaVisualSequence &&
      !model.control.otaVisualAnimate &&
      model.render.maintenanceEpoch == model.control.maintenanceEpoch &&
      model.render.otaVisualSequence == model.control.otaVisualSequence &&
      model.render.quiescent && model.render.ledError == 0;
}
void beginOtaVisualAnimation(uint8_t percent) {
  if (model.control.maintenance != Maintenance::Ota || model.otaVisualTimedOut) return;
  ++model.control.otaVisualSequence;
  if (!model.control.otaVisualSequence) ++model.control.otaVisualSequence;
  model.control.otaVisualPercent = std::min(percent, uint8_t(99));
  model.control.otaVisualAnimate = true;
  model.control.on = scheduledOn();
  // Only network reads may overlap repeated LED transfers; no flash calls here.
  xQueueOverwrite(controlQueue, &model.control);
  publishView();
}
bool requestOtaVisual(uint8_t percent) {
  if (model.control.maintenance != Maintenance::Ota || model.otaVisualTimedOut) return false;
  ++model.control.otaVisualSequence;
  if (!model.control.otaVisualSequence) ++model.control.otaVisualSequence;
  model.control.otaVisualPercent = std::min(percent, uint8_t(100));
  model.control.otaVisualAnimate = false;
  model.control.on = scheduledOn();
  // Retain brightness and avoid updateControl's broader state work during OTA.
  xQueueOverwrite(controlQueue, &model.control);
  publishView();
  const uint32_t started = uptimeMs();
  for (;;) {
    const uint32_t elapsed = uptimeMs() - started;
    if (elapsed >= kOtaVisualAckMs) break;
    RenderStatus render;
    const uint32_t remaining = kOtaVisualAckMs - elapsed;
    if (xQueueReceive(renderStatusQueue, &render, pdMS_TO_TICKS(std::min(remaining, uint32_t(10)))) == pdTRUE) {
      model.render = render;
      if (otaVisualAcknowledged()) return true;
    }
    // Yield even when stale status reports keep the queue readable (idle watchdog).
    if (uptimeMs() - started < kOtaVisualAckMs) vTaskDelay(1);
  }
  // A delayed stop request can still start DMA. Keep maintenance sealed and
  // forbid flash operations until this exact request is acknowledged.
  model.otaVisualTimedOut = true;
  model.otaRecoveryAt = uptimeMs() + kOtaRecoveryMs;
  ++model.maintenanceTimeouts;
  otaMessage("OTA visual ACK timed out; waiting for renderer recovery");
  reportOtaFailure(OtaPhase::Renderer, model.render.ledError ? model.render.ledError : ESP_ERR_TIMEOUT,
                   otaElapsed());
  return false;
}

void reportOtaDeadline() {
  otaMessage("OTA cycle timed out");
  reportOtaFailure(OtaPhase::Deadline, ESP_ERR_TIMEOUT, otaElapsed());
}

void restartForOta() {
  otaMessage("Firmware verified; restarting");
  vTaskDelay(pdMS_TO_TICKS(300));
  esp_restart();
}

void installFirmware() {
  Download download(model.otaDeadline);
  const auto fail = [&](const char *status, int32_t reason) {
    otaMessage(status);
    reportOtaFailure(OtaPhase::Image, reason, otaElapsed(), download.retries());
  };
  if (otaExpired()) return reportOtaDeadline();
  // The preview worker has drained. Free HTTPD's memory and sockets before
  // allocating TLS. The owner restores HTTPD after the renderer release ACK.
  model.otaDone = 0;
  model.otaTotal = model.manifest.size;
  if (model.http) {
    const esp_err_t result = httpd_stop(model.http);
    if (result != ESP_OK) return fail("HTTP server could not drain before OTA", result);
    model.http = nullptr;
    model.httpReady = false;
    model.httpRetryAt = uptimeMs();
  }
  if (!download.open(kFirmwareUrl, model.manifest.version)) {
    if (otaExpired()) return reportOtaDeadline();
    return fail("Firmware HTTPS request failed", download.reason());
  }
  if (download.length() >= 0 && uint64_t(download.length()) != model.manifest.size)
    return fail("Firmware length mismatch", ESP_ERR_INVALID_SIZE);
  std::array<uint8_t, 4096> buffer{};
  constexpr size_t prefixSize = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);
  size_t first = 0;
  while (first < prefixSize) {
    const int count = download.read(buffer.data() + first, prefixSize - first);
    if (otaExpired()) return reportOtaDeadline();
    if (count <= 0) return fail("Firmware header incomplete", count ? count : ESP_ERR_INVALID_SIZE);
    first += size_t(count);
  }
  if (!imageDescriptor(buffer.data(), first))
    return fail("Application descriptor or S3 chip mismatch", ESP_ERR_INVALID_RESPONSE);
  const esp_partition_t *partition = esp_ota_get_next_update_partition(nullptr);
  if (!partition || model.manifest.size > partition->size)
    return fail("OTA partition too small", ESP_ERR_INVALID_SIZE);
  struct Hash {
    psa_hash_operation_t operation = PSA_HASH_OPERATION_INIT;
    ~Hash() { psa_hash_abort(&operation); }
  } hash;
  psa_status_t hashResult = psa_crypto_init();
  if (hashResult == PSA_SUCCESS) hashResult = psa_hash_setup(&hash.operation, PSA_ALG_SHA_256);
  if (hashResult != PSA_SUCCESS) return fail("SHA-256 unavailable", hashResult);
  struct OtaHandle {
    esp_ota_handle_t handle = 0;
    ~OtaHandle() {
      // Pinned ESP-IDF 6.0.1 only unlinks and frees RAM in esp_ota_abort.
      // This also covers begin returning an erase error after assigning a handle.
      if (handle) {
        const esp_err_t result = esp_ota_abort(handle);
        if (result != ESP_OK) ESP_LOGE(kTag, "OTA handle cleanup failed: %s", esp_err_to_name(result));
      }
    }
  } image;
  if (otaExpired()) return reportOtaDeadline();
  if (!requestOtaVisual(0)) return;
  if (otaExpired()) return reportOtaDeadline();
  esp_err_t result = esp_ota_begin(partition, model.manifest.size, &image.handle);
  if (result != ESP_OK) return fail("OTA partition initialization failed", result);
  size_t count = first;
  while (model.otaDone < model.otaTotal) {
    if (count > model.otaTotal - model.otaDone) return fail("Firmware length mismatch", ESP_ERR_INVALID_SIZE);
    hashResult = psa_hash_update(&hash.operation, buffer.data(), count);
    if (hashResult != PSA_SUCCESS) return fail("Firmware SHA-256 update failed", hashResult);
    if (otaExpired()) return reportOtaDeadline();
    result = esp_ota_write(image.handle, buffer.data(), count);
    if (result != ESP_OK) return fail("Firmware flash write failed", result);
    model.otaDone += count; publishView();
    if (otaExpired()) return reportOtaDeadline();
    if (model.otaDone == model.otaTotal) break;
    const uint8_t percent = uint8_t(std::min(uint64_t(99), uint64_t(model.otaDone) * 100 / model.otaTotal));
    beginOtaVisualAnimation(percent);
    const int read = download.read(buffer.data(), std::min(buffer.size(), size_t(model.otaTotal - model.otaDone)));
    // Always revoke animation, including on timeout or EOF, before flash or release.
    if (!requestOtaVisual(percent)) return;
    if (otaExpired()) return reportOtaDeadline();
    if (read <= 0) return fail("Firmware transfer incomplete", read ? read : ESP_ERR_INVALID_SIZE);
    count = size_t(read); vTaskDelay(1);
  }
  if (otaExpired()) return reportOtaDeadline();
  beginOtaVisualAnimation(99);
  const int extra = download.read(buffer.data(), 1);
  if (!requestOtaVisual(99)) return;
  if (otaExpired()) return reportOtaDeadline();
  if (extra != 0 || !download.complete())
    return fail("Firmware transfer length or framing invalid", extra < 0 ? extra : ESP_ERR_INVALID_SIZE);
  download.close();
  uint8_t digest[32]; size_t digestSize = 0;
  hashResult = psa_hash_finish(&hash.operation, digest, sizeof(digest), &digestSize);
  if (hashResult != PSA_SUCCESS || digestSize != sizeof(digest) || std::memcmp(digest, model.manifest.sha, sizeof(digest)))
    return fail("Firmware SHA-256 verification failed", hashResult == PSA_SUCCESS ? ESP_ERR_INVALID_CRC : hashResult);
  // esp_ota_end performs the SDK's complete image/chip/signature checks.
  if (otaExpired()) return reportOtaDeadline();
  result = esp_ota_end(image.handle);
  image.handle = 0; // esp_ota_end consumes the handle on either outcome.
  if (result != ESP_OK) return fail("SDK image validation failed", result);
  if (otaExpired()) return reportOtaDeadline();
  esp_app_desc_t description{};
  result = esp_ota_get_partition_description(partition, &description);
  if (result != ESP_OK) return fail("Boot partition descriptor read failed", result);
  if (std::strncmp(description.project_name, "led_forge_s3", sizeof(description.project_name)))
    return fail("Boot partition project mismatch", ESP_ERR_INVALID_RESPONSE);
  if (otaExpired()) return reportOtaDeadline();
  result = esp_ota_set_boot_partition(partition);
  if (result != ESP_OK) return fail("Boot partition selection failed", result);
  model.otaBootSelected = true;
  if (requestOtaVisual(100)) restartForOta();
}

void requestMaintenance(Maintenance kind) {
  // Reuse the network task's protection, or acquire it for a direct request.
  RecoveryOwner recoveryOwner(true); if (!recoveryOwner) return;
  if (model.releasePending || model.control.maintenance != Maintenance::None) return;
  if (kind == Maintenance::Persist) {
    if (!web_auth::tryMaintenance()) return;
  } else web_auth::setMaintenance(true);
  previewStreamSealed.store(true);
  model.control.otaVisualSequence = 0; model.control.otaVisualPercent = 0;
  model.control.otaVisualAnimate = false;
  model.otaBootSelected = false;
  model.control.maintenance = kind; ++model.control.maintenanceEpoch;
  if (!model.control.maintenanceEpoch) ++model.control.maintenanceEpoch;
  model.maintenanceAt = uptimeMs(); updateControl(); publishView();
}
void releaseMaintenance() {
  if (model.otaVisualTimedOut) return;
  model.control.otaVisualSequence = 0; model.control.otaVisualPercent = 0;
  model.control.otaVisualAnimate = false;
  model.control.maintenance = Maintenance::None;
  // Keep the old epoch until Core 1 explicitly reports that it resumed.
  model.releasePending = true; model.releaseAt = uptimeMs(); updateControl(); publishView();
  previewStreamSealed.store(false);
  web_auth::setMaintenance(false);
}
bool acknowledged() {
  return model.render.quiescent &&
      model.render.maintenanceEpoch == model.control.maintenanceEpoch;
}
void refreshStationAfterSave() {
  if (!model.wifiChanged) return;
  model.wifiChanged = false;
  model.wifiRecovery.manual(uptimeMs());
  recoveryManualAt.store(uptimeMs()); recoveryManualGrace.store(true);
  esp_wifi_disconnect(); model.connected = false; model.stationConnecting = false; model.stationAttempts = 0;
  copy(model.control.ip, "192.168.4.1");
  if (!stationConfig()) message("Wi-Fi settings saved; radio configuration failed.");
  model.stationRetryAt = uptimeMs() + 500;
}
bool commandsSealedForOta() {
  if (xSemaphoreTake(viewMutex, pdMS_TO_TICKS(20)) != pdTRUE) return false;
  sharedView.control.maintenance = Maintenance::Ota;
  const bool pending = uxQueueMessagesWaiting(commandQueue) != 0;
  xSemaphoreGive(viewMutex);
  return !pending;
}
void discardBlockedTelemetry() {
  TelemetryRequest request;
  for (unsigned i = 0; i < 4 && xQueueReceive(telemetryQueue, &request, 0) == pdTRUE; ++i) {
    TelemetryReply reply{}; reply.id = request.id;
    xQueueSend(telemetryReplies, &reply, 0);
  }
  if (model.udp < 0) return;
  char bytes[161];
  // Never stamp datagrams buffered during a long flash transfer as fresh activity.
  for (unsigned i = 0; i < 64; ++i) if (recv(model.udp, bytes, sizeof(bytes), MSG_DONTWAIT) < 0) return;
  // A sustained flood cannot prolong the handoff; recreating the socket drops its queue.
  close(model.udp); model.udp = -1; initializeUdp();
}
bool bootConfirmationHealthy() {
  return model.nvsReady && std::strcmp(webFilesVersion(), "unavailable") &&
      model.wifiReady && model.httpReady && model.udp >= 0 && model.render.watchdogReady &&
      model.render.assetsReady && model.render.frames && model.render.ledError == 0;
}
void rejectPendingBoot(const char *status, esp_err_t reason) {
  otaMessage(status);
  reportOtaFailure(OtaPhase::BootConfirmation, reason,
                   uptimeMs() - (model.confirmationDeadline - kBootConfirmationMs));
  ESP_LOGE(kTag, "Pending OTA boot rejected: nvs=%d wifi=%d http=%d udp=%d watchdog=%d assets=%d frames=%lu led_error=%ld",
           model.nvsReady, model.wifiReady, model.httpReady, model.udp >= 0,
           model.render.watchdogReady, model.render.assetsReady,
           (unsigned long)model.render.frames, (long)model.render.ledError);
  // On reset, the rollback-enabled bootloader expires PENDING_VERIFY. This
  // path never writes OTA metadata while renderer quiescence is unknown.
  esp_restart();
}

void serviceMaintenance() {
  RecoveryOwner recoveryOwner; if (!recoveryOwner) return;
  const uint32_t now = uptimeMs();
  if (model.confirmationPending && due(now, model.confirmationDeadline)) {
    rejectPendingBoot("OTA boot confirmation timed out; restarting for rollback", ESP_ERR_TIMEOUT);
    return;
  }
  if (model.otaVisualTimedOut) {
    if (!otaVisualAcknowledged()) {
      if (!due(now, model.otaRecoveryAt)) return;
      reportOtaFailure(OtaPhase::Recovery, model.render.ledError ? model.render.ledError : ESP_ERR_TIMEOUT,
                       otaElapsed());
      otaMessage(model.otaBootSelected ? "OTA completion frame unavailable; booting verified image" :
                                        "OTA renderer failed to stop; restarting current firmware");
      // The install stack already released the handle. Keep the renderer
      // sealed and reset without further flash calls or issuing a release.
      esp_restart();
      return;
    }
    if (model.otaBootSelected) { restartForOta(); return; }
    model.otaVisualTimedOut = false;
    otaMessage("OTA cancelled after renderer acknowledged stop");
    model.manifestReady = false; model.otaAt = uptimeMs() + 60000;
    releaseMaintenance(); return;
  }
  if (model.releasePending) {
    if (!model.render.quiescent && model.render.maintenanceEpoch == model.control.maintenanceEpoch) {
      model.releasePending = false;
    } else {
      if (now - model.releaseAt >= kMaintenanceTimeout) {
        ++model.maintenanceTimeouts;
        diagnostic_log::Snapshot failure;
        failure.elapsed_ms = now - model.releaseAt;
        failure.reason = model.render.ledError ? model.render.ledError : ESP_ERR_TIMEOUT;
        diagnostics::event(diagnostics::Level::Error, diagnostics::Module::System, uint16_t(0x0102), failure);
        ESP_LOGE(kTag, "Renderer did not acknowledge maintenance release; restarting");
        esp_restart();
      }
      return;
    }
  }
  if (model.recoveryRestart && (model.connected || model.memorial.active ||
      model.control.memorialActive || model.render.memorialTransition || model.wifiChanged)) {
    model.restarting = model.recoveryRestart = false;
    if (model.control.maintenance == Maintenance::Restart) releaseMaintenance();
    return;
  }
  if (model.manifestReady && due(now, model.otaDeadline)) {
    reportOtaDeadline();
    model.manifestReady = false; model.otaAt = now + 60000;
    if (model.control.maintenance == Maintenance::Ota) releaseMaintenance();
    return;
  }
  if (model.control.maintenance != Maintenance::None) {
    if (!acknowledged() || previewStreamBusy.load()) {
      if (now - model.maintenanceAt >= kMaintenanceTimeout) {
        ++model.maintenanceTimeouts; message("Renderer/preview maintenance was not acknowledged; no flash write occurred.");
        diagnostic_log::Snapshot failure; failure.elapsed_ms = now - model.maintenanceAt;
        diagnostics::event(diagnostics::Level::Warning, diagnostics::Module::System, uint16_t(0x0102), failure);
        model.persistRetryAt = now + 5000; model.otaAt = now + 60000;
        model.manifestReady = false;
        if (model.control.maintenance == Maintenance::Restart) model.restarting = false;
        releaseMaintenance();
      }
      return;
    }
    switch (model.control.maintenance) {
      case Maintenance::Persist: {
        bool saved = !model.dirty || saveSettings();
        if (saved) refreshStationAfterSave();
        saved = saveWeekly() && saved;
        saved = daily_runtime::persist() && saved;
        saved = saveMemorialSettings() && saved;
        saved = saveAirAlertSettings() && saved;
        saved = saveExternalLight() && saved;
        saved = saveBuzzerVolume() && saved;
        saved = saveSkyColors() && saved;
        saved = web_auth::persist() && saved;
        if (diagnostic_persistence::due(now, model.restarting || model.manifestReady))
          diagnostic_persistence::flush(now);
        if (!saved) diagnostics::event(diagnostics::Level::Error, diagnostics::Module::System, uint16_t(0x0101));
        if (model.confirmationPending && due(uptimeMs(), model.confirmationDeadline)) {
          rejectPendingBoot("OTA boot confirmation timed out; restarting for rollback", ESP_ERR_TIMEOUT);
          return;
        }
        if (model.confirmationPending && saved && bootConfirmationHealthy()) {
          const esp_err_t result = esp_ota_mark_app_valid_cancel_rollback();
          if (result == ESP_OK) {
            model.confirmationPending = false;
            otaMessage("OTA boot confirmed");
          } else {
            message("Boot confirmation could not be persisted.");
            reportOtaFailure(OtaPhase::BootConfirmation, result,
                             uptimeMs() - (model.confirmationDeadline - kBootConfirmationMs));
            saved = false;
          }
        }
        model.persistRetryAt = uptimeMs() + (saved ? 0 : 5000); releaseMaintenance(); break;
      }
      case Maintenance::Ota:
        if (model.dirty || model.weeklyDirty || model.lifetimeDirty || daily_runtime::dirty() || model.memorialDirty || model.airDirty || model.lightDirty || model.buzzerDirty || model.skyColorsDirty || web_auth::pendingSave() || web_auth::busy() || model.restarting || model.control.memorialActive || model.render.memorialTransition) { releaseMaintenance(); break; }
        if (!model.render.assetsReady || model.render.ledError) {
          otaMessage("OTA refused: renderer unhealthy"); model.manifestReady = false; releaseMaintenance(); break;
        }
        // Let active handlers finish before calling the synchronous HTTPD stop.
        if (!commandsSealedForOta() || activeHandlers.load()) {
          if (now - model.maintenanceAt >= kMaintenanceTimeout) {
            otaMessage("OTA cancelled: commands or HTTP requests did not drain");
            reportOtaFailure(OtaPhase::Image, ESP_ERR_TIMEOUT, otaElapsed());
            model.manifestReady = false; model.otaAt = now + 60000;
            releaseMaintenance();
          }
          return;
        }
        installFirmware();
        discardBlockedTelemetry();
        if (model.otaVisualTimedOut) return;
        model.manifestReady = false; model.otaAt = uptimeMs() + 60000; releaseMaintenance(); break;
      case Maintenance::Restart:
        if (model.dirty || model.weeklyDirty || model.lifetimeDirty || daily_runtime::dirty() || model.memorialDirty || model.airDirty || model.lightDirty || model.buzzerDirty || model.skyColorsDirty || web_auth::pendingSave()) { releaseMaintenance(); break; }
        if (!activeHandlers.load() && !web_auth::busy() && !uxQueueMessagesWaiting(commandQueue)) esp_restart();
        else if (now - model.maintenanceAt >= kMaintenanceTimeout) {
          model.restarting = false; ++model.maintenanceTimeouts;
          message("Restart cancelled: HTTP requests did not drain. Please retry.");
          releaseMaintenance();
        }
        break;
      case Maintenance::None: break;
    }
    return;
  }
  if (model.restarting && activeHandlers.load() && now - model.restartAt >= kMaintenanceTimeout) {
    model.restarting = false; ++model.maintenanceTimeouts;
    message("Restart cancelled: HTTP requests remained busy. Please retry.");
    releaseMaintenance();
    return;
  }
  if (air_alert::busy()) return;
  if ((model.dirty && now - model.dirtyAt >= 300 && due(now, model.persistRetryAt)) ||
      (web_auth::pendingSave() && due(now, model.persistRetryAt)) ||
      (diagnostic_persistence::due(now, model.restarting || model.manifestReady) && due(now, model.persistRetryAt)) ||
      (daily_runtime::due(now, model.restarting || model.manifestReady) && due(now, model.persistRetryAt)) ||
      (model.memorialDirty && due(now, model.persistRetryAt)) ||
      (model.skyColorsDirty && due(now, model.persistRetryAt)) ||
      (model.airDirty && due(now, model.persistRetryAt)) ||
      (model.lightDirty && due(now, model.persistRetryAt)) ||
      (model.buzzerDirty && now - model.buzzerDirtyAt >= 300 && due(now, model.persistRetryAt)) ||
      ((model.weeklyDirty || model.lifetimeDirty) && (now - model.weeklySavedAt >= 300000 || model.restarting || model.manifestReady) && due(now, model.persistRetryAt)) ||
      (model.confirmationPending && bootConfirmationHealthy() && due(now, model.persistRetryAt))) {
    requestMaintenance(Maintenance::Persist); return;
  }
  if (model.confirmationPending) return;
  if (model.restarting) {
    if (!model.dirty && !model.weeklyDirty && !model.lifetimeDirty && !daily_runtime::dirty() && !model.memorialDirty && !model.airDirty && !model.lightDirty && !model.buzzerDirty && !model.skyColorsDirty && !web_auth::pendingSave() && !web_auth::busy() && now - model.restartAt >= 1500 && !activeHandlers.load()) requestMaintenance(Maintenance::Restart);
    return;
  }
  if (model.manifestReady && !model.dirty && !model.weeklyDirty && !model.lifetimeDirty && !daily_runtime::dirty() && !model.memorialDirty && !model.airDirty && !model.lightDirty && !model.buzzerDirty && !model.skyColorsDirty && !web_auth::pendingSave() && !web_auth::busy() && !model.control.memorialActive && !model.render.memorialTransition) { requestMaintenance(Maintenance::Ota); return; }
  if (!model.connected || model.dirty || !model.render.assetsReady || model.render.ledError) return;
  if (due(now, model.otaAt) && (model.control.on || model.updateRequested)) queueManifestCheck();
}
} // namespace
