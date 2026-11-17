// Private implementation unit; included only by network.cpp.
struct HttpHeaders { char location[1024] = {}; bool encoded = false; };
esp_err_t httpEvent(esp_http_client_event_t *event) {
  if (event->event_id != HTTP_EVENT_ON_HEADER || !event->user_data) return ESP_OK;
  auto &headers = *static_cast<HttpHeaders *>(event->user_data);
  if (!strcasecmp(event->header_key, "Location")) copy(headers.location, event->header_value);
  if (!strcasecmp(event->header_key, "Content-Encoding") && strcasecmp(event->header_value, "identity")) headers.encoded = true;
  return ESP_OK;
}
const char *directGitHubUrl(const char *source) {
  // Keep the published API URL compatible with older installed firmware.
  if (!std::strcmp(source, kFirmwareUrl))
    return "https://raw.githubusercontent.com/Vihten/CODEX_firmware_esp8266/main/esp32-s3/firmware.bin";
  if (!std::strcmp(source, "https://api.github.com/repos/Vihten/CODEX_firmware_esp8266/contents/esp32-s3/manifest.json?ref=main"))
    return "https://raw.githubusercontent.com/Vihten/CODEX_firmware_esp8266/main/esp32-s3/manifest.json";
  return source;
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
public:
  ~Download() { close(); }
  void close() { if (client_) { esp_http_client_close(client_); esp_http_client_cleanup(client_); client_ = nullptr; } }
  bool open(const char *source) {
    char url[1024]; copy(url, directGitHubUrl(source));
    for (unsigned redirects = 0; redirects <= 4; ++redirects) {
      if (!githubHttps(url)) return false;
      headers_ = {};
      esp_http_client_config_t config{}; config.url = url; config.crt_bundle_attach = esp_crt_bundle_attach;
      config.timeout_ms = 8000; config.buffer_size = 4096; config.buffer_size_tx = 1024;
      config.disable_auto_redirect = true; config.event_handler = httpEvent; config.user_data = &headers_;
      client_ = esp_http_client_init(&config); if (!client_) return false;
      esp_http_client_set_header(client_, "Accept", "application/vnd.github.raw+json");
      esp_http_client_set_header(client_, "Accept-Encoding", "identity");
      esp_http_client_set_header(client_, "Cache-Control", "no-cache");
      esp_http_client_set_header(client_, "User-Agent", "LED-Forge-ESP32-S3");
      if (esp_http_client_open(client_, 0) != ESP_OK) return false;
      const int64_t length = esp_http_client_fetch_headers(client_);
      if (length < 0 && !esp_http_client_is_chunked_response(client_)) return false;
      const int status = esp_http_client_get_status_code(client_);
      if (status == 200) return !headers_.encoded;
      if (status != 301 && status != 302 && status != 303 && status != 307 && status != 308) return false;
      if (!headers_.location[0] || !githubHttps(headers_.location)) return false;
      copy(url, headers_.location); close();
    }
    return false;
  }
  int read(void *data, size_t size) { return esp_http_client_read(client_, static_cast<char *>(data), int(size)); }
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
  const auto fail = [&](const char *status) { copy(result.status, status); return result; };
  Download download;
  if (!download.open(job.url)) return fail("Manifest HTTPS request failed");
  if (download.length() > 4096) return fail("Manifest exceeds 4096 bytes");
  char body[4097]; size_t used = 0; const uint32_t started = uptimeMs();
  for (;;) {
    if (uptimeMs() - started > 30000) return fail("Manifest timed out");
    const int count = download.read(body + used, sizeof(body) - 1 - used);
    if (count < 0) return fail("Manifest read failed");
    if (!count) break;
    if (std::memchr(body + used, 0, count)) return fail("Manifest contains binary data");
    used += size_t(count);
    if (used == sizeof(body) - 1) return fail("Manifest too large");
  }
  if (!download.complete()) return fail("Manifest incomplete");
  // The body is complete and local: release TLS/socket allocations before
  // allocating the JSON tree. close() is idempotent with the RAII destructor.
  download.close();
  body[used] = 0;
  const char *end = nullptr; cJSON *json = cJSON_ParseWithLengthOpts(body, used + 1, &end, true);
  if (!json) return fail("Manifest JSON invalid");
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
  if (!valid) return fail("Manifest does not match this S3 image");
  if (version <= job.currentVersion) { result.upToDate = true; return fail("Up to date"); }
  manifest.version = version; manifest.size = size;
  result.manifest = manifest; result.newer = true;
  return fail("New firmware verified; waiting for LEDs");
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
  if (!result.newer && !result.upToDate) {
    diagnostic_log::Snapshot failure; failure.detail = 1; // Manifest phase.
    diagnostics::event(diagnostics::Level::Warning, diagnostics::Module::Ota,
                       diagnostics::Code::UpdateFailed, failure);
  }
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
  if (xQueueSend(manifestJobs, &job, 0) != pdTRUE) { otaMessage("Manifest worker busy"); return; }
  model.manifestBusy = true; model.updateRequested = false; model.otaAt = uptimeMs() + kOtaInterval;
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
    if (elapsed >= 1000) break;
    RenderStatus render;
    const uint32_t remaining = 1000 - elapsed;
    if (xQueueReceive(renderStatusQueue, &render, pdMS_TO_TICKS(std::min(remaining, uint32_t(10)))) == pdTRUE) {
      model.render = render;
      if (otaVisualAcknowledged()) return true;
    }
    // Yield even when stale status reports keep the queue readable (idle watchdog).
    if (uptimeMs() - started < 1000) vTaskDelay(1);
  }
  // A delayed request can still start DMA. Keep maintenance sealed until its ACK;
  // neither abort nor release/restart is safe merely because a timer expired.
  model.otaVisualTimedOut = true;
  ++model.maintenanceTimeouts;
  otaMessage("OTA visual ACK timed out; flash cleanup waiting for renderer");
  return false;
}
bool installFirmware() {
  // Maintenance already sealed and drained the asynchronous preview worker.
  // Stop HTTPD from its network owner, releasing its task and all client sockets
  // before HTTPS allocates buffers. Recreate routes only after HTTPS is destroyed.
  struct HttpPause {
    bool stopped = false;
    ~HttpPause() {
      if (stopped) model.httpReady = initializeHttp();
    }
  } httpPause;
  if (model.http) {
    if (httpd_stop(model.http) != ESP_OK) {
      otaMessage("HTTP server could not drain before OTA");
      return false;
    }
    model.http = nullptr;
    model.httpReady = false;
    httpPause.stopped = true;
  }
  Download download;
  if (!download.open(kFirmwareUrl)) { otaMessage("Firmware HTTPS request failed"); return false; }
  if (download.length() >= 0 && uint64_t(download.length()) != model.manifest.size) { otaMessage("Firmware length mismatch"); return false; }
  std::array<uint8_t, 4096> buffer{};
  constexpr size_t prefixSize = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);
  size_t first = 0;
  while (first < prefixSize) {
    const int count = download.read(buffer.data() + first, prefixSize - first);
    if (count <= 0) { otaMessage("Firmware header incomplete"); return false; }
    first += size_t(count);
  }
  if (!imageDescriptor(buffer.data(), first)) { otaMessage("Application descriptor or S3 chip mismatch"); return false; }
  const esp_partition_t *partition = esp_ota_get_next_update_partition(nullptr);
  if (!partition || model.manifest.size > partition->size) { otaMessage("OTA partition too small"); return false; }
  psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
  if (psa_crypto_init() != PSA_SUCCESS || psa_hash_setup(&hash, PSA_ALG_SHA_256) != PSA_SUCCESS) { otaMessage("SHA-256 unavailable"); return false; }
  esp_ota_handle_t handle = 0;
  model.otaDone = 0; model.otaTotal = model.manifest.size;
  if (!requestOtaVisual(0)) { psa_hash_abort(&hash); return false; }
  if (esp_ota_begin(partition, model.manifest.size, &handle) != ESP_OK) { psa_hash_abort(&hash); otaMessage("OTA partition initialization failed"); return false; }
  const uint32_t started = uptimeMs(); bool okay = true; size_t count = first;
  while (model.otaDone < model.otaTotal) {
    if (uptimeMs() - started > 180000 || count > model.otaTotal - model.otaDone ||
        psa_hash_update(&hash, buffer.data(), count) != PSA_SUCCESS || esp_ota_write(handle, buffer.data(), count) != ESP_OK) { okay = false; break; }
    model.otaDone += count; publishView();
    if (model.otaDone == model.otaTotal) break;
    const uint8_t percent = uint8_t(std::min(uint64_t(99), uint64_t(model.otaDone) * 100 / model.otaTotal));
    beginOtaVisualAnimation(percent);
    const int read = download.read(buffer.data(), std::min(buffer.size(), size_t(model.otaTotal - model.otaDone)));
    // Stop even on EOF/error, before any subsequent write, abort or validation.
    if (!requestOtaVisual(percent)) {
      psa_hash_abort(&hash); model.otaAbortHandle = handle; return false;
    }
    if (read <= 0) { okay = false; break; }
    count = size_t(read); vTaskDelay(1);
  }
  if (okay) {
    beginOtaVisualAnimation(99);
    const int extra = download.read(buffer.data(), 1);
    if (!requestOtaVisual(99)) {
      psa_hash_abort(&hash); model.otaAbortHandle = handle; return false;
    }
    if (extra != 0 || !download.complete()) okay = false;
  }
  uint8_t digest[32]; size_t digestSize = 0;
  if (psa_hash_finish(&hash, digest, sizeof(digest), &digestSize) != PSA_SUCCESS || digestSize != 32 || std::memcmp(digest, model.manifest.sha, 32)) okay = false;
  psa_hash_abort(&hash);
  if (!okay) { esp_ota_abort(handle); otaMessage("Firmware transfer or raw SHA-256 verification failed"); return false; }
  // esp_ota_end performs the SDK's complete image/chip/signature checks.
  if (esp_ota_end(handle) != ESP_OK) { otaMessage("SDK image validation failed"); return false; }
  esp_app_desc_t description{};
  if (esp_ota_get_partition_description(partition, &description) != ESP_OK ||
      std::strcmp(description.project_name, "led_forge_s3") || esp_ota_set_boot_partition(partition) != ESP_OK) {
    otaMessage("Boot partition validation failed"); return false;
  }
  model.otaBootSelected = true;
  if (!requestOtaVisual(100)) return false;
  otaMessage("Firmware verified; restarting");
  vTaskDelay(pdMS_TO_TICKS(300));
  esp_restart(); return true;
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
  model.releasePending = true; updateControl(); publishView();
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
void serviceMaintenance() {
  RecoveryOwner recoveryOwner; if (!recoveryOwner) return;
  const uint32_t now = uptimeMs();
  if (model.recoveryRestart && (model.connected || model.memorial.active ||
      model.control.memorialActive || model.render.memorialTransition || model.wifiChanged)) {
    model.restarting = model.recoveryRestart = false;
    if (model.control.maintenance == Maintenance::Restart) releaseMaintenance();
    return;
  }
  if (model.otaVisualTimedOut) {
    if (!otaVisualAcknowledged()) return;
    if (model.otaBootSelected) {
      otaMessage("Firmware verified; restarting");
      vTaskDelay(pdMS_TO_TICKS(300)); esp_restart();
      return;
    }
    model.otaVisualTimedOut = false;
    if (model.otaAbortHandle) {
      esp_ota_abort(model.otaAbortHandle);
      otaMessage("OTA cancelled after renderer became quiescent");
    }
    model.otaAbortHandle = 0;
    model.manifestReady = false; model.otaAt = uptimeMs() + 60000;
    releaseMaintenance(); return;
  }
  if (model.releasePending) {
    if (!model.render.quiescent && model.render.maintenanceEpoch == model.control.maintenanceEpoch) model.releasePending = false;
    else return;
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
        if (model.confirmationPending && std::strcmp(webFilesVersion(), "unavailable") && model.render.watchdogReady && model.render.assetsReady && model.render.frames && model.render.ledError == 0 && model.wifiReady && model.httpReady && model.udp >= 0) {
          if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) model.confirmationPending = false;
          else { message("Boot confirmation could not be persisted."); saved = false; }
        }
        if (!saved) diagnostics::event(diagnostics::Level::Error, diagnostics::Module::System, uint16_t(0x0101));
        model.persistRetryAt = now + (saved ? 0 : 5000); releaseMaintenance(); break;
      }
      case Maintenance::Ota:
        if (model.dirty || model.weeklyDirty || model.lifetimeDirty || daily_runtime::dirty() || model.memorialDirty || model.airDirty || model.lightDirty || model.buzzerDirty || model.skyColorsDirty || web_auth::pendingSave() || web_auth::busy() || model.restarting || model.control.memorialActive || model.render.memorialTransition) { releaseMaintenance(); break; }
        if (!model.render.assetsReady || model.render.ledError) {
          otaMessage("OTA refused: renderer unhealthy"); model.manifestReady = false; releaseMaintenance(); break;
        }
        // Finish the command gate atomically; HTTP views and telemetry never block OTA.
        if (!commandsSealedForOta()) return;
        if (installFirmware()) return;
        { diagnostic_log::Snapshot failure; failure.detail = 2; // Image transfer/validation phase.
          diagnostics::event(diagnostics::Level::Error, diagnostics::Module::Ota, diagnostics::Code::UpdateFailed, failure); }
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
      (model.confirmationPending && std::strcmp(webFilesVersion(), "unavailable") && model.wifiReady && model.httpReady && model.udp >= 0 &&
       model.render.watchdogReady && model.render.assetsReady && model.render.frames && !model.render.ledError && due(now, model.persistRetryAt))) {
    requestMaintenance(Maintenance::Persist); return;
  }
  if (model.restarting) {
    if (!model.dirty && !model.weeklyDirty && !model.lifetimeDirty && !daily_runtime::dirty() && !model.memorialDirty && !model.airDirty && !model.lightDirty && !model.buzzerDirty && !model.skyColorsDirty && !web_auth::pendingSave() && !web_auth::busy() && now - model.restartAt >= 1500 && !activeHandlers.load()) requestMaintenance(Maintenance::Restart);
    return;
  }
  if (model.manifestReady && !model.dirty && !model.weeklyDirty && !model.lifetimeDirty && !daily_runtime::dirty() && !model.memorialDirty && !model.airDirty && !model.lightDirty && !model.buzzerDirty && !model.skyColorsDirty && !web_auth::pendingSave() && !web_auth::busy() && !model.control.memorialActive && !model.render.memorialTransition) { requestMaintenance(Maintenance::Ota); return; }
  if (!model.connected || model.dirty || model.confirmationPending || !model.render.assetsReady || model.render.ledError) return;
  if (due(now, model.otaAt) && (model.control.on || model.updateRequested)) queueManifestCheck();
}
} // namespace
