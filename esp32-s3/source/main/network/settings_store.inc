// Private implementation unit; included only by network.cpp.
uint32_t checksum(const Settings &settings) {
  const auto *data = reinterpret_cast<const uint8_t *>(&settings);
  uint32_t crc = UINT32_MAX;
  for (size_t i = 0; i < offsetof(Settings, crc); ++i) {
    crc ^= data[i];
    for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
  }
  return ~crc;
}
bool validSettings(const Settings &s) {
  if (s.magic != kSettingsMagic || s.schema != 1 || s.size != sizeof(Settings) ||
      s.crc != checksum(s) || !std::memchr(s.ssid, 0, sizeof(s.ssid)) ||
      !std::memchr(s.password, 0, sizeof(s.password)) || s.power > 2 ||
      !s.brightness || s.scheduleEnabled > 1 || s.days > 127 || s.start >= 1440 || s.end >= 1440) return false;
  const size_t passwordLength = std::strlen(s.password);
  if (passwordLength && (passwordLength < 8 || passwordLength > 63)) return false;
  for (const auto &entry : s.colors) if (entry.used > 1 || entry.rgb > 0xffffff) return false;
  return true;
}
void loadSettings() {
  // The renderer has not received its first frame yet; no RMT DMA exists at boot.
  const esp_err_t initialized = nvs_flash_init();
  if (initialized != ESP_OK) {
    message("Settings unavailable; existing NVS was not erased.");
    ESP_LOGE(kTag, "NVS init: %s; refusing automatic erase", esp_err_to_name(initialized));
    return;
  }
  model.nvsReady = true;
  nvs_handle_t handle;
  esp_err_t result = nvs_open("led_forge_s3", NVS_READONLY, &handle);
  if (result == ESP_ERR_NVS_NOT_FOUND) return;
  if (result != ESP_OK) { model.nvsReady = false; message("Settings namespace could not be read."); return; }
  Settings stored;
  size_t size = sizeof(stored);
  result = nvs_get_blob(handle, "settings", &stored, &size);
  nvs_close(handle);
  if (result == ESP_ERR_NVS_NOT_FOUND) return;
  if (result == ESP_OK && size == sizeof(stored) && validSettings(stored)) model.settings = stored;
  else { model.nvsReady = false; message("Settings are invalid; existing NVS was preserved."); }
}
void loadAgentUpdateKey() {
  if (!model.nvsReady) return;
  uint8_t boot[8];
  bootloader_random_enable(); esp_fill_random(boot, sizeof(boot)); bootloader_random_disable();
  for (unsigned i = 0; i < sizeof(boot); ++i) std::snprintf(agentUpdateBoot + i * 2, 3, "%02x", boot[i]);
  nvs_handle_t handle;
  if (nvs_open("led_forge_s3", NVS_READWRITE, &handle) != ESP_OK) return;
  size_t length = sizeof(agentUpdateKey);
  const esp_err_t result = nvs_get_str(handle, "agent_admin", agentUpdateKey, &length);
  if (result == ESP_ERR_NVS_NOT_FOUND) {
    uint8_t random[16];
    bootloader_random_enable(); esp_fill_random(random, sizeof(random)); bootloader_random_disable();
    for (unsigned i = 0; i < sizeof(random); ++i) std::snprintf(agentUpdateKey + i * 2, 3, "%02x", random[i]);
    if (nvs_set_str(handle, "agent_admin", agentUpdateKey) != ESP_OK || nvs_commit(handle) != ESP_OK) agentUpdateKey[0] = 0;
  } else if (result != ESP_OK || length != sizeof(agentUpdateKey)) agentUpdateKey[0] = 0;
  if (agentUpdateKey[0]) for (unsigned i = 0; i < 32; ++i)
    if (!((agentUpdateKey[i] >= '0' && agentUpdateKey[i] <= '9') || (agentUpdateKey[i] >= 'a' && agentUpdateKey[i] <= 'f'))) { agentUpdateKey[0] = 0; break; }
  nvs_close(handle);
  // Internal channel seed only; enrollment no longer needs an operator key.
}
bool knownAgent(const ControlFrame &control, uint32_t id) {
  for (const auto &agent : control.agents) if (id && agent.clientId == id && agent.present) return true;
  return false;
}
bool uninstallAcknowledgmentWindow(const agent_update::Table &table, uint32_t id, uint32_t now) {
  const auto *entry = table.find(id);
  return entry && entry->action == agent_update::Action::Uninstall && entry->actionState >= 1 &&
      entry->actionState <= 3 && now - entry->actionAt < agent_update::requestMs;
}
void loadWeekly() {
  if (!model.nvsReady) return;
  nvs_handle_t handle;
  const esp_err_t opened = nvs_open("led_forge_s3", NVS_READONLY, &handle);
  if (opened == ESP_ERR_NVS_NOT_FOUND) return;
  if (opened != ESP_OK) {
    model.weeklySaveFailed = model.lifetimeSaveFailed = model.rankingReadFailed = true; return;
  }
  weekly::RankingStore rankings;
  size_t size = sizeof(rankings);
  const esp_err_t result = nvs_get_blob(handle, "token_rankings", &rankings, &size);
  if (result == ESP_ERR_NVS_NOT_FOUND) {
    // Preserve the existing weekly history, without inventing lifetime totals.
    weekly::Store stored;
    size = sizeof(stored);
    const esp_err_t legacy = nvs_get_blob(handle, "weekly_tokens", &stored, &size);
    if (legacy == ESP_OK && size == sizeof(stored) && weekly::valid(stored)) model.weekly = stored;
    else if (legacy != ESP_ERR_NVS_NOT_FOUND) {
      model.weeklySaveFailed = true; model.rankingReadFailed = true;
    }
  } else if (result == ESP_OK && size == sizeof(rankings) && weekly::valid(rankings)) {
    model.weekly = rankings.week;
    model.lifetime = rankings.total;
  } else {
    // Do not overwrite corrupt authoritative history with empty defaults.
    model.weeklySaveFailed = model.lifetimeSaveFailed = true;
    model.rankingReadFailed = true;
  }
  nvs_close(handle);
}
bool saveWeekly() {
  if (!model.weeklyDirty && !model.lifetimeDirty) return true;
  if (!model.nvsReady || model.rankingReadFailed) {
    model.weeklySaveFailed = model.lifetimeSaveFailed = true; return false;
  }
  weekly::RankingStore stored;
  stored.week = model.weekly; stored.total = model.lifetime;
  stored.week.crc = weekly::checksum(stored.week);
  stored.total.crc = weekly::checksum(stored.total);
  stored.crc = weekly::checksum(stored);
  nvs_handle_t handle;
  esp_err_t result = nvs_open("led_forge_s3", NVS_READWRITE, &handle);
  if (result == ESP_OK) {
    result = nvs_set_blob(handle, "token_rankings", &stored, sizeof(stored));
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
  }
  model.weeklySaveFailed = model.lifetimeSaveFailed = result != ESP_OK;
  if (result != ESP_OK) { model.weeklyDirty = model.lifetimeDirty = true; return false; }
  model.weeklyDirty = model.lifetimeDirty = false;
  model.weeklySavedAt = uptimeMs();
  return true;
}
void loadMemorialSettings() {
  if (!model.nvsReady) return;
  nvs_handle_t handle;
  const esp_err_t opened = nvs_open("led_forge_s3", NVS_READONLY, &handle);
  if (opened == ESP_ERR_NVS_NOT_FOUND) return;
  if (opened != ESP_OK) { model.memorialSaveFailed = true; return; }
  uint32_t stored = 0; size_t size = sizeof(stored);
  const esp_err_t result = nvs_get_blob(handle, "memorial", &stored, &size);
  nvs_close(handle);
  if (result == ESP_ERR_NVS_NOT_FOUND) return;
  if (result == ESP_OK && size == sizeof(stored) && (stored & ~1U) == 0x4d450100U)
    model.memorialEnabled = (stored & 1U) != 0;
  else model.memorialSaveFailed = true;
}
bool saveMemorialSettings() {
  if (!model.memorialDirty) return true;
  if (!model.nvsReady) { model.memorialSaveFailed = true; return false; }
  const uint32_t stored = 0x4d450100U | unsigned(model.memorialEnabled);
  nvs_handle_t handle;
  esp_err_t result = nvs_open("led_forge_s3", NVS_READWRITE, &handle);
  if (result == ESP_OK) {
    result = nvs_set_blob(handle, "memorial", &stored, sizeof(stored));
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
  }
  model.memorialSaveFailed = result != ESP_OK;
  if (result != ESP_OK) return false;
  model.memorialDirty = false;
  return true;
}
void loadAirAlertSettings() {
  if (!model.nvsReady) return;
  nvs_handle_t handle;
  const auto opened = nvs_open("led_forge_s3", NVS_READONLY, &handle);
  if (opened == ESP_ERR_NVS_NOT_FOUND) return;
  if (opened != ESP_OK) { model.airSaveFailed = true; return; }
  AirAlertSettings stored; size_t size = sizeof(stored);
  const auto result = nvs_get_blob(handle, "air_alert", &stored, &size);
  nvs_close(handle);
  if (result == ESP_ERR_NVS_NOT_FOUND) return;
  if (result != ESP_OK || size != sizeof(stored) || stored.magic != 0x414c0100 ||
      stored.enabled > 1) {
    model.airSaveFailed = true; return;
  }
  model.airSettings = stored;
  for (char value : stored.retiredToken) if (value) { model.airDirty = true; break; }
  std::memset(model.airSettings.retiredToken, 0, sizeof(model.airSettings.retiredToken));
  std::memset(stored.retiredToken, 0, sizeof(stored.retiredToken));
}
void loadTelegramAlertState() {
  if (!model.nvsReady) return;
  nvs_handle_t handle;
  if (nvs_open("led_forge_s3", NVS_READONLY, &handle) != ESP_OK) return;
  air_alert::TelegramRecord stored; size_t size = sizeof(stored);
  const auto result = nvs_get_blob(handle, "tg_alert", &stored, &size);
  nvs_close(handle);
  if (result == ESP_ERR_NVS_NOT_FOUND) return;
  if (result != ESP_OK || size != sizeof(stored) || !air_alert::valid(stored)) {
    model.airSaveFailed = true; return;
  }
  model.air.telegramHeadId = stored.headId;
  model.airCheckpointHead = stored.headId;
  model.air.telegramMessageId = stored.messageId;
  model.air.telegramMessageTime = stored.messageTime;
  model.air.last = air_alert::Level(stored.level);
  model.air.seen = true;
  model.air.freshnessExpired = true;
  model.air.checkedAt = uptimeMs();
}
bool saveAirAlertSettings() {
  if (!model.airDirty) return true;
  if (!model.nvsReady) { model.airSaveFailed = true; return false; }
  nvs_handle_t handle;
  esp_err_t result = nvs_open("led_forge_s3", NVS_READWRITE, &handle);
  if (result == ESP_OK) {
    result = nvs_set_blob(handle, "air_alert", &model.airSettings, sizeof(model.airSettings));
    if (result == ESP_OK && model.air.telegramMessageId) {
      air_alert::TelegramRecord stored;
      stored.headId = model.air.telegramHeadId; stored.messageId = model.air.telegramMessageId;
      stored.messageTime = uint32_t(model.air.telegramMessageTime); stored.level = uint32_t(model.air.last);
      stored.crc = air_alert::checksum(stored);
      result = air_alert::valid(stored) ? nvs_set_blob(handle, "tg_alert", &stored, sizeof(stored)) : ESP_FAIL;
    }
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
  }
  model.airSaveFailed = result != ESP_OK;
  if (result != ESP_OK) return false;
  model.airDirty = false;
  model.airCheckpointSavedAt = uptimeMs();
  model.airCheckpointHead = model.air.telegramHeadId;
  return true;
}
bool backgroundTlsReady() {
  // Finish in-flight TLS normally, but do not queue another CPU-heavy handshake
  // while a password operation is admitted. Deferral never means alert clear.
  if (web_auth::busy()) return false;
  constexpr unsigned caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  return heap_caps_get_free_size(caps) >= 32768 && heap_caps_get_largest_free_block(caps) >= 16384;
}
bool scheduledOn() {
  const Settings &s = model.settings;
  if (s.power == 1) return true;
  if (s.power == 2) return false;
  if (!s.scheduleEnabled) return true;
  const time_t now = std::time(nullptr);
  if (now < 1700000000) return false;
  tm local{}; localtime_r(&now, &local);
  const unsigned minute = unsigned(local.tm_hour * 60 + local.tm_min);
  const unsigned day = unsigned(local.tm_wday);
  if (s.start == s.end) return (s.days & (1U << day)) != 0;
  if (s.start < s.end) return (s.days & (1U << day)) && minute >= s.start && minute < s.end;
  if (minute >= s.start) return (s.days & (1U << day)) != 0;
  return minute < s.end && (s.days & (1U << ((day + 6) % 7)));
}
void serviceAirAlert() {
  const uint32_t now = uptimeMs();
  model.air.expire(now);
  model.city.expire(now, int64_t(std::time(nullptr)));
  // A completion published during this drain must be applied before a new job.
  const bool workerWasBusy = air_alert::busy();
  air_alert::PollResult result;
  while (air_alert::receive(result)) {
    if (result.generation != model.airGeneration) continue;
    if (result.cityThreat) {
      model.city.observe(result, now, int64_t(std::time(nullptr)));
      model.cityNextAt = now + std::min(uint32_t(86400000), std::max(kAirPollIntervalMs, result.retryAfterMs));
      if (!result.valid) {
        diagnostic_log::Snapshot failure;
        failure.reason = result.httpStatus; failure.detail = 100 + int32_t(result.failure);
        failure.elapsed_ms = result.elapsedMs;
        diagnostics::event(diagnostics::Level::Warning, diagnostics::Module::Alert,
                           diagnostics::Code::AlertSourceFailed, failure);
      }
      continue;
    }
    const uint32_t previousMessage = model.air.telegramMessageId;
    const bool accepted = model.air.accept(result, model.airGeneration, now);
    if (accepted && result.level == air_alert::Level::Clear) model.city.expired = true;
    model.airHistory.observe(result, accepted, now);
    if (accepted && result.messageId &&
        (result.messageId != previousMessage ||
         (result.headId != model.airCheckpointHead && now - model.airCheckpointSavedAt >= 300000))) model.airDirty = true;
    model.airHttpStatus = result.httpStatus;
    model.airFailure = !accepted && result.failure == air_alert::Failure::None
        ? air_alert::Failure::Payload : result.failure;
    model.airPollMs = result.elapsedMs;
    model.airTlsError = result.tlsError; model.airTlsCode = result.tlsCode; model.airTlsFlags = result.tlsFlags;
    model.airFetchFailed = !accepted;
    if (!accepted) {
      diagnostic_log::Snapshot failure;
      failure.reason = result.httpStatus; failure.detail = int32_t(model.airFailure);
      failure.elapsed_ms = result.elapsedMs;
      diagnostics::event(diagnostics::Level::Warning, diagnostics::Module::Alert,
                         diagnostics::Code::AlertSourceFailed, failure);
    }
    model.airNextAt = now + std::min(uint32_t(86400000), std::max(kAirPollIntervalMs, result.retryAfterMs));
  }
  if (!scheduledOn() || !model.airWorkerReady || !model.airSettings.enabled ||
      !model.connected || model.restarting || model.manifestBusy || model.manifestReady || model.updateRequested ||
      model.control.maintenance != Maintenance::None || workerWasBusy) return;
  const bool municipalDue = due(now, model.airNextAt);
  const bool secondaryDue = due(now, model.cityNextAt);
  if (!municipalDue && !secondaryDue) return;
  // Give an overdue municipal poll the worker first. Admit secondary work
  // only after persistence and with enough time before the next municipal poll.
  if (!municipalDue && (model.airDirty || model.airNextAt - now <= air_alert::kPollBudgetMs)) return;
  if (!backgroundTlsReady()) {
    if (municipalDue) model.airNextAt = now + 5000;
    if (secondaryDue) model.cityNextAt = now + 5000;
    return;
  }
  air_alert::PollJob job{};
  job.cityThreat = !municipalDue;
  job.generation = model.airGeneration;
  job.knownHeadId = model.air.telegramHeadId; job.knownEventId = model.air.telegramMessageId;
  job.knownLevel = model.air.last; job.knownEventTime = model.air.telegramMessageTime;
  if (air_alert::submit(job)) {
    if (job.cityThreat) model.cityNextAt = now + kAirPollIntervalMs;
    else model.airNextAt = now + kAirPollIntervalMs;
  }
}
const char *airLevelName(uint8_t level) {
  return level == 1 ? "yellow" : level == 2 ? "red" : level == 4 ? "untyped" : level == 0 ? "clear" : "unknown";
}
void fillAirAlertView(View &view) {
  const uint32_t now = uptimeMs();
  view.airEnabled = model.airSettings.enabled;
  view.cityFresh = model.city.fresh(now);
  view.cityFailed = model.city.failed;
  view.cityDemo = model.city.demo;
  view.cityReported = model.city.messageId && !model.city.expired;
  view.airManual = model.air.manual(now);
  view.airFresh = model.air.fresh(now, model.connected, !model.airFetchFailed);
  view.airSaveFailed = model.airSaveFailed;
  view.airLevel = model.control.airAlertLevel;
  view.airOfficialLevel = uint8_t(view.airEnabled && (view.airFresh || air_alert::active(model.air.last)) ?
      model.air.last : air_alert::Level::Unknown);
  view.airFailure = model.airFailure;
  view.airHttpStatus = model.airHttpStatus;
  view.airPollMs = model.airPollMs;
  view.airTlsError = model.airTlsError; view.airTlsCode = model.airTlsCode; view.airTlsFlags = model.airTlsFlags;
  view.airRemaining = model.air.remainingSeconds(now);
  view.airMessageId = model.air.telegramMessageId;
  view.airHistory = model.airHistory;
  view.airAge = model.air.seen ? (now - model.air.checkedAt) / 1000 : UINT32_MAX;
  copy(view.airStatus, !view.airEnabled && !view.airManual ? portal_strings::off :
      view.airLevel == 2 ? "Ракетна загроза" : view.airLevel == 1 ? "Дронова загроза" :
      view.airLevel == 4 ? "Тривога: тип загрози невідомий" :
      view.airLevel == 0 ? "Тривогу не оголошено" : "Немає актуальних даних");
  copy(view.airSource, !view.airEnabled ? "Автоперевірку вимкнено" :
      !model.connected ? "Немає Wi-Fi" :
      !model.airWorkerReady ? "Сервіс перевірки недоступний" :
      model.airHttpStatus == 401 || model.airHttpStatus == 403 ? "Telegram: доступ тимчасово недоступний" :
      model.airHttpStatus == 429 ? "Ліміт запитів: очікування повтору" :
      model.airFetchFailed && model.airFailure == air_alert::Failure::Memory ? "Недостатньо пам’яті для перевірки" :
      model.airFetchFailed && model.airFailure == air_alert::Failure::Tls ? "Помилка захищеного з’єднання HTTPS" :
      model.airFetchFailed && model.airFailure == air_alert::Failure::Dns ? "Не вдалося знайти сервер Telegram" :
      model.airFetchFailed && model.airFailure == air_alert::Failure::Timeout ? "Перевищено час очікування Telegram" :
      model.airFetchFailed ? "Помилка оновлення" :
      view.airFresh ? "Дані Telegram отримано" : model.air.seen ? "Дані застаріли" : "Очікування Telegram");
  copy(view.airDetails, model.airSaveFailed ? "Налаштування не збережено. Спробуйте ще раз." :
      model.airDirty ? "Збереження…" : !model.control.on ? "Матриця вимкнена за розкладом або вручну." :
      view.airEnabled && !view.airFresh ? "Немає актуальних даних. Це не відбій тривоги." :
      view.airManual ? "Тимчасова перевірка. Реальна тривога має пріоритет." : "");
}
memorial::Clock memorialClock() {
  memorial::Clock clock;
  const time_t now = std::time(nullptr);
  if (now < 1700000000) return clock;
  tm local{}; localtime_r(&now, &local);
  clock.date = uint32_t((local.tm_year + 1900) * 10000 + (local.tm_mon + 1) * 100 + local.tm_mday);
  clock.hour = uint8_t(local.tm_hour); clock.minute = uint8_t(local.tm_min); clock.second = uint8_t(local.tm_sec);
  clock.valid = true;
  return clock;
}
uint32_t currentWeek() {
  const time_t now = std::time(nullptr);
  if (now < 1700000000) return 0;
  tm local{};
  localtime_r(&now, &local);
  local.tm_mday -= (local.tm_wday + 6) % 7;
  local.tm_hour = 12; local.tm_min = local.tm_sec = 0; local.tm_isdst = -1;
  if (mktime(&local) == time_t(-1)) return 0;
  return uint32_t((local.tm_year + 1900) * 10000 + (local.tm_mon + 1) * 100 + local.tm_mday);
}
void serviceWeekly() {
  if (weekly::advance(model.weekly, currentWeek())) model.weeklyDirty = true;
}
bool saveSettings() {
  if (!model.nvsReady) { message("Cannot save settings; NVS requires repair."); return false; }
  Settings snapshot = model.settings;
  snapshot.crc = checksum(snapshot);
  nvs_handle_t handle;
  esp_err_t result = nvs_open("led_forge_s3", NVS_READWRITE, &handle);
  if (result == ESP_OK) {
    result = nvs_set_blob(handle, "settings", &snapshot, sizeof(snapshot));
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
  }
  if (result != ESP_OK) { message("Settings could not be saved; retrying."); ESP_LOGE(kTag, "NVS save: %s", esp_err_to_name(result)); return false; }
  model.dirty = false;
  message("Settings saved.");
  return true;
}
void loadExternalLight() {
  if (!model.nvsReady) return;
  nvs_handle_t handle;
  if (nvs_open("led_forge_s3", NVS_READONLY, &handle) != ESP_OK) return;
  external_light::Settings stored;
  size_t size = sizeof(stored);
  const auto result = nvs_get_blob(handle, "exterior", &stored, &size);
  nvs_close(handle);
  if (result == ESP_OK && size == sizeof(stored) && external_light::valid(stored)) model.light = stored;
  else if (result != ESP_ERR_NVS_NOT_FOUND) message("External lighting settings invalid; outputs remain off.");
}
bool saveExternalLight() {
  if (!model.lightDirty) return true;
  external_light::Settings stored = model.light;
  external_light::seal(stored);
  nvs_handle_t handle;
  esp_err_t result = model.nvsReady ? nvs_open("led_forge_s3", NVS_READWRITE, &handle) : ESP_FAIL;
  if (result == ESP_OK) {
    result = nvs_set_blob(handle, "exterior", &stored, sizeof(stored));
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
  }
  model.lightSaveFailed = result != ESP_OK;
  if (model.lightSaveFailed) return false;
  model.lightDirty = false;
  return true;
}
void serviceExternalLight() {
  // PWM is owned by Core 1, synchronized with successfully transmitted frames.
  model.lightAllowed = external_light::ready() && model.render.lightAllowed;
}
void loadSkyColors() {
  if (!model.nvsReady) return;
  nvs_handle_t handle;
  if (nvs_open("led_forge_s3", NVS_READONLY, &handle) != ESP_OK) return;
  uint32_t stored[3] = {}; size_t size = sizeof(stored);
  const auto result = nvs_get_blob(handle, "skypalette", stored, &size);
  uint8_t minutes = 5;
  if (nvs_get_u8(handle, "quota_rotation", &minutes) == ESP_OK && minutes >= 1 && minutes <= 60)
    model.control.quotaRotationMinutes = minutes;
  nvs_close(handle);
  if (result == ESP_OK && size == sizeof(stored) && stored[0] == 1 &&
      stored[1] <= 0xffffff && stored[2] <= 0xffffff) {
    model.control.codexSkyColor = stored[1];
    model.control.claudeSkyColor = stored[2];
  }
}
bool saveSkyColors() {
  if (!model.skyColorsDirty) return true;
  const uint32_t stored[3] = {1, model.control.codexSkyColor, model.control.claudeSkyColor};
  nvs_handle_t handle;
  esp_err_t result = model.nvsReady ? nvs_open("led_forge_s3", NVS_READWRITE, &handle) : ESP_FAIL;
  if (result == ESP_OK) {
    result = nvs_set_blob(handle, "skypalette", stored, sizeof(stored));
    if (result == ESP_OK) result = nvs_set_u8(handle, "quota_rotation", model.control.quotaRotationMinutes);
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
  }
  if (result != ESP_OK) { message("Display settings could not be saved; retrying."); return false; }
  model.skyColorsDirty = false;
  return true;
}
void loadBuzzerVolume() {
  if (!model.nvsReady) return;
  nvs_handle_t handle;
  if (nvs_open("led_forge_s3", NVS_READONLY, &handle) != ESP_OK) return;
  uint8_t volume = 100; size_t size = sizeof(volume);
  const auto result = nvs_get_blob(handle, "buzzer_volume", &volume, &size);
  nvs_close(handle);
  if (result == ESP_OK && size == sizeof(volume) && volume <= 100) model.buzzerVolume = volume;
}
bool saveBuzzerVolume() {
  if (!model.buzzerDirty) return true;
  nvs_handle_t handle;
  esp_err_t result = model.nvsReady ? nvs_open("led_forge_s3", NVS_READWRITE, &handle) : ESP_FAIL;
  if (result == ESP_OK) {
    result = nvs_set_blob(handle, "buzzer_volume", &model.buzzerVolume, sizeof(model.buzzerVolume));
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
  }
  model.buzzerSaveFailed = result != ESP_OK;
  if (model.buzzerSaveFailed) return false;
  model.buzzerDirty = false;
  return true;
}
void serviceBuzzer() {
  const uint32_t now = uptimeMs();
  const bool allowed = !model.restarting && model.control.maintenance == Maintenance::None;
  buzzer::service(now, allowed);
  if (model.restarting || model.control.maintenance == Maintenance::Ota || model.control.maintenance == Maintenance::Restart)
    model.buzzerPending = false;
  if (allowed && model.buzzerPending) {
    model.buzzerPending = false;
    (void)buzzer::start(now, model.buzzerVolume);
  }
  const bool cityReported = !model.city.demo && model.city.active() &&
      (!model.airSettings.enabled ||
      air_alert::active(model.air.last));
  const bool cityAllowed = allowed && model.control.on &&
      (model.airSettings.enabled || model.city.demo);
  if (model.citySound.update(model.city.messageId, cityReported, model.city.demo,
                            model.city.demoAt, cityAllowed, now))
    // A simultaneous manual beep coalesces with this pulse; never queue catch-up audio.
    (void)buzzer::start(now, model.buzzerVolume);
}
const ColorOverride *findOverride(uint32_t id) {
  for (const auto &entry : model.settings.colors) if (entry.used && entry.clientId == id) return &entry;
  return nullptr;
}
void dirty() { if (!model.dirty) model.dirtyAt = uptimeMs(); model.dirty = true; }
