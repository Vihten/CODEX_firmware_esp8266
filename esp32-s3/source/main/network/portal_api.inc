// Private implementation unit; included only by network.cpp.
esp_err_t frameGet(httpd_req_t *request) {
  // Frame packets must not wait for a delayed ACK between headers and body.
  const int noDelay = 1;
  const int socket = httpd_req_to_sockfd(request);
  if (socket >= 0) (void)setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &noDelay, sizeof(noDelay));
  HandlerScope scope; View view;
  if (!snapshot(view) || view.restarting || view.control.maintenance != Maintenance::None ||
      !view.render.assetsReady || view.render.ledError)
    return error(request, "503 Service Unavailable", "Preview unavailable during maintenance.");
  uint8_t bytes[kPreviewFrameBytes];
  bool copied = copyPreview(bytes, sizeof(bytes), uptimeMs());
  // Cold leases need one renderer tick. Yield without holding the snapshot lock;
  // otherwise client retry backoff can outlast the lease and stay cold forever.
  for (unsigned attempt = 0; !copied && attempt < 6; ++attempt) {
    vTaskDelay(pdMS_TO_TICKS(10));
    copied = copyPreview(bytes, sizeof(bytes), uptimeMs());
  }
  if (!copied) {
    httpd_resp_set_hdr(request, "Retry-After", "1");
    return error(request, "503 Service Unavailable", "Waiting for an LED frame.");
  }
  httpd_resp_set_type(request, "application/octet-stream");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
  return httpd_resp_send(request, reinterpret_cast<const char *>(bytes), sizeof(bytes));
}

std::atomic<bool> previewStreamBusy{false};
std::atomic<bool> previewStreamSealed{false};
std::atomic<uint32_t> previewStreamFrames{0}, previewStreamStack{0};
std::atomic<uint32_t> previewStreamSendErrors{0};
StaticQueue_t previewStreamQueueStorage;
uint8_t previewStreamQueueBytes[sizeof(httpd_req_t *)];
QueueHandle_t previewStreamQueue = nullptr;
std::atomic<bool> previewStreamReady{false};
std::atomic<uint32_t> previewSessionRevision{0};
char previewSessionToken[65] = {};

bool previewStreamAllowed() {
  if (previewStreamSealed.load() || !viewMutex || xSemaphoreTake(viewMutex, pdMS_TO_TICKS(20)) != pdTRUE) return false;
  const bool allowed = !sharedView.restarting && sharedView.control.maintenance == Maintenance::None &&
      sharedView.render.assetsReady && !sharedView.render.ledError;
  xSemaphoreGive(viewMutex);
  return allowed && !previewStreamSealed.load();
}

void finishPreviewSocket(int socket, bool graceful) {
  if (socket < 0) return;
  if (graceful && shutdown(socket, SHUT_WR) == 0) {
    // Allow the terminal chunk/FIN to drain before shutting down reads. A peer
    // that never sends FIN must not retain one of the four HTTP sessions.
    const uint32_t started = uptimeMs();
    while (uint32_t(uptimeMs() - started) < 200) {
      char byte;
      const int received = recv(socket, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
      if (received >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) break;
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
  // Still own the async request here: the descriptor cannot be reused. Make
  // HTTPD observe EOF after completion; HTTPD remains the sole close() owner.
  (void)shutdown(socket, SHUT_RDWR);
}

void previewStreamTask(void *) {
  httpd_req_t *request = nullptr;
  uint8_t bytes[kPreviewFrameBytes];
  for (;;) {
    if (xQueueReceive(previewStreamQueue, &request, portMAX_DELAY) != pdTRUE) continue;
    const int socket = httpd_req_to_sockfd(request);
    const int noDelay = 1;
    const timeval timeout{1, 0};
    bool writable = socket >= 0 && setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0;
    if (writable) (void)setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &noDelay, sizeof(noDelay));
    httpd_resp_set_type(request, "application/octet-stream");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "Connection", "close");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    uint32_t lastSequence = 0;
    bool sent = false;
    const uint32_t started = uptimeMs();
    const uint32_t authRevision = previewSessionRevision.load();
    char authToken[65]; std::memcpy(authToken, previewSessionToken, sizeof(authToken));
    TickType_t wake = xTaskGetTickCount();
    while (writable && uint32_t(uptimeMs() - started) < 30000 &&
           authRevision == web_auth::sessionRevision() && web_auth::sessionActive(authToken) && previewStreamAllowed()) {
      if (copyPreview(bytes, sizeof(bytes), uptimeMs())) {
        uint32_t sequence = 0;
        for (unsigned i = 0; i < 4; ++i) sequence |= uint32_t(bytes[8 + i]) << (8 * i);
        if (!sent || sequence != lastSequence) {
          writable = httpd_resp_send_chunk(request, reinterpret_cast<const char *>(bytes), sizeof(bytes)) == ESP_OK;
          if (writable) { lastSequence = sequence; sent = true; ++previewStreamFrames; }
        }
      }
      // No view/preview mutex is held during network I/O. Never queue old frames.
      previewStreamStack.store(uxTaskGetStackHighWaterMark(nullptr), std::memory_order_relaxed);
      if (xTaskGetTickCount() - wake >= pdMS_TO_TICKS(66)) wake = xTaskGetTickCount();
      vTaskDelayUntil(&wake, pdMS_TO_TICKS(66));
    }
    if (writable && sent) writable = httpd_resp_send_chunk(request, nullptr, 0) == ESP_OK;
    if (!writable) ++previewStreamSendErrors;
    finishPreviewSocket(socket, writable && sent);
    (void)httpd_req_async_handler_complete(request);
    --activeHandlers;
    std::memset(authToken, 0, sizeof(authToken));
    std::memset(previewSessionToken, 0, sizeof(previewSessionToken));
    previewStreamBusy.store(false, std::memory_order_release);
  }
}

esp_err_t framesGet(httpd_req_t *request) {
  HandlerScope scope;
  if (!previewStreamReady.load(std::memory_order_acquire) || !previewStreamAllowed())
    return error(request, "503 Service Unavailable", "Preview stream unavailable.");
  if (previewStreamBusy.exchange(true))
    return error(request, "409 Conflict", "Preview stream is already in use.");
  if (previewStreamSealed.load()) {
    previewStreamBusy.store(false, std::memory_order_release);
    return error(request, "503 Service Unavailable", "Preview stream is draining for maintenance.");
  }
  // Revalidate after reserving the worker; it keeps its own token, not HTTP headers.
  // Capture the revision first so a concurrent password commit cannot grant a new epoch.
  const uint32_t authRevision = web_auth::sessionRevision();
  if (!web_auth::captureAuthenticatedSession(request, previewSessionToken)) {
    previewStreamBusy.store(false, std::memory_order_release);
    return error(request, "401 Unauthorized", "Потрібен вхід: /login");
  }
  previewSessionRevision.store(authRevision);
  httpd_req_t *async = nullptr;
  if (httpd_req_async_handler_begin(request, &async) != ESP_OK) {
    previewStreamBusy.store(false, std::memory_order_release);
    return error(request, "503 Service Unavailable", "Cannot start preview stream.");
  }
  ++activeHandlers;
  if (xQueueSend(previewStreamQueue, &async, 0) != pdTRUE) {
    (void)httpd_resp_set_status(async, "503 Service Unavailable");
    (void)httpd_resp_send(async, "Preview worker busy.", HTTPD_RESP_USE_STRLEN);
    (void)httpd_req_async_handler_complete(async);
    --activeHandlers;
    previewStreamBusy.store(false, std::memory_order_release);
  }
  return ESP_OK;
}
esp_err_t staticAsset(httpd_req_t *request) {
  HandlerScope scope;
  // Route registration excludes arbitrary filesystem paths; strip only URL query.
  char path[128];
  const size_t length = std::strcspn(request->uri, "?");
  if (!length || length >= sizeof(path)) return error(request, "404 Not Found", "Not found");
  std::memcpy(path, request->uri, length); path[length] = 0;
  const bool css = !std::strcmp(path, "/assets/css/portal.css") || !std::strcmp(path, "/assets/css/preview.css");
  const bool js = !std::strcmp(path, "/assets/js/portal.js") || !std::strcmp(path, "/assets/js/preview.js") || !std::strcmp(path, "/assets/js/boot.js") || !std::strcmp(path, "/assets/js/token-history.js");
  if (!css && !js) return error(request, "404 Not Found", "Not found");
  return serveWebFile(request, path + 1, css ? "text/css; charset=utf-8" : "text/javascript; charset=utf-8");
}
esp_err_t portalStatus(httpd_req_t *request) {
  HandlerScope scope; View view;
  if (!snapshot(view)) return error(request, "503 Service Unavailable", "Controller busy; retry.");
  cJSON *root = cJSON_CreateObject();
  if (!root) return error(request, "503 Service Unavailable", "Not enough memory.");
  bool okay = true;
  const auto number = [&](cJSON *object, const char *key, double value) { if (!cJSON_AddNumberToObject(object, key, value)) okay = false; };
  const auto string = [&](cJSON *object, const char *key, const char *value) { if (!cJSON_AddStringToObject(object, key, value)) okay = false; };
  const auto boolean = [&](cJSON *object, const char *key, bool value) { if (!cJSON_AddBoolToObject(object, key, value)) okay = false; };
#ifdef PROJECT_BUILD_EPOCH
  number(root, "build_epoch", double(PROJECT_BUILD_EPOCH));
#endif
  number(root, "uptime_ms", uptimeMs()); number(root, "frames", view.render.frames);
  number(root, "heap", heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  number(root, "rssi", view.rssi); number(root, "activity", view.control.activityPercent);
  number(root, "agents_count", portalAgentCount(view)); number(root, "brightness", view.control.brightness);
  number(root, "output_limit_percent", view.render.sceneVisible && !view.render.ledError ? view.render.outputLimitPercent : 100);
  number(root, "estimated_supply_ma", view.render.estimatedSupplyMa);
  cJSON *light = cJSON_AddObjectToObject(root, "external_light");
  if (!light) okay = false;
  else {
    number(light, "first", view.light.first); number(light, "second", view.light.second);
    number(light, "first_output", view.lightAllowed ? view.render.lightFirstOutput : 0);
    number(light, "second_output", view.lightAllowed ? view.render.lightSecondOutput : 0);
    boolean(light, "ready", view.lightReady); boolean(light, "allowed", view.lightAllowed);
    boolean(light, "follow_matrix", view.light.followMatrix); boolean(light, "saving", view.lightDirty);
    boolean(light, "save_failed", view.lightSaveFailed);
  }
  boolean(root, "preview_stream_busy", previewStreamBusy.load(std::memory_order_acquire));
  number(root, "preview_stream_frames", previewStreamFrames.load(std::memory_order_relaxed));
  number(root, "preview_stream_stack", previewStreamStack.load(std::memory_order_relaxed));
  number(root, "preview_stream_send_errors", previewStreamSendErrors.load(std::memory_order_relaxed));
  number(root, "ota_done", view.otaDone); number(root, "ota_total", view.otaTotal);
  const time_t epoch = std::time(nullptr); number(root, "time_epoch", epoch >= 1700000000 ? double(epoch) : 0);
  boolean(root, "standby", !view.control.on);
  boolean(root, "clock_ready", epoch >= 1700000000);
  number(root, "clock_retries", view.clockRetries);
  string(root, "quota_provider", quota_provider::name(view.control.quotaProvider));
  cJSON *quotas = cJSON_AddObjectToObject(root, "quotas");
  if (!quotas) okay = false;
  else {
    const auto addQuota = [&](const char *provider, const quota_provider::Snapshot &value) {
      cJSON *item = cJSON_AddObjectToObject(quotas, provider);
      if (!item) { okay = false; return; }
      number(item, "remaining", value.remaining);
      boolean(item, "known", value.known);
      boolean(item, "fresh", value.fresh);
      boolean(item, "stale", !value.fresh);
      if (value.resetAt) number(item, "reset_at", value.resetAt);
      else if (!cJSON_AddNullToObject(item, "reset_at")) okay = false;
      if (value.ageSeconds != UINT32_MAX) number(item, "age_seconds", value.ageSeconds);
      else if (!cJSON_AddNullToObject(item, "age_seconds")) okay = false;
    };
    addQuota("codex", view.control.codexQuota);
    addQuota("claude", view.control.claudeQuota);
  }
  boolean(root, "quota_known", view.control.quotaKnown);
  boolean(root, "quota_fresh", view.control.quotaFresh);
  boolean(root, "quota_available", view.control.quotaAvailable);
  number(root, "quota_rotation_minutes", view.control.quotaRotationMinutes);
  cJSON *skyColors = cJSON_AddObjectToObject(root, "sky_colors");
  if (!skyColors) okay = false;
  else {
    char codex[8], claude[8];
    std::snprintf(codex, sizeof(codex), "#%06lX", (unsigned long)view.control.codexSkyColor & 0xffffffUL);
    std::snprintf(claude, sizeof(claude), "#%06lX", (unsigned long)view.control.claudeSkyColor & 0xffffffUL);
    string(skyColors, "codex", codex);
    string(skyColors, "claude", claude);
  }
  if (view.control.quotaKnown) number(root, "remaining", view.control.remainingPercent);
  else if (!cJSON_AddNullToObject(root, "remaining")) okay = false;
  boolean(root, "wifi", view.control.connected); boolean(root, "on", view.control.on);
  boolean(root, "maintenance", view.restarting || view.control.maintenance == Maintenance::Ota || view.control.maintenance == Maintenance::Restart);
  boolean(root, "buzzer_ready", view.buzzerReady && view.control.maintenance == Maintenance::None);
  number(root, "buzzer_volume", view.buzzerVolume);
  boolean(root, "buzzer_saving", view.buzzerDirty); boolean(root, "buzzer_save_failed", view.buzzerSaveFailed);
  cJSON *blackHoleStatus = cJSON_AddObjectToObject(root, "black_hole");
  if (!blackHoleStatus) okay = false;
  else {
    const uint32_t elapsed = uint32_t(uptimeMs() - view.control.blackHolePreviewStartedAt);
    const bool manual = view.control.blackHolePreview && elapsed < 60000U;
    boolean(blackHoleStatus, "manual", manual);
    boolean(blackHoleStatus, "can_play", view.control.on && !view.control.memorialActive &&
        !view.render.memorialTransition && !view.restarting && view.control.maintenance == Maintenance::None);
    number(blackHoleStatus, "remaining_seconds", manual ? (60000U - elapsed + 999U) / 1000U : 0);
  }
  cJSON *memorialStatus = cJSON_AddObjectToObject(root, "memorial");
  if (!memorialStatus) okay = false;
  else {
    boolean(memorialStatus, "enabled", view.memorialEnabled);
    boolean(memorialStatus, "active", view.control.memorialActive);
    boolean(memorialStatus, "manual", view.memorialManual);
    boolean(memorialStatus, "time_ready", std::time(nullptr) >= 1700000000);
    boolean(memorialStatus, "saving", view.dirty);
    boolean(memorialStatus, "save_failed", view.memorialSaveFailed);
    const uint32_t elapsed = uptimeMs() - view.control.memorialStartedAt;
    number(memorialStatus, "remaining_seconds", view.control.memorialActive && elapsed < view.control.memorialDurationMs
        ? (view.control.memorialDurationMs - elapsed + 999) / 1000 : 0);
  }
  cJSON *airStatus = cJSON_AddObjectToObject(root, "air_alert");
  if (!airStatus) okay = false;
  else {
    boolean(airStatus, "enabled", view.airEnabled);
    string(airStatus, "provider", "telegram");
    string(airStatus, "scope", "Вінниця");
    number(airStatus, "message_id", view.airMessageId);
    boolean(airStatus, "manual", view.airManual);
    boolean(airStatus, "city_blink", view.control.airAlertBlink);
    boolean(airStatus, "city_demo", view.cityDemo);
    boolean(airStatus, "city_reported", view.cityReported);
    boolean(airStatus, "city_fresh", view.cityFresh);
    boolean(airStatus, "city_failed", view.cityFailed);
    boolean(airStatus, "fresh", view.airFresh);
    boolean(airStatus, "save_failed", view.airSaveFailed);
    string(airStatus, "level", airLevelName(view.airLevel));
    string(airStatus, "status", view.airStatus);
    string(airStatus, "source_status", view.airSource);
    string(airStatus, "details", view.airDetails);
    string(airStatus, "failure", air_alert::failureName(view.airFailure));
    number(airStatus, "http_status", view.airHttpStatus);
    number(airStatus, "poll_ms", view.airPollMs);
    number(airStatus, "tls_error", view.airTlsError);
    number(airStatus, "tls_code", view.airTlsCode);
    number(airStatus, "tls_flags", view.airTlsFlags);
    number(airStatus, "worker_stack_free", air_alert::workerStackFree());
    if (!view.airHistory.seen) {
      if (!cJSON_AddNullToObject(airStatus, "last_failure")) okay = false;
    } else {
      cJSON *last = cJSON_AddObjectToObject(airStatus, "last_failure");
      if (!last) okay = false;
      else {
        const auto &failure = view.airHistory.result;
        string(last, "failure", air_alert::failureName(failure.failure));
        number(last, "age_seconds", view.airHistory.ageSeconds(uptimeMs()));
        number(last, "http_status", failure.httpStatus);
        number(last, "tls_error", failure.tlsError); number(last, "tls_code", failure.tlsCode);
        number(last, "tls_flags", failure.tlsFlags); number(last, "poll_ms", failure.elapsedMs);
        number(last, "heap_before_tls", failure.heapBeforeTls);
        number(last, "largest_before_tls", failure.largestBeforeTls);
        number(last, "clock_before_tls", double(failure.clockBeforeTls));
        string(last, "fetch_phase", air_alert::fetchPhaseName(failure.fetchPhase));
        number(last, "page_attempt", failure.pageAttempt);
        number(last, "body_bytes", failure.bodyBytes);
        number(last, "wire_bytes", failure.wireBytes);
        number(last, "read_calls", failure.readCalls);
        number(last, "read_waits", failure.readWaits);
        number(last, "dns_ms", failure.dnsMs);
        number(last, "connect_ms", failure.connectMs);
        number(last, "write_ms", failure.writeMs);
        number(last, "read_ms", failure.readMs);
        number(last, "parse_ms", failure.parseMs);
      }
    }
    number(airStatus, "remaining_seconds", view.airRemaining);
    if (view.airAge == UINT32_MAX) { if (!cJSON_AddNullToObject(airStatus, "updated_age_seconds")) okay = false; }
    else number(airStatus, "updated_age_seconds", view.airAge);
  }
  string(root, "matrix", portalMatrixState(view)); string(root, "ota", view.ota); string(root, "ota_label", portalOtaLabel(view.ota));
  string(root, "power", view.power == 0 ? "auto" : view.power == 1 ? "on" : "off");
  char id[16], version[24]; std::snprintf(id, sizeof(id), "%06lX", (unsigned long)view.deviceId);
  std::snprintf(version, sizeof(version), "%llu", (unsigned long long)PROJECT_FIRMWARE_VERSION);
  string(root, "device_id", id); string(root, "version", version);
  string(root, "web_filesystem", "littlefs"); string(root, "web_version", webFilesVersion());
  cJSON *agents = cJSON_AddArrayToObject(root, "agents");
  if (!agents) okay = false;
  else for (const auto &agent : view.control.agents) {
    if (!agent.present && !agent.name[0]) continue;
    cJSON *item = cJSON_CreateObject();
    if (!item) { okay = false; break; }
    if (!cJSON_AddItemToArray(agents, item)) { cJSON_Delete(item); okay = false; break; }
    char client[16], color[8]; std::snprintf(client, sizeof(client), "%08lX", (unsigned long)agent.clientId);
    std::snprintf(color, sizeof(color), "#%06lX", (unsigned long)agent.rgb);
    string(item, "id", client); string(item, "name", agent.name); string(item, "color", color);
    boolean(item, "custom_color", std::any_of(std::begin(view.savedColors), std::end(view.savedColors),
        [&](const ColorOverride &entry) { return entry.used && entry.clientId == agent.clientId; }));
    boolean(item, "online", agent.present); number(item, "activity", agent.present ? agent.activity : 0);
    number(item, "remaining", agent.present ? agent.remaining : -1);
    boolean(item, "quota_source", agent.present && agent.quotaForPairedAccount);
    const auto *update = view.agentUpdates.find(agent.clientId);
    number(item, "agent_version", update ? double(update->version) : 0);
    boolean(item, "update_capable", update && view.agentUpdateKeyReady && agent_release::version &&
        uptimeMs() - update->polledAt < agent_update::freshMs);
    boolean(item, "update_pending", view.agentUpdates.target(agent.clientId, uptimeMs()) != 0);
    boolean(item, "update_failed", update && update->failed && update->failed >= agent_release::version);
    boolean(item, "update_available", update && agent_release::version > update->version && agent_release::version > update->failed);
    boolean(item, "management_capable", update && update->management && view.agentUpdateKeyReady &&
        uptimeMs() - update->polledAt < agent_update::freshMs);
    number(item, "management_state", update ? update->actionState : 0);
  }
  // Weekly ranking and calendar share daily evidence. Keep old cumulative
  // reports in NVS for diagnostics; never reinterpret them as verified day sums.
  daily::WeekSummary weekSummary;
  const auto rankingClock = memorialClock();
  const bool dailyReady = daily_runtime::weeklySummary(rankingClock.valid ? rankingClock.date : 0, weekSummary);
  weekly::Store displayWeek;
  displayWeek.week = weekSummary.week;
  if (dailyReady) for (size_t i = 0; i < daily::kAgents; ++i) {
    const auto &day = weekSummary.entries[i];
    if (!day.identity || !day.observedDays) continue;
    auto &entry = displayWeek.entries[i];
    entry.clientId = uint32_t(day.identity); entry.tokens = day.tokens;
    entry.rgb = 0xd400ff;
    std::snprintf(entry.name, sizeof(entry.name), "%08lX", (unsigned long)entry.clientId);
    for (const auto &reported : view.weekly.entries) if (reported.clientId == entry.clientId) {
      entry.rgb = reported.rgb; std::snprintf(entry.name, sizeof(entry.name), "%s", reported.name); break;
    }
    for (const auto &agent : view.control.agents) if (agent.present && agent.clientId == entry.clientId) {
      entry.rgb = agent.rgb; std::snprintf(entry.name, sizeof(entry.name), "%s", agent.name); break;
    }
  }
  for (unsigned kind = 0; kind < 2; ++kind) {
  const auto &store = kind ? view.lifetime : displayWeek;
  cJSON *ranking = cJSON_AddObjectToObject(root, kind ? "lifetime" : "weekly");
  if (!ranking) okay = false;
  else {
    if (!kind) number(ranking, "week", store.week);
    if (!kind) {
      string(ranking, "source", "daily"); boolean(ranking, "available", dailyReady);
      number(ranking, "observed_days", weekSummary.observedDays);
      number(ranking, "elapsed_days", weekSummary.elapsedDays);
    }
    boolean(ranking, "dirty", kind ? view.lifetimeDirty : weekSummary.saving);
    boolean(ranking, "save_failed", kind ? view.lifetimeSaveFailed : weekSummary.saveFailed);
    cJSON *rows = cJSON_AddArrayToObject(ranking, "entries");
    if (!rows) okay = false;
    else for (const auto &entry : store.entries) {
      if (!entry.clientId) continue;
      cJSON *item = cJSON_CreateObject();
      if (!item) { okay = false; break; }
      if (!cJSON_AddItemToArray(rows, item)) { cJSON_Delete(item); okay = false; break; }
      char id[9], color[8]; std::snprintf(id, sizeof(id), "%08lX", (unsigned long)entry.clientId);
      std::snprintf(color, sizeof(color), "#%06lX", (unsigned long)entry.rgb);
      string(item, "id", id); string(item, "name", entry.name); string(item, "color", color);
      number(item, "tokens", double(entry.tokens));
      if (!kind) {
        for (const auto &day : weekSummary.entries) if (day.identity == entry.clientId)
          number(item, "observed_days", day.observedDays);
        for (const auto &reported : view.weekly.entries) if (reported.clientId == entry.clientId) {
          number(item, "reported_tokens", double(reported.tokens));
          string(item, "reported_protocol", weekly::v2(reported) ? "LFT2" : "LFT1"); break;
        }
      }
    }
  }
  }
  // Keep the bounded response off both the HTTP stack and TLS's scarce internal heap.
  std::unique_ptr<char, decltype(&heap_caps_free)> body(
      static_cast<char *>(heap_caps_malloc(16384, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)), heap_caps_free);
  const bool printed = okay && body && cJSON_PrintPreallocated(root, body.get(), 16384, false);
  cJSON_Delete(root);
  if (!printed) return error(request, "503 Service Unavailable", "Status unavailable; retry.");
  httpd_resp_set_type(request, "application/json; charset=utf-8");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(request, body.get(), HTTPD_RESP_USE_STRLEN);
}
esp_err_t health(httpd_req_t *request) {
  HandlerScope scope; View view;
  if (!snapshot(view)) return error(request, "503 Service Unavailable", "Controller busy; retry.");
  unsigned agents = 0; for (const auto &agent : view.control.agents) agents += agent.present;
  char body[3072];
  const int count = std::snprintf(body, sizeof(body),
      "firmware=%llu board=esp32-s3-n16r8 controllerId=%06lX network_core=0 http_core=0 render_core=1 led_transport=rmt-dma led_pin=%d fps=%d uptime_ms=%lu render_frames=%lu render_missed=%lu render_last_us=%lu render_stack=%lu render_watchdog=%u network_stack=%lu network_loops=%lu assets_ready=%u led_error=%ld heap=%u largest_block=%u psram=%u psram_free=%u wifi_connected=%u wifi_rssi=%d wifi_sleep=none wifi_disconnects=%lu wifi_disconnect_reason=%u agent_count=%u activity=%u remaining=%u maintenance=%u maintenance_epoch=%lu render_epoch=%lu quiescent=%u maintenance_timeouts=%lu settings_dirty=%u nvs_ready=%u udp_packets=%lu ota_done=%lu ota_total=%lu reset_reason=%u\n",
      (unsigned long long)PROJECT_FIRMWARE_VERSION, (unsigned long)view.deviceId, PROJECT_DATA_PIN, PROJECT_FPS,
      (unsigned long)uptimeMs(), (unsigned long)view.render.frames, (unsigned long)view.render.missedDeadlines,
      (unsigned long)view.render.lastFrameUs, (unsigned long)view.render.stackFreeBytes, view.render.watchdogReady, (unsigned long)view.netStack,
      (unsigned long)view.networkLoops, view.render.assetsReady, (long)view.render.ledError,
      unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
      unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
      unsigned(esp_psram_get_size()), unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)), view.control.connected,
      view.rssi, (unsigned long)view.disconnects, view.disconnectReason, agents, view.control.activityPercent,
      view.control.remainingPercent, unsigned(view.control.maintenance), (unsigned long)view.control.maintenanceEpoch,
      (unsigned long)view.render.maintenanceEpoch, view.render.quiescent, (unsigned long)view.maintenanceTimeouts,
      view.dirty, view.nvsReady, (unsigned long)view.udpPackets, (unsigned long)view.otaDone,
      (unsigned long)view.otaTotal, unsigned(esp_reset_reason()));
  if (count < 0 || count >= int(sizeof(body))) return error(request, "503 Service Unavailable", "Diagnostics too large.");
  const int authCount = std::snprintf(body + count, sizeof(body) - size_t(count),
      "auth_check_ms=%lu auth_http_stack_free=%lu\n", (unsigned long)web_auth::lastCheckMs(),
      (unsigned long)web_auth::httpStackFree());
  if (authCount < 0 || size_t(authCount) >= sizeof(body) - size_t(count))
    return error(request, "503 Service Unavailable", "Diagnostics too large.");
  // Only the HTTP server task samples this observer. Snapshot independently of
  // View so a blocked network owner cannot hide behind its last published view.
  static network_progress::Observer observer;
  const auto progress = snapshotNetworkProgress();
  const auto progressHealth = observer.sample(progress, uptimeMs(), 5000);
  const size_t used = size_t(count) + size_t(authCount);
  const int progressCount = std::snprintf(body + used, sizeof(body) - used,
      "network_stage=%s network_stage_age_ms=%lu network_last_progress_ms=%lu network_heartbeat=%lu network_completed_loops=%lu network_progress_started=%u network_stale=%u network_stale_episodes=%lu network_stale_samples=%lu heap_internal_min=%u\n",
      network_progress::name(progress.stage), (unsigned long)progressHealth.stageAgeMs,
      (unsigned long)progress.enteredAtMs, (unsigned long)progress.heartbeat,
      (unsigned long)progress.completedLoops, unsigned(progress.started), unsigned(progressHealth.stale),
      (unsigned long)progressHealth.staleEpisodes, (unsigned long)progressHealth.staleSamples,
      unsigned(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
  if (progressCount < 0 || size_t(progressCount) >= sizeof(body) - used)
    return error(request, "503 Service Unavailable", "Diagnostics too large.");
  httpd_resp_set_type(request, "text/plain; charset=utf-8"); httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  const size_t recoveryOffset = used + size_t(progressCount);
  const uint32_t budgetStatus = recoveryBudgetStatus.load();
  const int recoveryCount = std::snprintf(body + recoveryOffset, sizeof(body) - recoveryOffset,
      "recovery_stage=%u recovery_stack_attempts=%u recovery_faults=%u recovery_offline_ms=%lu recovery_last_action=%04X recovery_budget_valid=%u recovery_budget_used=%u recovery_budget_available=%u recovery_last_reset_code=%04X\n",
      unsigned(view.recoveryStage), unsigned(view.recoveryStacks), unsigned(view.recoveryFaults),
      (unsigned long)view.recoveryOfflineMs, unsigned(view.recoveryCode), unsigned(budgetStatus & 1),
      unsigned((budgetStatus >> 1) & 3), unsigned(recoveryBudgetAvailable.load()), unsigned(budgetStatus >> 16));
  if (recoveryCount < 0 || size_t(recoveryCount) >= sizeof(body) - recoveryOffset)
    return error(request, "503 Service Unavailable", "Diagnostics too large.");
  const auto tcp = network_memory::snapshot();
  const auto alert = air_alert::memoryStats();
  const size_t memoryOffset = recoveryOffset + size_t(recoveryCount);
  const int memoryCount = std::snprintf(body + memoryOffset, sizeof(body) - memoryOffset,
      "tcp_sampled=%u tcp_age_ms=%lu tcp_active=%lu tcp_time_wait=%lu tcp_queued_bytes=%lu tcp_queued_segments=%lu tcp_sample_failures=%lu "
      "http_handlers=%u preview_busy=%u telemetry_pending=%u "
      "alert_polls=%lu alert_age_ms=%lu alert_heap_before=%lu alert_heap_after=%lu alert_largest_after=%lu alert_psram_before=%lu alert_psram_after=%lu alert_city=%u "
      "alert_phase=%s alert_page=%u alert_failure=%s alert_body_bytes=%lu alert_wire_bytes=%lu alert_read_calls=%lu alert_read_waits=%lu "
      "alert_dns_ms=%lu alert_connect_ms=%lu alert_write_ms=%lu alert_read_ms=%lu alert_parse_ms=%lu\n",
      unsigned(tcp.sampled), (unsigned long)(tcp.sampled ? uptimeMs() - tcp.sampledAt : 0),
      (unsigned long)tcp.active, (unsigned long)tcp.timeWait, (unsigned long)tcp.queuedBytes,
      (unsigned long)tcp.queuedSegments, (unsigned long)tcp.callbackFailures,
      unsigned(activeHandlers.load()), unsigned(previewStreamBusy.load()),
      unsigned(telemetryQueue ? uxQueueMessagesWaiting(telemetryQueue) : 0),
      (unsigned long)alert.polls, (unsigned long)(alert.polls ? uptimeMs() - alert.sampledAt : 0), (unsigned long)alert.heapBefore, (unsigned long)alert.heapAfter,
      (unsigned long)alert.largestAfter, (unsigned long)alert.psramBefore,
      (unsigned long)alert.psramAfter, unsigned(alert.cityThreat),
      air_alert::fetchPhaseName(alert.result.fetchPhase), unsigned(alert.result.pageAttempt), air_alert::failureName(alert.result.failure),
      (unsigned long)alert.result.bodyBytes, (unsigned long)alert.result.wireBytes,
      (unsigned long)alert.result.readCalls, (unsigned long)alert.result.readWaits,
      (unsigned long)alert.result.dnsMs, (unsigned long)alert.result.connectMs,
      (unsigned long)alert.result.writeMs, (unsigned long)alert.result.readMs,
      (unsigned long)alert.result.parseMs);
  if (memoryCount < 0 || size_t(memoryCount) >= sizeof(body) - memoryOffset)
    return error(request, "503 Service Unavailable", "Diagnostics too large.");
  const size_t clockOffset = memoryOffset + size_t(memoryCount);
  const bool synced = clockSyncSeen.load(std::memory_order_acquire);
  const uint32_t syncedAt = lastClockSyncAt.load(std::memory_order_relaxed);
  const uint32_t observedAt = uptimeMs();
  const int clockCount = std::snprintf(body + clockOffset, sizeof(body) - clockOffset,
      "standby=%u clock_ready=%u clock_retries=%lu clock_synced=%u clock_sync_age_ms=%lu\n",
      unsigned(!view.control.on), unsigned(std::time(nullptr) >= 1700000000),
      (unsigned long)view.clockRetries, unsigned(synced),
      (unsigned long)(synced ? observedAt - syncedAt : 0));
  if (clockCount < 0 || size_t(clockCount) >= sizeof(body) - clockOffset)
    return error(request, "503 Service Unavailable", "Diagnostics too large.");
  return httpd_resp_send(request, body, int(clockOffset) + clockCount);
}
void diagnosticMonitorTask(void *) {
  TickType_t wake = xTaskGetTickCount();
  int32_t previousLedError = 0;
  uint32_t checkpointAt = 0, memoryWarningAt = 0;
  bool memoryWarned = false;
  for (;;) {
    diagnostics::HealthContext context;
    context.summary.version = PROJECT_FIRMWARE_VERSION;
    context.summary.reset_cause = unsigned(esp_reset_reason());
    context.summary.heap_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    context.heap_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    context.largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const time_t epoch = std::time(nullptr);
    if (epoch >= 1700000000 && uint64_t(epoch) <= UINT32_MAX) context.unix_epoch_s = uint32_t(epoch);
    const auto progress = snapshotNetworkProgress();
    // Read the clock AFTER the synchronized progress snapshot. A newer
    // enteredAtMs paired with an older clock would wrap and fake a long stall.
    context.summary.uptime_ms = uptimeMs();
    context.summary.network_stage = unsigned(progress.stage);
    context.network_age_ms = context.summary.uptime_ms - progress.enteredAtMs;
    context.network_started = progress.started && progress.stage != network_progress::Stage::Starting;
    int32_t ledError = 0;
    bool recoverySafeView = false;
    if (viewMutex && xSemaphoreTake(viewMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      context.view_valid = true;
      context.render_frames = sharedView.render.frames;
      context.network_stack = sharedView.netStack;
      context.wifi_connected = sharedView.control.connected;
      context.wifi_rssi = sharedView.rssi;
      for (const auto &agent : sharedView.control.agents) context.agents += agent.present;
      ledError = sharedView.render.ledError;
      recoverySafeView = sharedView.control.maintenance == Maintenance::None &&
          !sharedView.restarting && !sharedView.control.memorialActive && !sharedView.render.memorialTransition;
      // Flash/OTA deliberately park the network owner. Record only an overrun.
      const auto maintenance = sharedView.control.maintenance;
      if (maintenance != Maintenance::None && context.network_age_ms <
          (maintenance == Maintenance::Ota ? 210000U : 15000U)) context.network_started = false;
      xSemaphoreGive(viewMutex);
    }
    portENTER_CRITICAL(&recoveryBudgetMux);
    recoveryBudget.observe(retainedRecovery, context.view_valid && context.wifi_connected &&
        context.network_age_ms < 5000, context.summary.uptime_ms);
    recoveryBudgetAvailable.store(recoveryBudget.available(retainedRecovery));
    recoveryBudgetStatus.store((recovery::valid(retainedRecovery) ? 1U : 0U) |
        (retainedRecovery.used << 1) | (retainedRecovery.reason << 16));
    portEXIT_CRITICAL(&recoveryBudgetMux);
    const bool manualGrace = recoveryManualGrace.load() &&
        uint32_t(context.summary.uptime_ms - recoveryManualAt.load()) < recovery::manualGraceMs;
    if (recovery::stalled(context.network_started, context.network_age_ms, context.summary.uptime_ms,
          recoveryBudgetAvailable.load(), recoverySafeView && !manualGrace) && recoveryGate.claim()) {
      // The gate prevents a concurrently resumed owner from starting flash or commands.
      // A stale decision must not reset an owner that resumed before the claim.
      // Read the synchronized heartbeat first, then time, to avoid age underflow.
      const auto claimedProgress = snapshotNetworkProgress();
      const uint32_t claimedAt = uptimeMs();
      if (recovery::stillStalled(progress, claimedProgress, claimedAt) &&
          reserveRecoveryRestart(recovery::stalledCode)) {
        diagnostic_log::Snapshot fault; fault.elapsed_ms = claimedAt - claimedProgress.enteredAtMs;
        fault.detail = unsigned(claimedProgress.stage);
        diagnostics::event(diagnostics::Level::Error, diagnostics::Module::System, recovery::stalledCode, fault);
        diagnostics::checkpoint(context.summary);
        esp_restart();
      }
      recoveryGate.cancel();
    }
    diagnostics::healthCheck(context);
    network_memory::request();
    // Preserve numeric queue observations alongside the existing flash archive.
    // Sampled on tcpip, copied here, never traverse lwIP lists from this task.
    if ((context.heap_free < 49152 || context.largest_block < 16384) &&
        (!memoryWarned || uint32_t(context.summary.uptime_ms - memoryWarningAt) >= 30000)) {
      const auto tcp = network_memory::snapshot();
      bool recorded = false;
      if (tcp.sampled && uint32_t(uptimeMs() - tcp.sampledAt) < 5000) {
        diagnostic_log::Snapshot queues;
        queues.reason = int32_t(tcp.active); queues.detail = int32_t(tcp.timeWait);
        queues.attempts = tcp.queuedBytes; queues.elapsed_ms = tcp.queuedSegments;
        recorded = diagnostics::event(diagnostics::Level::Warning, diagnostics::Module::Http, uint16_t(0x0600), queues);
      }
      const auto alert = air_alert::memoryStats();
      const uint32_t alertAge = uptimeMs() - alert.sampledAt;
      if (alert.polls && alertAge < 30000) {
        diagnostic_log::Snapshot memory;
        memory.reason = int32_t(alert.heapBefore); memory.detail = int32_t(alert.heapAfter);
        memory.attempts = alert.largestAfter; memory.elapsed_ms = alertAge;
        recorded = diagnostics::event(diagnostics::Level::Warning, diagnostics::Module::Alert, uint16_t(0x0601), memory) || recorded;
      }
      if (recorded) {
        memoryWarned = true;
        memoryWarningAt = context.summary.uptime_ms;
      }
    }
    if (context.summary.uptime_ms - checkpointAt >= 10000) {
      diagnostics::checkpoint(context.summary);
      checkpointAt = context.summary.uptime_ms;
    }
    if (ledError && ledError != previousLedError) {
      diagnostic_log::Snapshot error; error.reason = ledError;
      diagnostics::event(diagnostics::Level::Error, diagnostics::Module::Output,
                         diagnostics::Code::OutputFailed, error);
    }
    previousLedError = ledError;
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(2000));
  }
}

bool readBody(httpd_req_t *request, char *body, size_t capacity) {
  if (request->content_len >= capacity) return false;
  size_t read = 0; const uint32_t started = uptimeMs();
  while (read < request->content_len) {
    if (uptimeMs() - started >= 5000) return false;
    const int count = httpd_req_recv(request, body + read, request->content_len - read);
    if (uptimeMs() - started >= 5000 || count <= 0 || std::memchr(body + read, 0, count)) return false;
    read += size_t(count);
  }
  body[read] = 0; return true;
}
bool decode(const char *begin, const char *end, char *output, size_t capacity) {
  size_t size = 0;
  for (auto p = begin; p < end; ++p) {
    char value = *p;
    if (value == '+') value = ' ';
    else if (value == '%') {
      if (end - p < 3 || hexDigit(p[1]) < 0 || hexDigit(p[2]) < 0) return false;
      value = char((hexDigit(p[1]) << 4) | hexDigit(p[2])); p += 2;
    }
    if (!value || uint8_t(value) < 0x20 || size + 1 >= capacity) return false;
    output[size++] = value;
  }
  output[size] = 0; return true;
}
struct Form {
  struct Field { char key[16]; char value[129]; } fields[16] = {};
  size_t count = 0;
  bool parse(const char *body) {
    const char *p = body;
    while (*p) {
      if (count == 16) return false;
      const char *end = std::strchr(p, '&'); if (!end) end = p + std::strlen(p);
      const char *equal = static_cast<const char *>(std::memchr(p, '=', end - p));
      if (!equal || !decode(p, equal, fields[count].key, sizeof(fields[count].key)) ||
          !decode(equal + 1, end, fields[count].value, sizeof(fields[count].value))) return false;
      for (size_t i = 0; i < count; ++i) if (!std::strcmp(fields[i].key, fields[count].key)) return false;
      ++count; p = *end ? end + 1 : end;
    }
    return true;
  }
  const char *get(const char *key) const { for (size_t i = 0; i < count; ++i) if (!std::strcmp(key, fields[i].key)) return fields[i].value; return ""; }
  bool has(const char *key) const { for (size_t i = 0; i < count; ++i) if (!std::strcmp(key, fields[i].key)) return true; return false; }
};
esp_err_t acceptedCommand(httpd_req_t *request) {
  httpd_resp_set_status(request, "202 Accepted");
  httpd_resp_set_type(request, "application/json");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(request, "{\"accepted\":true}", HTTPD_RESP_USE_STRLEN);
}
bool clockMinutes(const char *text, uint16_t &result) {
  if (std::strlen(text) != 5 || text[2] != ':') return false;
  char hour[3] = {text[0], text[1], 0}, minute[3] = {text[3], text[4], 0}; uint64_t h, m;
  if (!unsignedValue(hour, 23, h) || !unsignedValue(minute, 59, m)) return false;
  result = uint16_t(h * 60 + m); return true;
}
bool agentMac(const uint8_t *key, size_t keySize, const char *text, uint8_t (&output)[32]) {
  psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
  psa_set_key_type(&attributes, PSA_KEY_TYPE_HMAC);
  psa_set_key_bits(&attributes, keySize * 8);
  psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_MESSAGE);
  psa_set_key_algorithm(&attributes, PSA_ALG_HMAC(PSA_ALG_SHA_256));
  psa_key_id_t id = 0;
  if (psa_import_key(&attributes, key, keySize, &id) != PSA_SUCCESS) return false;
  size_t written = 0;
  const bool okay = psa_mac_compute(id, PSA_ALG_HMAC(PSA_ALG_SHA_256), reinterpret_cast<const uint8_t *>(text),
      std::strlen(text), output, sizeof(output), &written) == PSA_SUCCESS && written == sizeof(output);
  psa_destroy_key(id);
  return okay;
}
void hexBytes(const uint8_t (&bytes)[32], char (&hex)[65]) {
  for (unsigned i = 0; i < sizeof(bytes); ++i) std::snprintf(hex + i * 2, 3, "%02x", bytes[i]);
}
bool agentSecret(uint32_t client, uint8_t (&output)[32]) {
  char context[32]; std::snprintf(context, sizeof(context), "LFAU1-AGENT\n%08lX\n", (unsigned long)client);
  return agentUpdateKey[0] && agentMac(reinterpret_cast<const uint8_t *>(agentUpdateKey), 32, context, output);
}
bool lowerHex(const char *value, size_t size) {
  if (std::strlen(value) != size) return false;
  for (size_t i = 0; i < size; ++i) if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f'))) return false;
  return true;
}
uint64_t agentChallengeCounter() { return uint64_t(esp_timer_get_time()) / 10000000ULL + 1; }
void agentChallenge(char (&nonce)[33]) {
  std::snprintf(nonce, sizeof(nonce), "%s%016llx", agentUpdateBoot, (unsigned long long)agentChallengeCounter());
}
bool validAgentChallenge(const char *nonce, uint64_t &counter) {
  if (!agentUpdateBoot[0] || !lowerHex(nonce, 64) || std::strncmp(nonce, agentUpdateBoot, 16)) return false;
  counter = 0;
  for (unsigned i = 16; i < 32; ++i) counter = counter * 16 + unsigned(hexDigit(nonce[i]));
  const uint64_t current = agentChallengeCounter();
  return counter && counter <= current && current - counter <= 1;
}
esp_err_t agentUpdateArtifact(httpd_req_t *request) {
  HandlerScope scope;
  if (!agent_release::version) return error(request, "503 Service Unavailable", "No signed agent release.");
  if (!std::strcmp(request->uri, "/agent-update/manifest.json"))
    return serveWebFile(request, "agent-update/manifest.json", "application/json; charset=utf-8");
  if (!std::strcmp(request->uri, "/agent-update/package.zip"))
    return serveWebFile(request, "agent-update/package.zip", "application/zip");
  return error(request, "404 Not Found", "Not found.");
}
esp_err_t agentUpdatePost(httpd_req_t *request) {
  HandlerScope scope;
  View view;
  if (!snapshot(view) || view.restarting || view.control.maintenance == Maintenance::Ota || view.control.maintenance == Maintenance::Restart)
    return error(request, "503 Service Unavailable", "Controller maintenance; retry.");
  char body[513]; Form form;
  if (!readBody(request, body, sizeof(body)) || !form.parse(body)) return error(request, "400 Bad Request", "Invalid form.");
  uint32_t client;
  if (!hexValue(form.get("client"), 8, 8, client))
    return error(request, "400 Bad Request", "Known online agent required.");
  const bool enroll = !std::strcmp(request->uri, "/agent-update-enroll");
  const bool poll = !std::strcmp(request->uri, "/agent-update-poll");
  const bool challenge = !std::strcmp(request->uri, "/agent-update-challenge");
  if (!knownAgent(view.control, client) && !((poll || challenge) && uninstallAcknowledgmentWindow(view.agentUpdates, client, uptimeMs())))
    return error(request, "400 Bad Request", "Known online agent required.");
  const bool rename = !std::strcmp(request->uri, "/agent-rename");
  const bool uninstall = !std::strcmp(request->uri, "/agent-uninstall");
  if (!enroll && !poll && !challenge && !rename && !uninstall && std::strcmp(request->uri, "/agent-upgrade")) return error(request, "404 Not Found", "Not found.");
  if (enroll) {
    uint32_t account;
    if (!hexValue(form.get("account"), 8, 8, account) || account != PROJECT_ACCOUNT_FINGERPRINT)
      return error(request, "400 Bad Request", "Invalid enrollment account.");
  }
  uint8_t secret[32];
  if (!agentSecret(client, secret)) return error(request, "503 Service Unavailable", "Agent updates unavailable.");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  httpd_resp_set_hdr(request, "Connection", "close");
  if (challenge) {
    uint32_t account;
    if (!hexValue(form.get("account"), 8, 8, account) || account != PROJECT_ACCOUNT_FINGERPRINT)
      return error(request, "400 Bad Request", "Invalid challenge request.");
    char nonce[33], response[100]; agentChallenge(nonce);
    std::snprintf(response, sizeof(response), "{\"device\":\"%06lX\",\"nonce\":\"%s\"}", (unsigned long)view.deviceId, nonce);
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_send(request, response, HTTPD_RESP_USE_STRLEN);
  }
  if (enroll) {
    char hex[65], response[128]; hexBytes(secret, hex);
    std::snprintf(response, sizeof(response), "{\"device\":\"%06lX\",\"secret\":\"%s\"}", (unsigned long)view.deviceId, hex);
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_send(request, response, HTTPD_RESP_USE_STRLEN);
  }
  NetworkCommand command{}; command.clientId = client;
  if (poll) {
    uint32_t account;
    if (!unsignedValue(form.get("version"), weekly::maximum, command.agentVersion) || !command.agentVersion ||
        !hexValue(form.get("account"), 8, 8, account) || account != PROJECT_ACCOUNT_FINGERPRINT ||
        !validAgentChallenge(form.get("nonce"), command.agentNonceCounter) || !lowerHex(form.get("mac"), 64))
      return error(request, "400 Bad Request", "Invalid update poll.");
    if (const auto *entry = view.agentUpdates.find(command.clientId))
      if (command.agentNonceCounter < entry->lastCounter || (command.agentNonceCounter == entry->lastCounter &&
          !(form.get("ack")[0] && entry->actionState == 1))) return error(request, "409 Conflict", "Poll challenge already consumed.");
    command.kind = CommandKind::AgentPoll;
    command.agentManagement = !std::strcmp(form.get("manage"), "1");
    if (form.get("manage")[0] && !command.agentManagement) return error(request, "400 Bad Request", "Invalid management capability.");
    if (form.get("ack")[0] || form.get("result")[0]) {
      if (!command.agentManagement || !unsignedValue(form.get("ack"), weekly::maximum, command.agentActionAck) || !command.agentActionAck)
        return error(request, "400 Bad Request", "Invalid command acknowledgment.");
      command.agentActionResult = !std::strcmp(form.get("result"), "ok") ? 1 : !std::strcmp(form.get("result"), "failed") ? 2 : 0;
      if (!command.agentActionResult) return error(request, "400 Bad Request", "Invalid command result.");
    }
    if (form.get("failed")[0] && !unsignedValue(form.get("failed"), weekly::maximum, command.agentFailedVersion))
      return error(request, "400 Bad Request", "Invalid update failure status.");
    char signedText[384]; uint8_t expected[32]; char hex[65];
    std::snprintf(signedText, sizeof(signedText), "LFAU1-POLL\n%06lX\n%08lX\n%s\n%llu\n",
        (unsigned long)view.deviceId, (unsigned long)command.clientId, form.get("nonce"), (unsigned long long)command.agentVersion);
    if (command.agentFailedVersion) {
      const size_t used = std::strlen(signedText);
      std::snprintf(signedText + used, sizeof(signedText) - used, "FAILED\n%llu\n", (unsigned long long)command.agentFailedVersion);
    }
    if (command.agentManagement) {
      size_t used = std::strlen(signedText);
      std::snprintf(signedText + used, sizeof(signedText) - used, "MANAGE\n1\n");
      if (command.agentActionAck) {
        used = std::strlen(signedText);
        std::snprintf(signedText + used, sizeof(signedText) - used, "ACK\n%llu\n%s\n", (unsigned long long)command.agentActionAck, form.get("result"));
      }
    }
    if (!agentMac(secret, sizeof(secret), signedText, expected)) return error(request, "503 Service Unavailable", "Authentication unavailable.");
    hexBytes(expected, hex);
    unsigned difference = 0;
    for (unsigned i = 0; i < 64; ++i) difference |= unsigned(uint8_t(hex[i]) ^ uint8_t(form.get("mac")[i]));
    if (difference) return error(request, "403 Forbidden", "Invalid poll authentication.");
  } else if (rename || uninstall) {
    const auto *entry = view.agentUpdates.find(client);
    if (!entry || !entry->management || uptimeMs() - entry->polledAt >= agent_update::freshMs ||
        entry->actionState == 1 || view.agentUpdates.target(client, uptimeMs()))
      return error(request, "409 Conflict", "Agent management channel is not ready.");
    command.kind = rename ? CommandKind::AgentRename : CommandKind::AgentUninstall;
    if (rename) {
      const char *name = form.get("name"); const size_t length = std::strlen(name);
      bool visible = false;
      for (size_t i = 0; i < length; ++i) if (uint8_t(name[i]) > 0x20) visible = true;
      if (!length || length > 48 || !visible || !validUtf8(name, length))
        return error(request, "400 Bad Request", "Name must contain 1-48 UTF-8 bytes without controls.");
      copy(command.agentName, name);
    } else if (std::strcmp(form.get("confirm"), "uninstall")) return error(request, "400 Bad Request", "Uninstall confirmation required.");
  } else {
    command.kind = CommandKind::AgentUpgrade;
    const auto *entry = view.agentUpdates.find(command.clientId);
    if (!entry || !agent_release::version || entry->version >= agent_release::version || entry->failed >= agent_release::version ||
        uptimeMs() - entry->polledAt >= agent_update::freshMs || entry->actionState == 1)
      return error(request, "409 Conflict", "Agent update channel is not ready.");
  }
  if (xSemaphoreTake(viewMutex, pdMS_TO_TICKS(20)) != pdTRUE) return error(request, "503 Service Unavailable", "Controller busy.");
  const bool blocked = sharedView.restarting || sharedView.control.maintenance == Maintenance::Ota || sharedView.control.maintenance == Maintenance::Restart;
  const bool queued = !blocked && xQueueSend(commandQueue, &command, 0) == pdTRUE;
  xSemaphoreGive(viewMutex);
  if (!queued) return error(request, "503 Service Unavailable", "Update queue unavailable.");
  if (poll) {
    uint64_t target = view.agentUpdates.target(command.clientId, uptimeMs());
    if (target <= command.agentVersion) target = 0;
    const auto *action = command.agentManagement ? view.agentUpdates.pendingAction(command.clientId, uptimeMs()) : nullptr;
    if (action && command.agentActionAck == action->actionId) action = nullptr;
    char signedText[384]; uint8_t mac[32]; char hex[65], response[512], nameHex[97] = {};
    std::snprintf(signedText, sizeof(signedText), "LFAU1\n%06lX\n%08lX\n%s\n%llu\n",
        (unsigned long)view.deviceId, (unsigned long)command.clientId, form.get("nonce"), (unsigned long long)target);
    const char *actionName = action && action->action == agent_update::Action::Rename ? "rename" : "uninstall";
    if (action) {
      for (size_t i = 0; action->actionName[i]; ++i) std::snprintf(nameHex + i * 2, 3, "%02x", uint8_t(action->actionName[i]));
      const size_t used = std::strlen(signedText);
      std::snprintf(signedText + used, sizeof(signedText) - used, "ACTION\n%llu\n%s\n%s\n", (unsigned long long)action->actionId, actionName, nameHex);
    }
    if (!agentMac(secret, sizeof(secret), signedText, mac)) return error(request, "503 Service Unavailable", "Authentication unavailable.");
    hexBytes(mac, hex);
    if (action) std::snprintf(response, sizeof(response), "{\"device\":\"%06lX\",\"version\":%llu,\"nonce\":\"%s\",\"mac\":\"%s\",\"action_id\":%llu,\"action\":\"%s\",\"name_hex\":\"%s\"}",
        (unsigned long)view.deviceId, (unsigned long long)target, form.get("nonce"), hex, (unsigned long long)action->actionId, actionName, nameHex);
    else std::snprintf(response, sizeof(response), "{\"device\":\"%06lX\",\"version\":%llu,\"nonce\":\"%s\",\"mac\":\"%s\"}",
        (unsigned long)view.deviceId, (unsigned long long)target, form.get("nonce"), hex);
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_send(request, response, HTTPD_RESP_USE_STRLEN);
  }
  if (!std::strcmp(form.get("__async"), "1")) return acceptedCommand(request);
  httpd_resp_set_status(request, "303 See Other"); httpd_resp_set_hdr(request, "Location", "/agents");
  return httpd_resp_send(request, nullptr, 0);
}
esp_err_t telemetryPost(httpd_req_t *request) {
  HandlerScope scope;
  View view;
  if (!snapshot(view) || view.control.maintenance == Maintenance::Ota || view.control.maintenance == Maintenance::Restart)
    return error(request, "503 Service Unavailable", "Controller maintenance; retry.");
  TelemetryRequest command{}; command.id = ++requestSequence;
  if (!readBody(request, command.packet, sizeof(command.packet))) return error(request, "400 Bad Request", "Invalid telemetry.");
  AgentInput agent; bool legacy; uint8_t remaining, activity;
  weekly::Entry weeklyEntry; uint32_t week = 0;
  quota_provider::Packet claudePacket;
  quota_metadata::Packet metadataPacket;
  if (!parseTelemetry(command.packet, agent, legacy, remaining, activity) &&
      !parseWeeklyTelemetry(command.packet, weeklyEntry, week) &&
      !quota_provider::parse(command.packet, PROJECT_ACCOUNT_FINGERPRINT, claudePacket) &&
      !quota_metadata::parse(command.packet, PROJECT_ACCOUNT_FINGERPRINT, metadataPacket)) return error(request, "400 Bad Request", "Invalid telemetry.");
  command.receivedAt = uptimeMs();
  if (xQueueSend(telemetryQueue, &command, 0) != pdTRUE) return error(request, "503 Service Unavailable", "Telemetry queue full.");
  const uint32_t start = uptimeMs(); TelemetryReply reply{};
  while (uptimeMs() - start < 500) {
    if (xQueueReceive(telemetryReplies, &reply, pdMS_TO_TICKS(20)) != pdTRUE) continue;
    if (reply.id != command.id) continue;
    if (!reply.accepted) return error(request, "503 Service Unavailable", "Agent table full; retry.");
    httpd_resp_set_type(request, "text/plain"); httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, reply.ack, HTTPD_RESP_USE_STRLEN);
  }
  return error(request, "503 Service Unavailable", "Telemetry owner busy; retry.");
}
esp_err_t commandPost(httpd_req_t *request) {
  HandlerScope scope; View view;
  if (!snapshot(view)) return error(request, "503 Service Unavailable", "Controller busy; retry.");
  if (view.restarting || view.control.maintenance == Maintenance::Ota || view.control.maintenance == Maintenance::Restart)
    return error(request, "503 Service Unavailable", "Controller maintenance; retry after restart or update.");
  char body[513];
  if (!readBody(request, body, sizeof(body))) return error(request, "400 Bad Request", "Invalid or oversized request.");
  Form form; if (!form.parse(body)) return error(request, "400 Bad Request", "Invalid form.");
  NetworkCommand command; uint64_t number;
  const char *uri = request->uri;
  if (!std::strcmp(uri, "/save")) {
    command.kind = CommandKind::Wifi;
    const char *ssid = form.get("ssid"), *password = form.get("password");
    const size_t ssidLength = std::strlen(ssid), passwordLength = std::strlen(password);
    if (!ssidLength || ssidLength > 32 || (passwordLength && (passwordLength < 8 || passwordLength > 63))) return error(request, "400 Bad Request", "Wi-Fi name or password length is invalid.");
    copy(command.ssid, ssid); copy(command.password, password);
  } else if (!std::strcmp(uri, "/power")) {
    command.kind = CommandKind::Power; const char *mode = form.get("mode");
    if (std::strcmp(mode, "auto") && std::strcmp(mode, "on") && std::strcmp(mode, "off")) return error(request, "400 Bad Request", "Invalid power mode.");
    command.value = !std::strcmp(mode, "auto") ? 0 : !std::strcmp(mode, "on") ? 1 : 2;
  } else if (!std::strcmp(uri, "/brightness")) {
    command.kind = CommandKind::Brightness;
    if (!unsignedValue(form.get("value"), 255, number) || !number) return error(request, "400 Bad Request", "Brightness must be 1 to 255.");
    command.value = uint8_t(number);
  } else if (!std::strcmp(uri, "/external-light")) {
    command.kind = CommandKind::ExternalLight;
    if (!view.lightReady) return error(request, "503 Service Unavailable", "Виходи підсвітки недоступні.");
    uint64_t second;
    const char *follow = form.get("follow_matrix"), *off = form.get("off");
    if (!unsignedValue(form.get("first"), 100, number) || !unsignedValue(form.get("second"), 100, second) ||
        (follow[0] && std::strcmp(follow, "1")) || (off[0] && std::strcmp(off, "1")))
      return error(request, "400 Bad Request", "Яскравість підсвітки має бути від 0 до 100%.");
    command.start = off[0] ? 0 : uint16_t(number); command.end = off[0] ? 0 : uint16_t(second);
    command.enabled = follow[0] != 0;
  } else if (!std::strcmp(uri, "/buzzer-test")) {
    if (!view.buzzerReady || view.control.maintenance != Maintenance::None)
      return error(request, "503 Service Unavailable", "Звуковий вихід недоступний. Спробуйте ще раз.");
    command.kind = CommandKind::BuzzerTest;
  } else if (!std::strcmp(uri, "/buzzer-volume")) {
    if (!unsignedValue(form.get("value"), 100, number))
      return error(request, "400 Bad Request", "Гучність має бути від 0 до 100%.");
    command.kind = CommandKind::BuzzerVolume; command.value = uint8_t(number);
  } else if (!std::strcmp(uri, "/schedule")) {
    command.kind = CommandKind::Schedule; command.enabled = !std::strcmp(form.get("enabled"), "1");
    if (!clockMinutes(form.get("start"), command.start) || !clockMinutes(form.get("end"), command.end)) return error(request, "400 Bad Request", "Invalid schedule time.");
    for (unsigned day = 0; day < 7; ++day) { char key[4]; std::snprintf(key, sizeof(key), "d%u", day); if (!std::strcmp(form.get(key), "1")) command.days |= 1U << day; }
    if (command.enabled && !command.days) return error(request, "400 Bad Request", "Select at least one day.");
  } else if (!std::strcmp(uri, "/quota-rotation")) {
    if (!unsignedValue(form.get("minutes"), 60, number) || number < 1)
      return error(request, "400 Bad Request", "Інтервал має бути від 1 до 60 хвилин.");
    command.kind = CommandKind::QuotaRotation; command.value = uint8_t(number);
  } else if (!std::strcmp(uri, "/sky-colors")) {
    command.kind = CommandKind::SkyColors;
    // Absent preserves the legacy color-only request; present-but-empty is invalid.
    if (form.has("minutes")) {
      if (!unsignedValue(form.get("minutes"), 60, number) || number < 1)
        return error(request, "400 Bad Request", "Rotation interval must be between 1 and 60 minutes.");
      command.value = uint8_t(number);
    }
    const char *codex = form.get("codex"), *claude = form.get("claude");
    if (!hexValue(codex + (codex[0] == '#'), 6, 6, command.codexSkyColor) ||
        !hexValue(claude + (claude[0] == '#'), 6, 6, command.claudeSkyColor))
      return error(request, "400 Bad Request", "Sky colors must contain exactly six hexadecimal digits.");
  } else if (!std::strcmp(uri, "/agent-color")) {
    command.kind = CommandKind::Color; const char *mode = form.get("mode"), *color = form.get("color");
    command.automatic = !std::strcmp(mode, "auto");
    if (!hexValue(form.get("client"), 8, 8, command.clientId) || (!command.automatic && std::strcmp(mode, "manual")) ||
        (!command.automatic && (color[0] != '#' || !hexValue(color + 1, 6, 6, command.rgb)))) return error(request, "400 Bad Request", "Invalid agent color.");
  } else if (!std::strcmp(uri, "/air-alert")) {
    const char *mode = form.get("mode");
    if (!std::strcmp(mode, "save")) {
      const char *enabled = form.get("enabled");
      if ((enabled[0] && std::strcmp(enabled, "1")) || form.get("api_token")[0] || form.get("clear_token")[0])
        return error(request, "400 Bad Request", "Telegram-канал не потребує API-токена. Оновіть сторінку.");
      command.kind = CommandKind::AirAlertSave; command.enabled = enabled[0] != 0;
    } else if (!std::strcmp(mode, "yellow") || !std::strcmp(mode, "red") || !std::strcmp(mode, "city")) {
      if (!view.control.on) return error(request, "409 Conflict", "Матриця вимкнена. Спочатку ввімкніть її.");
      command.kind = CommandKind::AirAlertTest; command.value = !std::strcmp(mode, "city") ? 5 : !std::strcmp(mode, "red") ? 2 : 1;
    } else if (!std::strcmp(mode, "stop")) command.kind = CommandKind::AirAlertStop;
    else return error(request, "400 Bad Request", "Некоректна дія перевірки тривоги.");
  } else if (!std::strcmp(uri, "/black-hole")) {
    const char *mode = form.get("mode");
    if (!std::strcmp(mode, "play")) {
      if (!view.control.on || view.control.memorialActive || view.render.memorialTransition)
        return error(request, "409 Conflict", "Black hole preview unavailable.");
      if (view.control.maintenance != Maintenance::None)
        return error(request, "503 Service Unavailable", "Controller maintenance; retry.");
      command.kind = CommandKind::BlackHolePlay;
    } else if (!std::strcmp(mode, "stop")) command.kind = CommandKind::BlackHoleStop;
    else return error(request, "400 Bad Request", "Invalid black hole action.");
  } else if (!std::strcmp(uri, "/memorial")) {
    const char *mode = form.get("mode");
    if (!std::strcmp(mode, "save")) {
      const char *enabled = form.get("enabled");
      if (enabled[0] && std::strcmp(enabled, "1")) return error(request, "400 Bad Request", "Invalid memorial setting.");
      command.kind = CommandKind::MemorialSave; command.enabled = enabled[0] != 0;
    } else if (!std::strcmp(mode, "play")) {
      if (!view.control.on) return error(request, "409 Conflict", "Матриця вимкнена. Спочатку ввімкніть її.");
      command.kind = CommandKind::MemorialPlay;
    } else if (!std::strcmp(mode, "stop")) command.kind = CommandKind::MemorialStop;
    else return error(request, "400 Bad Request", "Invalid memorial action.");
  } else if (!std::strcmp(uri, "/restart")) command.kind = CommandKind::Restart;
  else if (!std::strcmp(uri, "/update")) command.kind = CommandKind::Update;
  else return error(request, "404 Not Found", "Unknown command.");
  // Serialize the final queue submission with publication of the OTA/restart gate.
  if (xSemaphoreTake(viewMutex, pdMS_TO_TICKS(20)) != pdTRUE)
    return error(request, "503 Service Unavailable", "Controller busy; retry.");
  const bool unavailable = sharedView.restarting || sharedView.control.maintenance == Maintenance::Ota ||
      sharedView.control.maintenance == Maintenance::Restart ||
      ((command.kind == CommandKind::BuzzerTest || command.kind == CommandKind::BlackHolePlay) && sharedView.control.maintenance != Maintenance::None) ||
      (command.kind == CommandKind::BlackHolePlay && (!sharedView.control.on || sharedView.control.memorialActive || sharedView.render.memorialTransition));
  const bool queued = !unavailable && xQueueSend(commandQueue, &command, 0) == pdTRUE;
  xSemaphoreGive(viewMutex);
  if (!queued) return error(request, "503 Service Unavailable", unavailable ? "Controller maintenance; retry." : "Command queue full; retry.");
  if (!std::strcmp(form.get("__async"), "1")) return acceptedCommand(request);
  httpd_resp_set_status(request, "303 See Other");
  httpd_resp_set_hdr(request, "Location", command.kind == CommandKind::Color ? "/agents" :
      command.kind == CommandKind::QuotaRotation || command.kind == CommandKind::SkyColors || command.kind == CommandKind::Wifi || command.kind == CommandKind::Update || command.kind == CommandKind::Restart || command.kind == CommandKind::BuzzerTest || command.kind == CommandKind::BuzzerVolume ||
      command.kind == CommandKind::AirAlertSave || command.kind == CommandKind::AirAlertTest || command.kind == CommandKind::AirAlertStop ||
      command.kind == CommandKind::BlackHolePlay || command.kind == CommandKind::BlackHoleStop ||
      command.kind == CommandKind::MemorialSave || command.kind == CommandKind::MemorialPlay || command.kind == CommandKind::MemorialStop ? "/settings" : "/");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store"); httpd_resp_set_hdr(request, "Connection", "close");
  return httpd_resp_send(request, nullptr, 0);
}
using HttpHandler = esp_err_t (*)(httpd_req_t *);
esp_err_t tokenHistoryGet(httpd_req_t *request) {
  const auto clock = memorialClock();
  return daily_runtime::get(request, clock.valid ? clock.date : 0);
}
struct ProtectedRoute { HttpHandler handler = nullptr; };
ProtectedRoute protectedRoutes[40];
size_t protectedRouteCount = 0;
esp_err_t protectedRequest(httpd_req_t *request) {
  HandlerScope scope;
  if (!web_auth::authorize(request)) return ESP_OK;
  const auto *route = static_cast<const ProtectedRoute *>(request->user_ctx);
  return route && route->handler ? route->handler(request) : ESP_FAIL;
}
bool machineRoute(const char *uri, httpd_method_t method) {
  // LAN agents keep their existing telemetry and signed-update protocol.
  // Browser actions (upgrade, rename, uninstall) are deliberately not exempt.
  if (method == HTTP_POST) return !std::strcmp(uri, "/agent") ||
      !std::strcmp(uri, "/agent-update-enroll") || !std::strcmp(uri, "/agent-update-challenge") ||
      !std::strcmp(uri, "/agent-update-poll");
  return method == HTTP_GET && (!std::strcmp(uri, "/agent-update/manifest.json") ||
      !std::strcmp(uri, "/agent-update/package.zip"));
}
bool initializeHttp() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG(); config.core_id = 0; config.stack_size = 16384;
  config.max_uri_handlers = 64; config.max_open_sockets = 4; config.lru_purge_enable = true;
  // Cookies and browser cache validators share this buffer. The 1 KiB IDF
  // default can reject authenticated asset requests before our handlers run.
  // Keep the limit bounded; do not increase socket count or task stack use.
  config.max_req_hdr_len = 2048;
  config.recv_wait_timeout = 2; config.send_wait_timeout = 2;
  if (httpd_start(&model.http, &config) != ESP_OK) return false;
  const auto failed = []() { httpd_stop(model.http); model.http = nullptr; return false; };
  protectedRouteCount = 0;
  const auto registerRoute = [](const char *uri, httpd_method_t method, HttpHandler handler) {
    httpd_uri_t route{}; route.uri = uri; route.method = method; route.handler = handler;
    if (!machineRoute(uri, method)) {
      if (protectedRouteCount >= sizeof(protectedRoutes) / sizeof(protectedRoutes[0])) return false;
      auto &binding = protectedRoutes[protectedRouteCount++]; binding.handler = handler;
      route.handler = protectedRequest; route.user_ctx = &binding;
    }
    return httpd_register_uri_handler(model.http, &route) == ESP_OK;
  };
  if (!registerRoute("/", HTTP_GET, page) || !registerRoute("/agents", HTTP_GET, page) ||
      !registerRoute("/settings", HTTP_GET, page) || !registerRoute("/status", HTTP_GET, portalStatus) ||
      !registerRoute("/frame", HTTP_GET, frameGet) ||
      !registerRoute("/frames", HTTP_GET, framesGet) ||
      !registerRoute("/device.png", HTTP_GET, deviceImage) ||
      !registerRoute("/favicon.svg", HTTP_GET, favicon) ||
      !registerRoute("/health", HTTP_GET, health) ||
      !registerRoute("/token-history", HTTP_GET, tokenHistoryGet) ||
      !registerRoute("/logs", HTTP_GET, diagnosticJournal) ||
      !registerRoute("/logs.txt", HTTP_GET, diagnosticJournal) ||
      !registerRoute("/agent", HTTP_POST, telemetryPost)) return failed();
  for (const char *uri : {"/save", "/power", "/brightness", "/external-light", "/buzzer-test", "/buzzer-volume", "/schedule", "/sky-colors", "/quota-rotation", "/agent-color", "/restart", "/update", "/memorial", "/black-hole", "/air-alert"})
    if (!registerRoute(uri, HTTP_POST, commandPost)) return failed();
  for (const char *uri : {"/assets/css/portal.css", "/assets/css/preview.css", "/assets/js/portal.js", "/assets/js/preview.js", "/assets/js/boot.js", "/assets/js/token-history.js"})
    if (!registerRoute(uri, HTTP_GET, staticAsset)) return failed();
  for (const char *uri : {"/agent-update-enroll", "/agent-update-challenge", "/agent-update-poll", "/agent-upgrade", "/agent-rename", "/agent-uninstall"})
    if (!registerRoute(uri, HTTP_POST, agentUpdatePost)) return failed();
  for (const char *uri : {"/agent-update/manifest.json", "/agent-update/package.zip"})
    if (!registerRoute(uri, HTTP_GET, agentUpdateArtifact)) return failed();
  if (!web_auth::registerRoutes(model.http)) return failed();
  // The preview worker survives an HTTP server restart after a failed OTA.
  if (previewStreamReady.load(std::memory_order_acquire)) return true;
  previewStreamQueue = xQueueCreateStatic(1, sizeof(httpd_req_t *), previewStreamQueueBytes, &previewStreamQueueStorage);
  previewStreamReady.store(previewStreamQueue &&
    xTaskCreatePinnedToCore(previewStreamTask, "preview-stream", 8192, nullptr, 2, nullptr, 0) == pdPASS,
    std::memory_order_release);
  return true;
}
