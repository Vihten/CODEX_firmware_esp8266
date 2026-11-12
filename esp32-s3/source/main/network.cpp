#include "runtime.hpp"
#include "web_files.hpp"
#include "web_auth.hpp"
#include "preview.hpp"
#include "network_progress.hpp"
#include "recovery.hpp"
#include "esp_attr.h"
#include "diagnostics.hpp"
#include "diagnostic_persistence.hpp"
#include "diagnostic_http.hpp"
#include "weekly.hpp"
#include "quota_filter.hpp"
#include "daily_runtime.hpp"
#include "agent_update.hpp"
#include "agent_release.hpp"
#include "memorial.hpp"
#include "air_alert.hpp"
#include "city_presence.hpp"
#include "air_alert_history.hpp"
#include "external_light.hpp"
#include "buzzer.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <ctime>
#include <fcntl.h>
#include <strings.h>
#include <unistd.h>
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_random.h"
#include "bootloader_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "esp_sntp.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "psa/crypto.h"

namespace {
constexpr char kTag[] = "network";
portMUX_TYPE networkProgressMux = portMUX_INITIALIZER_UNLOCKED;
network_progress::Progress networkProgress;

void markNetworkProgress(network_progress::Stage stage) {
  portENTER_CRITICAL(&networkProgressMux);
  networkProgress.enter(stage, uptimeMs());
  portEXIT_CRITICAL(&networkProgressMux);
}

void completeNetworkProgress() {
  portENTER_CRITICAL(&networkProgressMux);
  networkProgress.completeLoop(uptimeMs());
  portEXIT_CRITICAL(&networkProgressMux);
}

network_progress::Heartbeat snapshotNetworkProgress() {
  portENTER_CRITICAL(&networkProgressMux);
  const auto progress = networkProgress.snapshot();
  portEXIT_CRITICAL(&networkProgressMux);
  return progress;
}

constexpr char kFirmwareUrl[] = "https://api.github.com/repos/Vihten/CODEX_firmware_esp8266/contents/esp32-s3/firmware.bin?ref=main";
constexpr uint32_t kAgentTimeout = 7000, kMaintenanceTimeout = 10000;
constexpr uint32_t kOtaInterval = 15 * 60 * 1000;
constexpr uint32_t kAirPollIntervalMs = 15000;
constexpr uint16_t kUdpPort = 4210;
constexpr uint32_t kSettingsMagic = 0x4c465333;
struct ColorOverride { uint32_t clientId; uint32_t rgb; uint8_t used; uint8_t reserved[3]; };
struct AirAlertSettings {
  uint32_t magic = 0x414c0100;
  uint8_t enabled = 0;
  // Retain the existing NVS layout; retire and erase the former provider secret.
  char retiredToken[129] = {};
};
struct Settings {
  uint32_t magic = kSettingsMagic;
  uint16_t schema = 1;
  uint16_t size = sizeof(Settings);
  char ssid[33] = {};
  char password[65] = {};
  uint8_t power = 0, brightness = PROJECT_BRIGHTNESS, scheduleEnabled = 0, days = 0x7f;
  uint16_t start = 8 * 60, end = 23 * 60;
  ColorOverride colors[kAgentCapacity] = {};
  uint32_t crc = 0;
};
struct Manifest {
  uint64_t version = 0;
  size_t size = 0;
  uint8_t sha[32] = {};
};
struct ManifestJob {
  char url[256] = {};
  uint64_t currentVersion = PROJECT_FIRMWARE_VERSION;
  uint32_t partitionSize = 0;
};
struct ManifestResult {
  Manifest manifest;
  bool newer = false;
  bool upToDate = false;
  char status[96] = {};
};
struct WifiEvent {
  enum Kind : uint8_t { Started, Disconnected, GotIp } kind;
  uint16_t reason = 0;
  esp_netif_ip_info_t ip = {};
};
struct TelemetryRequest { uint32_t id; uint32_t receivedAt; char packet[200]; };
struct TelemetryReply { uint32_t id; bool accepted; char ack[64]; };
struct View {
  bool buzzerReady = false;
  uint8_t buzzerVolume = 100;
  bool buzzerDirty = false, buzzerSaveFailed = false;
  external_light::Settings light;
  bool lightReady = false, lightDirty = false, lightSaveFailed = false, lightAllowed = false;
  agent_update::Table agentUpdates;
  bool agentUpdateKeyReady = false;
  weekly::Store weekly;
  weekly::Store lifetime;
  bool lifetimeDirty = false, lifetimeSaveFailed = false;
  bool weeklyDirty = false, weeklySaveFailed = false;
  ControlFrame control;
  RenderStatus render;
  char ssid[33] = {}, ap[32] = {}, message[96] = {}, ota[96] = {};
  uint32_t deviceId = 0, maintenanceTimeouts = 0, udpPackets = 0, otaDone = 0, otaTotal = 0;
  uint32_t netStack = 0, networkLoops = 0, disconnects = 0;
  uint32_t recoveryOfflineMs = 0;
  uint16_t recoveryCode = 0;
  uint8_t recoveryStage = 0, recoveryStacks = 0, recoveryFaults = 0;
  int rssi = -127;
  uint16_t start = 0, end = 0, disconnectReason = 0;
  uint8_t power = 0, days = 0;
  bool scheduleEnabled = false, dirty = false, nvsReady = false, restarting = false;
  bool memorialEnabled = false, memorialManual = false, memorialSaveFailed = false;
  bool airEnabled = false, airManual = false, airFresh = false, airSaveFailed = false;
  bool cityFresh = false, cityFailed = false, cityDemo = false, cityReported = false;
  uint8_t airLevel = 0;
  air_alert::Failure airFailure = air_alert::Failure::None;
  int airHttpStatus = 0;
  uint32_t airPollMs = 0;
  int airTlsError = 0, airTlsCode = 0, airTlsFlags = 0;
  uint32_t airAge = UINT32_MAX, airRemaining = 0;
  uint32_t airMessageId = 0;
  air_alert::FailureHistory airHistory;
  char airStatus[96] = {}, airDetails[160] = {}, airSource[96] = {};
  bool overrideColor[kAgentCapacity] = {};
  ColorOverride savedColors[kAgentCapacity] = {};
};
struct Model {
  uint8_t buzzerVolume = 100;
  bool buzzerPending = false;
  bool buzzerDirty = false, buzzerSaveFailed = false;
  uint32_t buzzerDirtyAt = 0;
  external_light::Settings light;
  bool lightDirty = false, lightSaveFailed = false, lightAllowed = false;
  agent_update::Table agentUpdates;
  uint64_t actionSequence = 0;
  weekly::Store weekly;
  weekly::Store lifetime;
  bool lifetimeDirty = false, lifetimeSaveFailed = false;
  bool weeklyDirty = false, weeklySaveFailed = false;
  bool rankingReadFailed = false;
  uint32_t weeklySavedAt = 0;
  Settings settings;
  bool skyColorsDirty = false;
  bool blackHolePreview = false;
  uint32_t blackHolePreviewStartedAt = 0;
  memorial::State memorial;
  bool memorialEnabled = false, memorialDirty = false, memorialSaveFailed = false;
  AirAlertSettings airSettings;
  air_alert::State air;
  city_presence::State city;
  uint32_t cityNextAt = 0;
  bool airDirty = false, airSaveFailed = false, airWorkerReady = false, airFetchFailed = false;
  uint32_t airGeneration = 1, airNextAt = 0;
  uint32_t airCheckpointSavedAt = 0;
  uint32_t airCheckpointHead = 0;
  int airHttpStatus = 0;
  air_alert::Failure airFailure = air_alert::Failure::None;
  uint32_t airPollMs = 0;
  int airTlsError = 0, airTlsCode = 0, airTlsFlags = 0;
  air_alert::FailureHistory airHistory;
  ControlFrame control;
  RenderStatus render;
  Manifest manifest;
  esp_netif_t *sta = nullptr, *ap = nullptr;
  httpd_handle_t http = nullptr;
  int udp = -1, rssi = -127;
  uint32_t deviceId = 0, maintenanceAt = 0, persistRetryAt = 0, dirtyAt = 0;
  uint32_t restartAt = 0, otaAt = 0, stationAt = 0, stationRetryAt = 0, discoveryAt = 0;
  uint32_t legacyAt = 0, maintenanceTimeouts = 0, udpPackets = 0, disconnects = 0, loops = 0;
  uint32_t otaDone = 0, otaTotal = 0, requestedRgb[kAgentCapacity] = {};
  esp_ota_handle_t otaAbortHandle = 0;
  bool otaVisualTimedOut = false, otaBootSelected = false;
  uint32_t wifiSampleAt = 0, wifiEventsDropped = 0, wifiReconciledLosses = 0;
  uint8_t wifiNegativeSamples = 0;
  bool wifiSampled = false;
  bool wifiInitialized = false, wifiDriverStarted = false, recoveryRestart = false;
  recovery::Wifi wifiRecovery;
  uint16_t recoveryCode = 0;
  uint16_t disconnectReason = 0;
  uint8_t legacyActivity = 0, legacyRemaining = 100, stationAttempts = 0;
  bool legacySeen = false, directSeen = false, linked = false, connected = false;
  bool quotaResetReported = false;
  quota_provider::State codexQuota, claudeQuota;
  quota_provider::Rotation quotaRotation;
  quota_provider::Sample codexSamples[kAgentCapacity], claudeSamples[kAgentCapacity];
  bool nvsReady = false, dirty = false, wifiChanged = false, stationConnecting = false;
  bool restarting = false, updateRequested = false, manifestReady = false;
  bool releasePending = false, confirmationPending = false, sntpStarted = false;
  bool manifestBusy = false, manifestWorkerReady = false, wifiReady = false, httpReady = false;
  char apName[32] = {}, message[96] = {}, ota[96] = "Not checked";
} model;

recovery::Gate recoveryGate;
RTC_NOINIT_ATTR recovery::Retained retainedRecovery;
recovery::Budget recoveryBudget;
portMUX_TYPE recoveryBudgetMux = portMUX_INITIALIZER_UNLOCKED;
std::atomic<bool> recoveryBudgetAvailable{false};
std::atomic<uint32_t> recoveryBudgetStatus{0};
std::atomic<uint32_t> recoveryManualAt{0};
std::atomic<bool> recoveryManualGrace{false};

bool reserveRecoveryRestart(uint16_t reason = recovery::queuedCode) {
  portENTER_CRITICAL(&recoveryBudgetMux);
  const bool reserved = recoveryBudget.consume(retainedRecovery, reason);
  recoveryBudgetStatus.store((recovery::valid(retainedRecovery) ? 1U : 0U) |
      (retainedRecovery.used << 1) | (retainedRecovery.reason << 16));
  portEXIT_CRITICAL(&recoveryBudgetMux);
  if (reserved) recoveryBudgetAvailable.store(false);
  return reserved;
}
class RecoveryOwner {
 public:
  explicit RecoveryOwner(bool allowProtected = false)
      : borrowed_(allowProtected && recoveryGate.protectedOwner()),
        held_(!borrowed_ && recoveryGate.owner() && recoveryGate.protect()) {}
  explicit operator bool() const { return borrowed_ || held_; }
  ~RecoveryOwner() {
    if (held_) recoveryGate.release(model.control.maintenance != Maintenance::None ||
        model.memorial.active || model.control.memorialActive || model.render.memorialTransition ||
        model.restarting || model.manifestReady || model.confirmationPending || model.releasePending);
  }
 private:
  bool borrowed_;
  bool held_;
};

StaticSemaphore_t viewMutexStorage;
SemaphoreHandle_t viewMutex = nullptr;
View sharedView;
StaticQueue_t wifiEventStorage, telemetryStorage, telemetryReplyStorage;
StaticQueue_t manifestJobStorage, manifestResultStorage;
alignas(4) uint8_t wifiEventBytes[16 * sizeof(WifiEvent)];
alignas(4) uint8_t telemetryBytes[4 * sizeof(TelemetryRequest)];
alignas(4) uint8_t telemetryReplyBytes[4 * sizeof(TelemetryReply)];
alignas(4) uint8_t manifestJobBytes[sizeof(ManifestJob)];
alignas(4) uint8_t manifestResultBytes[sizeof(ManifestResult)];
QueueHandle_t wifiEvents = nullptr, telemetryQueue = nullptr, telemetryReplies = nullptr;
QueueHandle_t manifestJobs = nullptr, manifestResults = nullptr;
std::atomic<uint32_t> requestSequence{0}, activeHandlers{0};
std::atomic<uint32_t> wifiEventDrops{0};
char agentUpdateKey[33] = {};
char agentUpdateBoot[17] = {};

template<size_t N> void copy(char (&target)[N], const char *text) {
  std::snprintf(target, N, "%s", text ? text : "");
}
bool due(uint32_t now, uint32_t deadline) { return static_cast<int32_t>(now - deadline) >= 0; }
void message(const char *text) { copy(model.message, text); }
void otaMessage(const char *text) { copy(model.ota, text); ESP_LOGI(kTag, "%s", text); }
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
  constexpr unsigned caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  return heap_caps_get_free_size(caps) >= 32768 && heap_caps_get_largest_free_block(caps) >= 16384;
}
void serviceAirAlert() {
  const uint32_t now = uptimeMs();
  model.air.expire(now);
  model.city.expire(now, int64_t(std::time(nullptr)));
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
  if (!model.airWorkerReady || !model.airSettings.enabled ||
      !model.connected || model.restarting || model.manifestBusy || model.manifestReady || model.updateRequested ||
      model.control.maintenance != Maintenance::None || air_alert::busy() || (!due(now, model.airNextAt) && !due(now, model.cityNextAt))) return;
  // Persist a new municipal event before starting the secondary TLS request.
  if (model.airDirty && !due(now, model.airNextAt)) return;
  if (!backgroundTlsReady()) { model.airNextAt = model.cityNextAt = now + 5000; return; }
  air_alert::PollJob job{};
  job.cityThreat = !due(now, model.airNextAt);
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
  view.airFailure = model.airFailure;
  view.airHttpStatus = model.airHttpStatus;
  view.airPollMs = model.airPollMs;
  view.airTlsError = model.airTlsError; view.airTlsCode = model.airTlsCode; view.airTlsFlags = model.airTlsFlags;
  view.airRemaining = model.air.remainingSeconds(now);
  view.airMessageId = model.air.telegramMessageId;
  view.airHistory = model.airHistory;
  view.airAge = model.air.seen ? (now - model.air.checkedAt) / 1000 : UINT32_MAX;
  copy(view.airStatus, !view.airEnabled && !view.airManual ? "Вимкнено" :
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
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
  }
  if (result != ESP_OK) { message("Sky colors could not be saved; retrying."); return false; }
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
  const bool allowed = !model.restarting && model.control.maintenance == Maintenance::None;
  buzzer::service(uptimeMs(), allowed);
  if (model.restarting || model.control.maintenance == Maintenance::Ota || model.control.maintenance == Maintenance::Restart)
    model.buzzerPending = false;
  if (allowed && model.buzzerPending) {
    model.buzzerPending = false;
    (void)buzzer::start(uptimeMs(), model.buzzerVolume);
  }
}
const ColorOverride *findOverride(uint32_t id) {
  for (const auto &entry : model.settings.colors) if (entry.used && entry.clientId == id) return &entry;
  return nullptr;
}
void dirty() { if (!model.dirty) model.dirtyAt = uptimeMs(); model.dirty = true; }
int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
bool hexValue(const char *text, size_t minimum, size_t maximum, uint32_t &value) {
  const size_t length = std::strlen(text);
  if (length < minimum || length > maximum) return false;
  value = 0;
  for (size_t i = 0; i < length; ++i) { const int d = hexDigit(text[i]); if (d < 0) return false; value = (value << 4) | uint32_t(d); }
  return true;
}
bool unsignedValue(const char *text, uint64_t maximum, uint64_t &value) {
  if (!text || !*text) return false;
  value = 0;
  for (const char *p = text; *p; ++p) {
    if (*p < '0' || *p > '9' || uint64_t(*p - '0') > maximum || value > (maximum - uint64_t(*p - '0')) / 10) return false;
    value = value * 10 + uint64_t(*p - '0');
  }
  return true;
}
bool percent(const char *text, bool allowUnknown, int &value) {
  if (allowUnknown && !std::strcmp(text, "-1")) { value = -1; return true; }
  uint64_t number;
  if (!unsignedValue(text, 100, number)) return false;
  value = int(number); return true;
}
bool validUtf8(const char *text, size_t length) {
  for (size_t i = 0; i < length;) {
    const uint8_t c = uint8_t(text[i++]);
    if (c < 0x80) { if (c < 0x20 || c == 0x7f) return false; continue; }
    unsigned following; uint32_t point, minimum;
    if (c >= 0xc2 && c <= 0xdf) { following = 1; point = c & 31; minimum = 0x80; }
    else if (c >= 0xe0 && c <= 0xef) { following = 2; point = c & 15; minimum = 0x800; }
    else if (c >= 0xf0 && c <= 0xf4) { following = 3; point = c & 7; minimum = 0x10000; }
    else return false;
    if (i + following > length) return false;
    while (following--) { const uint8_t next = uint8_t(text[i++]); if ((next & 0xc0) != 0x80) return false; point = (point << 6) | (next & 63); }
    if (point < minimum || point > 0x10ffff || (point >= 0xd800 && point <= 0xdfff) || (point >= 0x80 && point <= 0x9f)) return false;
  }
  return true;
}
uint32_t fallbackColor(uint32_t id) {
  static constexpr uint32_t colors[] = {0xff504a, 0xf5d445, 0x4be071, 0x3ad8e9, 0x5575ff, 0xee5bd5, 0xff9845, 0x91f04f};
  return colors[(id ^ (id >> 16)) % 8];
}
int colorHue(uint32_t rgb) {
  const int red = (rgb >> 16) & 255, green = (rgb >> 8) & 255, blue = rgb & 255;
  const int top = std::max({red, green, blue}), delta = top - std::min({red, green, blue});
  if (!delta) return -1;
  int hue = top == red ? 60 * (green - blue) / delta
      : top == green ? 120 + 60 * (blue - red) / delta : 240 + 60 * (red - green) / delta;
  return hue < 0 ? hue + 360 : hue;
}
unsigned hueDistance(uint32_t first, uint32_t second) {
  const int a = colorHue(first), b = colorHue(second);
  if (a < 0 || b < 0) return 180;
  const unsigned distance = unsigned(std::abs(a - b));
  return std::min(distance, 360U - distance);
}
uint32_t hueColor(unsigned hue) {
  const unsigned sector = hue / 60, fraction = (hue % 60) * 255 / 60;
  const unsigned rising = fraction, falling = 255 - fraction;
  switch (sector) {
    case 0: return 0xff0000 | (rising << 8);
    case 1: return (falling << 16) | 0x00ff00;
    case 2: return 0x00ff00 | rising;
    case 3: return (falling << 8) | 0x0000ff;
    case 4: return (rising << 16) | 0x0000ff;
    default: return 0xff0000 | falling;
  }
}
uint32_t distinctColor(uint32_t requested, uint32_t clientId) {
  const uint32_t now = uptimeMs();
  const auto nearest = [&](uint32_t rgb) {
    unsigned distance = 180;
    for (const auto &agent : model.control.agents) {
      if (!agent.present || agent.clientId == clientId || now - agent.seenAt >= kAgentTimeout) continue;
      distance = std::min(distance, hueDistance(rgb, agent.rgb));
    }
    return distance;
  };
  unsigned bestDistance = nearest(requested);
  if (bestDistance >= 35) return requested;
  uint32_t best = requested;
  // Additional hue candidates retain useful separation with eight active owners.
  for (unsigned hue = 0; hue < 360; hue += 10) {
    const uint32_t rgb = hueColor(hue); const unsigned distance = nearest(rgb);
    if (distance > bestDistance) { best = rgb; bestDistance = distance; }
  }
  return best;
}
bool parseTelemetry(const char *packet, AgentInput &agent, bool &legacy, uint8_t &remaining, uint8_t &activity) {
  const size_t length = std::strlen(packet);
  if (!length || length >= 160) return false;
  char buffer[160]; std::memcpy(buffer, packet, length + 1);
  agent = AgentInput{};
  char *fields[8] = {buffer}; size_t count = 1;
  for (size_t i = 0; i < length; ++i) if (buffer[i] == ',') {
    if (count == 8) return false;
    buffer[i] = 0; fields[count++] = buffer + i + 1;
  }
  uint32_t account;
  if (count < 2 || !hexValue(fields[1], 1, 8, account) || account != PROJECT_ACCOUNT_FINGERPRINT) return false;
  int a, r;
  if (!std::strcmp(fields[0], "LFX1")) {
    legacy = false;
    if (length > 64 || count != 5 || std::strlen(fields[1]) != 8 ||
        !hexValue(fields[2], 8, 8, agent.clientId) || !agent.clientId ||
        !percent(fields[3], false, a) || !percent(fields[4], false, r)) return false;
    agent.sourceActivityKnown = true;
    agent.codexActivity = uint8_t(a); agent.claudeActivity = uint8_t(r);
    return true;
  }
  legacy = !std::strcmp(fields[0], "LF2") || !std::strcmp(fields[0], "LF1");
  if (legacy) {
    if (count != 4 || !percent(fields[2], false, r) || !percent(fields[3], false, a)) return false;
    remaining = uint8_t(r); activity = uint8_t(a); return true;
  }
  const bool verifiedProtocol = !std::strcmp(fields[0], "LFA3");
  if ((verifiedProtocol ? count != 8 : (std::strcmp(fields[0], "LFA2") || (count != 5 && count != 7))) ||
      !hexValue(fields[2], 1, 8, agent.clientId) || !agent.clientId || !percent(fields[3], false, a) || !percent(fields[4], true, r)) return false;
  agent.activity = uint8_t(a); agent.remaining = int16_t(r); agent.rgb = fallbackColor(agent.clientId);
  uint32_t quotaAccount = 0;
  if (verifiedProtocol) {
    if (!hexValue(fields[5], 8, 8, quotaAccount)) return false;
    agent.quotaForPairedAccount = quotaAccount == PROJECT_ACCOUNT_FINGERPRINT && r >= 0;
  }
  if (count >= 7) {
    const size_t colorField = verifiedProtocol ? 6 : 5;
    const char *nameHex = fields[colorField + 1];
    if (!hexValue(fields[colorField], 6, 6, agent.rgb)) return false;
    const size_t hexLength = std::strlen(nameHex);
    if (hexLength > 96 || (hexLength & 1)) return false;
    for (size_t i = 0; i < hexLength / 2; ++i) {
      const int high = hexDigit(nameHex[i * 2]), low = hexDigit(nameHex[i * 2 + 1]);
      if (high < 0 || low < 0) return false;
      agent.name[i] = char((high << 4) | low);
    }
    if (!validUtf8(agent.name, hexLength / 2)) return false;
  }
  if (!agent.name[0]) std::snprintf(agent.name, sizeof(agent.name), "%08lX", (unsigned long)agent.clientId);
  return true;
}
bool parseWeeklyTelemetry(const char *packet, weekly::Entry &entry, uint32_t &week,
                          uint64_t *lifetimeTokens = nullptr) {
  const size_t length = std::strlen(packet);
  if (!length || length >= 200) return false;
  entry = weekly::Entry{};
  char buffer[200]; std::memcpy(buffer, packet, length + 1);
  char *fields[9] = {buffer}; size_t count = 1;
  for (size_t i = 0; i < length; ++i) if (buffer[i] == ',') {
    if (count == 9) return false;
    buffer[i] = 0; fields[count++] = buffer + i + 1;
  }
  const bool v2 = !std::strcmp(fields[0], "LFT2");
  const unsigned color = v2 ? 7 : 5, name = color + 1;
  uint32_t account;
  if (count != (v2 ? 9U : 7U) || (!v2 && std::strcmp(fields[0], "LFT1")) ||
      !hexValue(fields[1], 1, 8, account) || account != PROJECT_ACCOUNT_FINGERPRINT ||
      !hexValue(fields[2], 1, 8, entry.clientId) || !entry.clientId ||
      std::strlen(fields[3]) != 8 || !hexValue(fields[color], 6, 6, entry.rgb)) return false;
  week = 0;
  for (const char *c = fields[3]; *c; ++c) {
    if (*c < '0' || *c > '9') return false;
    week = week * 10 + unsigned(*c - '0');
  }
  if (!unsignedValue(fields[4], weekly::maximum, entry.tokens)) return false;
  if (v2) {
    uint64_t all = 0, revision = 0;
    if (!unsignedValue(fields[5], weekly::maximum, all) || all < entry.tokens ||
        !unsignedValue(fields[6], weekly::maxRevision, revision)) return false;
    weekly::setRevision(entry, revision);
    if (lifetimeTokens) *lifetimeTokens = all;
  }
  const size_t nameLength = std::strlen(fields[name]);
  if (nameLength > 96 || (nameLength & 1)) return false;
  for (size_t i = 0; i < nameLength / 2; ++i) {
    const int high = hexDigit(fields[name][i * 2]), low = hexDigit(fields[name][i * 2 + 1]);
    if (high < 0 || low < 0) return false;
    entry.name[i] = char((high << 4) | low);
  }
  if (!validUtf8(entry.name, nameLength / 2)) return false;
  if (!entry.name[0]) std::snprintf(entry.name, sizeof(entry.name), "%08lX", (unsigned long)entry.clientId);
  return true;
}
bool acceptWeeklyTelemetry(const char *packet, char (&ack)[64]) {
  weekly::Entry entry; uint32_t week = 0; uint64_t lifetimeTokens = 0;
  if (model.rankingReadFailed) return false;
  if (!parseWeeklyTelemetry(packet, entry, week, &lifetimeTokens) || !week || week != currentWeek()) return false;
  if (const auto *override = findOverride(entry.clientId)) entry.rgb = override->rgb;
  bool changed = false, allChanged = false;
  if (weekly::v2(entry)) {
    if (!weekly::mergeV2(model.weekly, model.lifetime, week, entry, lifetimeTokens, changed, allChanged)) return false;
  } else {
    // Lifetime's marker persists when the weekly table rolls over.
    for (const auto &all : model.lifetime.entries)
      if (all.clientId == entry.clientId && weekly::v2(all)) {
        std::snprintf(ack, sizeof(ack), "LFT1_ACK,%06lX,%08lX", (unsigned long)model.deviceId, (unsigned long)entry.clientId);
        return true;
      }
    serviceWeekly();
    if (!weekly::merge(model.weekly, week, entry, changed)) return false;
  }
  model.weeklyDirty |= changed;
  model.lifetimeDirty |= allChanged;
  std::snprintf(ack, sizeof(ack), "%s_ACK,%06lX,%08lX", weekly::v2(entry) ? "LFT2" : "LFT1",
      (unsigned long)model.deviceId, (unsigned long)entry.clientId);
  return true;
}
bool acceptTelemetry(const char *packet, char (&ack)[64], uint32_t receivedAt) {
  if (!std::strncmp(packet, "LFD1,", 5)) {
    const auto clock = memorialClock();
    return daily_runtime::accept(packet, clock.valid ? clock.date : 0, model.deviceId, ack);
  }
  if (!std::strncmp(packet, "LFT1,", 5) || !std::strncmp(packet, "LFT2,", 5)) return acceptWeeklyTelemetry(packet, ack);
  if (!std::strncmp(packet, "LFC1,", 5)) {
    quota_provider::Packet incoming;
    if (!quota_provider::parse(packet, PROJECT_ACCOUNT_FINGERPRINT, incoming)) return false;
    for (size_t i = 0; i < kAgentCapacity; ++i) {
      const auto &agent = model.control.agents[i];
      if (!agent.clientId || agent.clientId != incoming.clientId) continue;
      model.claudeSamples[i] = {incoming.clientId, receivedAt, incoming.remaining, incoming.validForMs};
      ++model.udpPackets;
      std::snprintf(ack, sizeof(ack), "LFC1_ACK,%06lX,%08lX", (unsigned long)model.deviceId,
          (unsigned long)incoming.clientId);
      return true;
    }
    return false;
  }
  AgentInput incoming; bool legacy = false; uint8_t remaining = 100, activity = 0;
  if (!parseTelemetry(packet, incoming, legacy, remaining, activity)) return false;
  const uint32_t now = uptimeMs();
  if (incoming.sourceActivityKnown) {
    for (auto &agent : model.control.agents) {
      if (!agent.present || agent.clientId != incoming.clientId || now - agent.seenAt >= kAgentTimeout) continue;
      agent.sourceActivityKnown = true;
      agent.sourceActivityAt = receivedAt;
      agent.codexActivity = incoming.codexActivity;
      agent.claudeActivity = incoming.claudeActivity;
      std::snprintf(ack, sizeof(ack), "LFX1_ACK,%06lX,%08lX", (unsigned long)model.deviceId,
          (unsigned long)incoming.clientId);
      return true;
    }
    return false;
  }
  if (legacy) {
    model.legacySeen = true; model.legacyAt = receivedAt; model.legacyRemaining = remaining; model.legacyActivity = activity;
    std::snprintf(ack, sizeof(ack), "LF1_ACK,%06lX,%d", (unsigned long)model.deviceId, model.rssi);
  } else {
    size_t slot = kAgentCapacity;
    for (size_t i = 0; i < kAgentCapacity; ++i) if (model.control.agents[i].clientId == incoming.clientId &&
        (model.control.agents[i].present || model.control.agents[i].name[0])) { slot = i; break; }
    if (slot == kAgentCapacity) for (size_t i = 0; i < kAgentCapacity; ++i)
      if (!model.control.agents[i].clientId) { slot = i; break; }
    if (slot == kAgentCapacity) {
      uint32_t oldestAge = 0;
      for (size_t i = 0; i < kAgentCapacity; ++i) {
        const auto &agent = model.control.agents[i];
        const uint32_t age = now - agent.seenAt;
        if ((!agent.present || age >= kAgentTimeout) && (slot == kAgentCapacity || age > oldestAge)) {
          slot = i; oldestAge = age;
        }
      }
    }
    if (slot == kAgentCapacity) return false;
    const auto &previous = model.control.agents[slot];
    model.quotaResetReported |= incoming.quotaForPairedAccount && previous.quotaForPairedAccount &&
        previous.present && previous.clientId == incoming.clientId &&
        now - previous.seenAt < kAgentTimeout && previous.remaining >= 0 &&
        previous.remaining < 100 && incoming.remaining == 100;
    const bool sameColor = previous.present && previous.clientId == incoming.clientId &&
        now - previous.seenAt < kAgentTimeout && model.requestedRgb[slot] == incoming.rgb;
    if (previous.clientId == incoming.clientId && previous.sourceActivityKnown &&
        now - previous.sourceActivityAt < 15000) {
      incoming.sourceActivityKnown = true;
      incoming.sourceActivityAt = previous.sourceActivityAt;
      incoming.codexActivity = previous.codexActivity;
      incoming.claudeActivity = previous.claudeActivity;
    }
    incoming.present = true; incoming.seenAt = receivedAt;
    model.requestedRgb[slot] = incoming.rgb;
    if (const auto *entry = findOverride(incoming.clientId)) incoming.rgb = entry->rgb;
    else incoming.rgb = sameColor ? previous.rgb : distinctColor(incoming.rgb, incoming.clientId);
    if (previous.clientId != incoming.clientId) model.claudeSamples[slot] = {};
    model.codexSamples[slot] = {incoming.clientId, receivedAt,
        incoming.quotaForPairedAccount ? incoming.remaining : int16_t(-1)};
    model.control.agents[slot] = incoming;
    model.directSeen = true;
    std::snprintf(ack, sizeof(ack), "LFA2_ACK,%06lX,%08lX,%06lX", (unsigned long)model.deviceId,
                  (unsigned long)incoming.clientId, (unsigned long)incoming.rgb);
  }
  model.linked = true; ++model.udpPackets;
  return true;
}
void serviceTelemetry() {
  TelemetryRequest request;
  for (unsigned i = 0; i < 4 && xQueueReceive(telemetryQueue, &request, 0) == pdTRUE; ++i) {
    TelemetryReply reply{}; reply.id = request.id;
    reply.accepted = uptimeMs() - request.receivedAt < 500 && acceptTelemetry(request.packet, reply.ack, request.receivedAt);
    xQueueSend(telemetryReplies, &reply, 0);
  }
  if (model.udp < 0) return;
  for (unsigned i = 0; i < 8; ++i) {
    char packet[201]; sockaddr_in peer{}; socklen_t size = sizeof(peer);
    const int received = recvfrom(model.udp, packet, sizeof(packet), MSG_DONTWAIT, reinterpret_cast<sockaddr *>(&peer), &size);
    if (received < 0) break;
    if (!received || received >= 200 || std::memchr(packet, 0, received)) continue;
    packet[received] = 0; char ack[64] = {};
    if (acceptTelemetry(packet, ack, uptimeMs())) sendto(model.udp, ack, std::strlen(ack), 0, reinterpret_cast<sockaddr *>(&peer), size);
  }
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
void updateControl() {
  // Scheduled memorial activation and publication must exclude stale watchdog
  // snapshots just like manual commands. Nested maintenance keeps its guard.
  RecoveryOwner recoveryOwner(true); if (!recoveryOwner) return;
  const uint32_t now = uptimeMs();
  model.agentUpdates.expire(now);
  unsigned combined = 0; int remaining = 101; bool direct = false;
  for (auto &agent : model.control.agents) {
    if (agent.present && now - agent.seenAt >= kAgentTimeout) agent.present = false;
    if (!agent.present) continue;
    direct = true; combined += agent.activity;
  }
  const bool legacy = model.legacySeen && now - model.legacyAt < 8000;
  model.control.activityPercent = direct ? uint8_t(std::min(combined, 100U)) : legacy && !model.directSeen ? model.legacyActivity : 0;
  int claudeRemaining = 101;
  for (size_t i = 0; i < kAgentCapacity; ++i) {
    if (model.codexSamples[i].clientId != model.control.agents[i].clientId) model.codexSamples[i] = {};
    if (model.claudeSamples[i].clientId != model.control.agents[i].clientId) model.claudeSamples[i] = {};
    if (model.codexSamples[i].fresh(now, quota_provider::codexFreshMs))
      remaining = std::min(remaining, int(model.codexSamples[i].remaining));
    if (model.claudeSamples[i].fresh(now, quota_provider::claudeFreshMs))
      claudeRemaining = std::min(claudeRemaining, int(model.claudeSamples[i].remaining));
  }
  const bool codexRecovered = model.codexQuota.observe(remaining, model.quotaResetReported, now);
  // Claude has independent samples and filtering. Provider changes never report a refill.
  model.claudeQuota.observe(claudeRemaining, false, now);
  model.quotaResetReported = false;
  const auto previousProvider = model.control.quotaProvider;
  model.control.quotaProvider = model.quotaRotation.update(
      model.codexQuota.snapshot, model.claudeQuota.snapshot, now);
  if (codexRecovered && previousProvider == model.control.quotaProvider &&
      previousProvider == quota_provider::Provider::Codex) ++model.control.quotaRecoveryEpoch;
  model.control.codexQuota = model.codexQuota.snapshot;
  model.control.claudeQuota = model.claudeQuota.snapshot;
  const auto &selected = model.control.quotaProvider == quota_provider::Provider::Claude
      ? model.control.claudeQuota : model.control.codexQuota;
  if (selected.known) model.control.remainingPercent = uint8_t(selected.remaining);
  model.control.quotaKnown = selected.known;
  model.control.quotaFresh = selected.fresh;
  model.control.connected = model.connected; model.control.accountLinked = model.linked;
  model.control.brightness = model.settings.brightness; model.control.on = scheduledOn();
  model.control.lightFirst = model.light.first;
  model.control.lightSecond = model.light.second;
  model.control.lightFollowMatrix = model.light.followMatrix;
  model.control.lightForcedOff = model.restarting;
  model.control.airAlertLevel = uint8_t(model.air.effective(now, model.airSettings.enabled,
      model.connected, !model.airFetchFailed));
  model.control.airAlertBlink = model.city.active() &&
      (model.airSettings.enabled || model.city.demo) &&
      air_alert::active(air_alert::Level(model.control.airAlertLevel));
  model.memorial.update(now, memorialClock(), model.memorialEnabled, model.control.on);
  model.control.memorialActive = model.memorial.active;
  model.control.memorialStartedAt = model.memorial.startedAt;
  model.control.memorialDurationMs = model.memorial.durationMs;
  if (model.blackHolePreview && uint32_t(now - model.blackHolePreviewStartedAt) >= 60000U)
    model.blackHolePreview = false;
  model.control.blackHolePreview = model.blackHolePreview;
  model.control.blackHolePreviewStartedAt = model.blackHolePreviewStartedAt;
  // Maintenance and its epoch are deliberately retained in every overwritten snapshot.
  xQueueOverwrite(controlQueue, &model.control);
}
void restoreWeeklyAgents(View &view) {
  // History belongs to the web snapshot, never to live telemetry or the renderer.
  for (const auto &entry : view.weekly.entries) {
    if (!entry.clientId || !entry.name[0]) continue;
    size_t empty = kAgentCapacity;
    bool listed = false;
    for (size_t i = 0; i < kAgentCapacity; ++i) {
      if (view.control.agents[i].clientId == entry.clientId) { listed = true; break; }
      if (!view.control.agents[i].clientId && empty == kAgentCapacity) empty = i;
    }
    if (listed || empty == kAgentCapacity) continue;
    auto &agent = view.control.agents[empty];
    agent = AgentInput{};
    agent.clientId = entry.clientId; agent.rgb = entry.rgb;
    copy(agent.name, entry.name);
    view.overrideColor[empty] = false;
    for (const auto &color : view.savedColors) if (color.used && color.clientId == entry.clientId) {
      agent.rgb = color.rgb; view.overrideColor[empty] = true; break;
    }
  }
}
void overlayRanking(weekly::Store &store) {
  for (auto &entry : store.entries) {
    if (!entry.clientId) continue;
    for (const auto &agent : model.control.agents) {
      if (agent.clientId == entry.clientId && agent.name[0]) {
        copy(entry.name, agent.name);
        entry.rgb = agent.rgb;
        break;
      }
    }
    if (const auto *color = findOverride(entry.clientId)) entry.rgb = color->rgb;
  }
}
void publishView() {
  View view;
  view.agentUpdates = model.agentUpdates;
  view.agentUpdateKeyReady = agentUpdateKey[0] != 0;
  view.weekly = model.weekly;
  view.lifetime = model.lifetime;
  view.lifetimeDirty = model.lifetimeDirty; view.lifetimeSaveFailed = model.lifetimeSaveFailed;
  view.weeklyDirty = model.weeklyDirty; view.weeklySaveFailed = model.weeklySaveFailed;
  overlayRanking(view.weekly);
  overlayRanking(view.lifetime);
  view.control = model.control; view.render = model.render;
  copy(view.ssid, model.settings.ssid); copy(view.ap, model.apName); copy(view.message, model.message); copy(view.ota, model.ota);
  view.deviceId = model.deviceId; view.rssi = model.rssi; view.power = model.settings.power;
  view.start = model.settings.start; view.end = model.settings.end; view.days = model.settings.days;
  view.scheduleEnabled = model.settings.scheduleEnabled; view.dirty = model.dirty; view.nvsReady = model.nvsReady;
  view.light = model.light; view.lightReady = external_light::ready(); view.lightAllowed = model.lightAllowed;
  view.buzzerReady = buzzer::ready();
  view.buzzerVolume = model.buzzerVolume; view.buzzerDirty = model.buzzerDirty; view.buzzerSaveFailed = model.buzzerSaveFailed;
  view.lightDirty = model.lightDirty; view.lightSaveFailed = model.lightSaveFailed;
  view.dirty = view.dirty || model.lightDirty || model.buzzerDirty || model.skyColorsDirty;
  view.restarting = model.restarting; view.maintenanceTimeouts = model.maintenanceTimeouts; view.udpPackets = model.udpPackets;
  view.memorialEnabled = model.memorialEnabled; view.memorialManual = model.memorial.manual;
  view.memorialSaveFailed = model.memorialSaveFailed; view.dirty = view.dirty || model.memorialDirty;
  fillAirAlertView(view); view.dirty = view.dirty || model.airDirty;
  view.otaDone = model.otaDone; view.otaTotal = model.otaTotal; view.netStack = uxTaskGetStackHighWaterMark(nullptr);
  view.recoveryOfflineMs = model.wifiRecovery.offlineAge(uptimeMs());
  view.recoveryStage = model.wifiRecovery.stage(); view.recoveryStacks = model.wifiRecovery.stacks();
  view.recoveryFaults = model.wifiRecovery.faults(); view.recoveryCode = model.recoveryCode;
  view.networkLoops = model.loops; view.disconnects = model.disconnects; view.disconnectReason = model.disconnectReason;
  for (size_t i = 0; i < kAgentCapacity; ++i) {
    view.overrideColor[i] = findOverride(model.control.agents[i].clientId) != nullptr;
    view.savedColors[i] = model.settings.colors[i];
  }
  restoreWeeklyAgents(view);
  if (xSemaphoreTake(viewMutex, pdMS_TO_TICKS(5)) == pdTRUE) { sharedView = view; xSemaphoreGive(viewMutex); }
}
bool snapshot(View &view) {
  if (xSemaphoreTake(viewMutex, pdMS_TO_TICKS(20)) != pdTRUE) return false;
  view = sharedView; xSemaphoreGive(viewMutex); return true;
}
void wifiEvent(void *, esp_event_base_t base, int32_t id, void *data) {
  WifiEvent event{};
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) event.kind = WifiEvent::Started;
  else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    event.kind = WifiEvent::Disconnected; event.reason = static_cast<wifi_event_sta_disconnected_t *>(data)->reason;
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    event.kind = WifiEvent::GotIp; event.ip = static_cast<ip_event_got_ip_t *>(data)->ip_info;
  } else return;
  if (xQueueSend(wifiEvents, &event, 0) != pdTRUE) wifiEventDrops.fetch_add(1, std::memory_order_relaxed);
}
bool stationConfig() {
  wifi_config_t station{};
  std::memcpy(station.sta.ssid, model.settings.ssid, std::strlen(model.settings.ssid));
  std::memcpy(station.sta.password, model.settings.password, std::strlen(model.settings.password));
  station.sta.threshold.authmode = model.settings.password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
  station.sta.pmf_cfg.capable = true; station.sta.pmf_cfg.required = false;
  return esp_wifi_set_config(WIFI_IF_STA, &station) == ESP_OK;
}
bool initializeWifi() {
  if (esp_netif_init() != ESP_OK || esp_event_loop_create_default() != ESP_OK) return false;
  model.sta = esp_netif_create_default_wifi_sta(); model.ap = esp_netif_create_default_wifi_ap();
  if (!model.sta || !model.ap) return false;
  wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT(); config.nvs_enable = false;
  if (esp_wifi_init(&config) != ESP_OK || esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK) return false;
  if (esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifiEvent, nullptr) != ESP_OK ||
      esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifiEvent, nullptr) != ESP_OK) return false;
  uint8_t mac[6]; if (esp_wifi_get_mac(WIFI_IF_STA, mac) != ESP_OK) return false;
  model.deviceId = (uint32_t(mac[3]) << 16) | (uint32_t(mac[4]) << 8) | mac[5];
  std::snprintf(model.apName, sizeof(model.apName), "LED-Forge-%06lX", (unsigned long)model.deviceId);
  esp_netif_ip_info_t address{};
  IP4_ADDR(&address.ip, 192, 168, 4, 1); IP4_ADDR(&address.gw, 192, 168, 4, 1); IP4_ADDR(&address.netmask, 255, 255, 255, 0);
  esp_netif_dhcps_stop(model.ap);
  if (esp_netif_set_ip_info(model.ap, &address) != ESP_OK) return false;
  esp_netif_dhcps_start(model.ap);
  wifi_config_t ap{};
  std::memcpy(ap.ap.ssid, model.apName, std::strlen(model.apName)); ap.ap.ssid_len = std::strlen(model.apName);
  std::memcpy(ap.ap.password, "ledforge", 8); ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
  ap.ap.max_connection = 4; ap.ap.channel = 1;
  if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK || esp_wifi_set_config(WIFI_IF_AP, &ap) != ESP_OK) return false;
  model.wifiInitialized = true;
  if (!stationConfig()) return false;
  model.wifiDriverStarted = esp_wifi_start() == ESP_OK;
  if (!model.wifiDriverStarted || esp_wifi_set_ps(WIFI_PS_NONE) != ESP_OK) return false;
  setenv("TZ", "EET-2EEST,M3.5.0/3,M10.5.0/4", 1); tzset();
  model.stationRetryAt = uptimeMs();
  return true;
}
void serviceWifi() {
  const uint32_t now = uptimeMs();
  const uint32_t dropped = wifiEventDrops.load(std::memory_order_relaxed);
  if (dropped != model.wifiEventsDropped) {
    model.wifiEventsDropped = dropped;
    ESP_LOGW(kTag, "Wi-Fi event queue drops=%lu", (unsigned long)dropped);
    diagnostic_log::Snapshot failure; failure.reason = int32_t(dropped);
    diagnostics::event(diagnostics::Level::Warning, diagnostics::Module::Wifi, uint16_t(0x0200), failure);
  }
  WifiEvent event;
  bool gotIp = false;
  for (unsigned i = 0; i < 16 && xQueueReceive(wifiEvents, &event, 0) == pdTRUE; ++i) {
    if (event.kind == WifiEvent::GotIp) gotIp = true;
    // GOT_IP is only a hint: queued events can describe an obsolete connection.
    if (event.kind == WifiEvent::Disconnected) {
      diagnostic_log::Snapshot failure; failure.reason = event.reason; failure.attempts = model.stationAttempts;
      diagnostics::event(diagnostics::Level::Warning, diagnostics::Module::Wifi,
                         diagnostics::Code::WifiDisconnected, failure);
      model.connected = false; model.stationConnecting = false; model.disconnectReason = event.reason; ++model.disconnects;
      model.rssi = -127; model.wifiNegativeSamples = 0;
      copy(model.control.ip, "192.168.4.1");
      model.stationRetryAt = now + (model.stationAttempts < 3 ? 2000 : 30000);
    }
  }
  // Poll independently of events, which can be dropped while the owner is busy.
  // Debounce negative samples; cached IP alone never proves association.
  const bool sampleDue = !model.wifiSampled || now - model.wifiSampleAt >= 1000;
  if (sampleDue || gotIp) {
    if (sampleDue) { model.wifiSampled = true; model.wifiSampleAt = now; }
    wifi_ap_record_t accessPoint{};
    esp_netif_ip_info_t ip{};
    const bool associated = esp_wifi_sta_get_ap_info(&accessPoint) == ESP_OK;
    const bool usable = associated && model.sta && esp_netif_is_netif_up(model.sta) &&
        esp_netif_get_ip_info(model.sta, &ip) == ESP_OK && ip.ip.addr != 0;
    if (usable) {
      if (!model.connected && model.wifiSampled && model.disconnects) {
        model.recoveryCode = recovery::restoredCode;
        diagnostics::event(diagnostics::Level::Warning, diagnostics::Module::Wifi, recovery::restoredCode);
      }
      model.connected = true; model.wifiReady = true; model.wifiDriverStarted = true;
      model.stationConnecting = false; model.stationAttempts = 0;
      model.wifiNegativeSamples = 0;
      if (sampleDue) model.rssi = accessPoint.rssi;
      std::snprintf(model.control.ip, sizeof(model.control.ip), IPSTR, IP2STR(&ip.ip));
      if (!model.sntpStarted) {
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL); esp_sntp_setservername(0, const_cast<char *>("pool.ntp.org"));
        esp_sntp_init(); model.sntpStarted = true;
      }
    } else {
      if (sampleDue && model.wifiNegativeSamples < 3) ++model.wifiNegativeSamples;
      if (model.connected && model.wifiNegativeSamples >= 3) {
        model.connected = false; model.rssi = -127;
        copy(model.control.ip, "192.168.4.1");
        ++model.wifiReconciledLosses; ++model.disconnects;
        diagnostic_log::Snapshot failure; failure.reason = associated ? 1 : 0;
        failure.attempts = model.stationAttempts;
        diagnostics::event(diagnostics::Level::Warning, diagnostics::Module::Wifi, uint16_t(0x0201), failure);
        model.stationRetryAt = now + (model.stationAttempts < 3 ? 2000 : 30000);
        ESP_LOGW(kTag, "Wi-Fi state reconciled: associated=%u netif/IP unavailable; losses=%lu",
                 unsigned(associated), (unsigned long)model.wifiReconciledLosses);
      }
      // Do not interrupt DHCP or static-IP netif bring-up with repeated connect calls.
      if (associated && !model.connected && !model.stationConnecting && due(now, model.stationRetryAt)) {
        model.stationConnecting = true; model.stationAt = now;
      }
    }
  }
  if (model.stationConnecting && now - model.stationAt >= 20000) {
    diagnostic_log::Snapshot failure; failure.elapsed_ms = now - model.stationAt;
    failure.attempts = model.stationAttempts;
    diagnostics::event(diagnostics::Level::Warning, diagnostics::Module::Wifi, uint16_t(0x0204), failure);
    esp_wifi_disconnect(); model.stationConnecting = false;
    model.stationRetryAt = now + 30000;
  }
  if (!model.connected && !model.stationConnecting && model.settings.ssid[0] && due(now, model.stationRetryAt)) {
    const esp_err_t connected = esp_wifi_connect();
    model.stationConnecting = connected == ESP_OK; model.stationAt = now;
    if (connected != ESP_OK) {
      if (connected == ESP_ERR_WIFI_NOT_STARTED) model.wifiDriverStarted = false;
      if (connected == ESP_ERR_WIFI_NOT_INIT || connected == ESP_ERR_WIFI_NOT_STARTED || connected == ESP_ERR_WIFI_CONN)
        model.wifiRecovery.fault();
      diagnostic_log::Snapshot failure; failure.reason = connected; failure.attempts = model.stationAttempts;
      diagnostics::event(diagnostics::Level::Warning, diagnostics::Module::Wifi, uint16_t(0x0203), failure);
    }
    if (model.stationAttempts < 3) ++model.stationAttempts;
    model.stationRetryAt = now + 30000;
  }
}
void serviceWifiRecovery() {
  const uint32_t now = uptimeMs();
  const bool protectedWork = model.control.maintenance != Maintenance::None || model.releasePending ||
      model.restarting || model.manifestReady || model.wifiChanged ||
      model.memorial.active || model.control.memorialActive || model.render.memorialTransition ||
      activeHandlers.load() != 0 || web_auth::busy() || air_alert::busy();
  const auto action = model.wifiRecovery.step(now, {model.settings.ssid[0] != 0 && model.wifiInitialized,
      model.connected, protectedWork, recoveryBudgetAvailable.load() && !model.confirmationPending});
  if (action == recovery::Action::None) return;
  RecoveryOwner recoveryOwner; if (!recoveryOwner) return;
  diagnostic_log::Snapshot event; event.reason = model.disconnectReason;
  event.attempts = model.stationAttempts;
  if (action == recovery::Action::RestartStation) {
    model.recoveryCode = recovery::stackCode;
    diagnostics::event(diagnostics::Level::Warning, diagnostics::Module::Wifi, recovery::stackCode, event);
    // AP remains available: remove only STA, then recreate its control block.
    // Do not deinit the driver/netifs or erase RAM/NVS configuration.
    esp_err_t result = esp_wifi_set_mode(WIFI_MODE_AP);
    if (result == ESP_OK) result = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (result == ESP_OK && !stationConfig()) result = ESP_FAIL;
    if (result == ESP_OK && !model.wifiDriverStarted) {
      result = esp_wifi_start(); model.wifiDriverStarted = result == ESP_OK;
    }
    if (result == ESP_OK) result = esp_wifi_set_ps(WIFI_PS_NONE);
    model.wifiReady = result == ESP_OK;
    model.stationConnecting = false; model.stationAttempts = 0;
    model.stationRetryAt = now + 2000;
    if (result != ESP_OK) {
      if (result != ESP_ERR_NO_MEM) model.wifiRecovery.fault();
      model.recoveryCode = recovery::failedCode; event.reason = result;
      diagnostics::event(diagnostics::Level::Error, diagnostics::Module::Wifi, recovery::failedCode, event);
    }
  } else if (action == recovery::Action::QueueRestart && reserveRecoveryRestart()) {
    model.recoveryRestart = model.restarting = true; model.restartAt = now;
    model.recoveryCode = recovery::queuedCode;
    diagnostics::event(diagnostics::Level::Warning, diagnostics::Module::Wifi, recovery::queuedCode, event);
    message("Wi-Fi recovery restart queued; settings preserved.");
  } else {
    model.recoveryCode = recovery::exhaustedCode;
    diagnostics::event(diagnostics::Level::Warning, diagnostics::Module::Wifi, recovery::exhaustedCode, event);
    message("Wi-Fi recovery budget exhausted; AP and connection retries remain available.");
  }
}
void initializeUdp() {
  model.udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (model.udp < 0) {
    diagnostic_log::Snapshot failure; failure.reason = errno;
    diagnostics::event(diagnostics::Level::Error, diagnostics::Module::System, uint16_t(0x0103), failure);
    message("UDP socket unavailable."); return;
  }
  int enabled = 1; setsockopt(model.udp, SOL_SOCKET, SO_BROADCAST, &enabled, sizeof(enabled));
  if (fcntl(model.udp, F_SETFL, O_NONBLOCK) < 0) {
    diagnostic_log::Snapshot failure; failure.reason = errno;
    failure.detail = 1;
    diagnostics::event(diagnostics::Level::Error, diagnostics::Module::System, uint16_t(0x0103), failure);
    close(model.udp); model.udp = -1;
    message("UDP nonblocking setup failed.");
    ESP_LOGE(kTag, "UDP nonblocking setup failed");
    return;
  }
  sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(kUdpPort); address.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(model.udp, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
    diagnostic_log::Snapshot failure; failure.reason = errno; failure.detail = 2;
    diagnostics::event(diagnostics::Level::Error, diagnostics::Module::System, uint16_t(0x0103), failure);
    close(model.udp); model.udp = -1; message("UDP port 4210 is unavailable.");
  }
}
void discover() {
  const uint32_t now = uptimeMs();
  if (!model.connected || model.udp < 0 || now - model.discoveryAt < 2000) return;
  model.discoveryAt = now;
  esp_netif_ip_info_t ip{}; if (esp_netif_get_ip_info(model.sta, &ip) != ESP_OK) return;
  sockaddr_in peer{}; peer.sin_family = AF_INET; peer.sin_port = htons(kUdpPort); peer.sin_addr.s_addr = ip.ip.addr | ~ip.netmask.addr;
  char packet[48]; std::snprintf(packet, sizeof(packet), "LF1_HELLO,%06lX,%d", (unsigned long)model.deviceId, model.rssi);
  sendto(model.udp, packet, std::strlen(packet), 0, reinterpret_cast<sockaddr *>(&peer), sizeof(peer));
}

struct HandlerScope { HandlerScope() { ++activeHandlers; } ~HandlerScope() { --activeHandlers; } };
esp_err_t error(httpd_req_t *request, const char *status, const char *text) {
  const int code = std::atoi(status);
  if (code >= 500) {
    diagnostic_log::Snapshot failure; failure.reason = code;
    diagnostics::event(diagnostics::Level::Error, diagnostics::Module::Http,
                       diagnostics::Code::RequestFailed, failure);
  }
  httpd_resp_set_status(request, status); httpd_resp_set_type(request, "text/plain; charset=utf-8");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store"); httpd_resp_set_hdr(request, "Connection", "close");
  const esp_err_t result = httpd_resp_send(request, text, HTTPD_RESP_USE_STRLEN);
  httpd_sess_trigger_close(request->handle, httpd_req_to_sockfd(request));
  return result;
}
class Html {
  httpd_req_t *request_; char buffer_[1024]; size_t size_ = 0; esp_err_t result_ = ESP_OK;
public:
  explicit Html(httpd_req_t *request) : request_(request) {}
  void flush() {
    if (result_ == ESP_OK && size_) result_ = httpd_resp_send_chunk(request_, buffer_, size_);
    size_ = 0;
  }
  void text(const char *text) {
    if (result_ != ESP_OK) return;
    while (*text) { if (size_ == sizeof(buffer_)) flush(); if (result_ != ESP_OK) return; buffer_[size_++] = *text++; }
  }
  void format(const char *format, ...) {
    char value[320]; va_list arguments; va_start(arguments, format);
    const int size = std::vsnprintf(value, sizeof(value), format, arguments); va_end(arguments);
    if (size < 0 || size >= int(sizeof(value))) { result_ = ESP_FAIL; return; }
    text(value);
  }
  void escaped(const char *value) {
    for (; *value; ++value) switch (*value) {
      case '&': text("&amp;"); break; case '<': text("&lt;"); break; case '>': text("&gt;"); break;
      case '"': text("&quot;"); break; case '\'': text("&#39;"); break;
      default: { char character[2] = {*value, 0}; text(character); }
    }
  }
  void file(const char *relativePath) {
    if (result_ != ESP_OK) return;
    FILE *input = openWebFile(relativePath);
    if (!input) { result_ = ESP_FAIL; return; }
    char chunk[256];
    size_t count;
    while (result_ == ESP_OK && (count = std::fread(chunk, 1, sizeof(chunk), input)) != 0) {
      for (size_t i = 0; i < count && result_ == ESP_OK; ++i) {
        if (size_ == sizeof(buffer_)) flush();
        if (result_ == ESP_OK) buffer_[size_++] = chunk[i];
      }
    }
    if (std::ferror(input)) result_ = ESP_FAIL;
    std::fclose(input);
  }
  template <typename Writer> void render(const char *relativePath, Writer writeToken) {
    if (result_ != ESP_OK) return;
    FILE *input = openWebFile(relativePath);
    if (!input) { result_ = ESP_FAIL; return; }
    // Values stream directly and are never reparsed as templates.
    int character;
    while (result_ == ESP_OK && (character = std::fgetc(input)) != EOF) {
      if (character == '{') {
        if (std::fgetc(input) != '{') { result_ = ESP_FAIL; break; }
        unsigned token = 0, digits = 0;
        while ((character = std::fgetc(input)) >= '0' && character <= '9') {
          if (++digits > 3) { result_ = ESP_FAIL; break; }
          token = token * 10 + unsigned(character - '0');
        }
        if (result_ != ESP_OK) break;
        if (!digits || character != '}' || std::fgetc(input) != '}') { result_ = ESP_FAIL; break; }
        writeToken(token);
      } else {
        if (size_ == sizeof(buffer_)) flush();
        if (result_ == ESP_OK) buffer_[size_++] = char(character);
      }
    }
    if (std::ferror(input)) result_ = ESP_FAIL;
    std::fclose(input);
  }
  void attribute(const char *value) {
    if (!std::strcmp(value, "@current")) file("templates/current.html");
    else if (!std::strcmp(value, "@checked")) file("templates/checked.html");
    else if (!std::strcmp(value, "@disabled")) file("templates/disabled.html");
  }
  esp_err_t end() { flush(); if (result_ == ESP_OK) result_ = httpd_resp_send_chunk(request_, nullptr, 0); return result_; }
};
void writeAgentUpgrade(Html &html, const View &view, uint32_t id, bool online) {
  const auto *entry = view.agentUpdates.find(id);
  const bool ready = online && view.agentUpdateKeyReady && agent_release::version && entry &&
      uptimeMs() - entry->polledAt < agent_update::freshMs;
  html.file("templates/agent/update-open.html");
  if (!entry) html.text("Очікування агента з підтримкою оновлень");
  else if (entry->failed && entry->failed >= agent_release::version) html.text("Оновлення не вдалося; збережено попередню версію");
  else if (view.agentUpdates.target(id, uptimeMs())) html.text("Запит у черзі; очікування підтвердження нової версії");
  else if (agent_release::version && entry->version >= agent_release::version) html.text("Установлена актуальна версія");
  else if (!online) html.text("Агент не на зв’язку");
  else if (!agent_release::version) html.text("Підписаний пакет ще не підготовлено");
  else html.format("Версія %llu · доступна %llu", (unsigned long long)entry->version, (unsigned long long)agent_release::version);
  html.render("templates/agent/update-form.html", [&](unsigned token) {
    if (!token) html.format("%08lX", (unsigned long)id);
    else if (token == 1) html.attribute(ready && entry->version < agent_release::version && entry->failed < agent_release::version &&
        !view.agentUpdates.target(id, uptimeMs()) ? "" : "@disabled");
  });
  const bool managementReady = ready && entry->management && entry->actionState != 1 && !view.agentUpdates.target(id, uptimeMs());
  html.file("templates/agent/manage-open.html");
  if (entry && entry->actionState == 1) html.text("Команда в черзі; очікування відповіді агента");
  else if (entry && entry->actionState == 2) html.text("Агент підтвердив виконання");
  else if (entry && entry->actionState == 3) html.text("Не вдалося виконати команду на ПК");
  else if (entry && entry->actionState == 4) html.text("Час очікування минув; команду скасовано");
  else if (!entry || !entry->management) html.text("Для керування потрібна нова версія агента");
  html.render("templates/agent/manage-form.html", [&](unsigned token) {
    if (!token) html.format("%08lX", (unsigned long)id);
    else if (token == 1) html.attribute(managementReady ? "" : "@disabled");
    else if (token == 2) for (const auto &agent : view.control.agents) if (agent.clientId == id) { html.escaped(agent.name); break; }
  });
}
void writeAgentControls(Html &html, const char *name, uint32_t id, uint32_t rgb,
                        bool online, unsigned activity, int remaining, bool custom, const View *view = nullptr) {
  html.render("templates/agent/heading-open.html", [&](unsigned token) {
    switch (token) {
      case 0: html.format("%08lX", (unsigned long)id); break;
    }
  });
  html.file("templates/agent/swatch-open.html");
  html.format("%06lX", (unsigned long)rgb);
  html.file("templates/agent/swatch-close.html"); html.escaped(name); html.file("templates/agent/heading-close.html");
  html.render("templates/agent/status.html", [&](unsigned token) {
    switch (token) {
      case 0: html.format("%s", online ? " online" : ""); break;
      case 1: html.format("%s", online ? "На зв’язку" : "Не на зв’язку"); break;
    }
  });
  html.render("templates/agent/metadata.html", [&](unsigned token) {
    switch (token) {
      case 0: html.format("%08lX", (unsigned long)id); break;
      case 1: html.format("%s", custom ? "Власний колір" : "Автоматичний колір"); break;
    }
  });
  html.render("templates/agent/activity.html", [&](unsigned token) {
    switch (token) {
      case 0: html.format("%u", online ? activity : 0); break;
      case 1: html.format("%u", online ? activity : 0); break;
      case 2: html.format("%u", online ? activity : 0); break;
    }
  });
  html.file("templates/agent/remaining-open.html");
  if (online && remaining >= 0) html.format("%d%%", remaining);
  else html.file("templates/agent/missing-value.html");
  html.file("templates/agent/metrics-close.html");
  html.file("templates/agent/color-form-open.html");
  html.render("templates/agent/client-input.html", [&](unsigned token) {
    switch (token) {
      case 0: html.format("%08lX", (unsigned long)id); break;
    }
  });
  html.render("templates/agent/color-input-open.html", [&](unsigned token) {
    switch (token) {
      case 0: html.format("%08lX", (unsigned long)id); break;
      case 1: html.format("%08lX", (unsigned long)id); break;
    }
  });
  html.escaped(name);
  html.render("templates/agent/color-input-value.html", [&](unsigned token) {
    switch (token) {
      case 0: html.format("%06lX", (unsigned long)rgb); break;
      case 1: html.format("%06lX", (unsigned long)rgb); break;
    }
  });
  html.file("templates/agent/controls-close.html");
  if (view) writeAgentUpgrade(html, *view, id, online);
  html.file("templates/summary/section-close.html");
}

void writeAgents(Html &html, const View &view) {
  unsigned connected = 0;
  for (const auto &agent : view.control.agents) connected += agent.present;
  html.render("templates/agents/heading.html", [&](unsigned token) {
    switch (token) {
      case 0: html.format("%u", connected); break;
      case 1: html.format("%u", unsigned(kAgentCapacity)); break;
    }
  });
  html.file("templates/agents/filters.html");
  html.file("templates/agents/list-open.html");
  unsigned known = 0;
  for (const bool online : {true, false}) for (size_t i = 0; i < kAgentCapacity; ++i) {
    const auto &agent = view.control.agents[i];
    if (agent.present != online || (!agent.present && !agent.name[0])) continue;
    ++known;
    writeAgentControls(html, agent.name, agent.clientId, agent.rgb,
                       agent.present, agent.activity, agent.quotaForPairedAccount ? agent.remaining : -1,
                       view.overrideColor[i], &view);
  }
  for (const auto &entry : view.savedColors) {
    if (!entry.used) continue;
    bool listed = false;
    for (const auto &agent : view.control.agents)
      if ((agent.present || agent.name[0]) && agent.clientId == entry.clientId) listed = true;
    if (listed) continue;
    char name[24]; std::snprintf(name, sizeof(name), "Агент %08lX", (unsigned long)entry.clientId);
    ++known;
    writeAgentControls(html, name, entry.clientId, entry.rgb, false, 0, -1, true, &view);
  }
  if (!known) html.file("templates/agents/empty.html");
  html.file("templates/agents/list-close.html");
  html.file("templates/agents/filter-footer.html");
  html.file("templates/agents/template-open.html");
  writeAgentControls(html, "", 0, 0x56d39b, false, 0, -1, false, &view);
  html.file("templates/agents/template-close.html");
}

unsigned portalAgentCount(const View &view) {
  unsigned count = 0;
  for (const auto &agent : view.control.agents) count += agent.present;
  return count;
}
const char *portalMatrixState(const View &view) {
  if (!view.render.assetsReady || view.render.ledError) return "Помилка відмальовування";
  if (!view.control.on) return view.render.sceneVisible && view.render.sceneProgress ? "Вимикається" : "Вимкнено";
  if (view.control.memorialActive) return "Хвилина мовчання";
  if (view.render.memorialTransition) return "Повернення до неба";
  if (!view.render.sceneVisible) return "Показ IP-адреси";
  return view.render.sceneProgress < PROJECT_SCENE_DURATION_MS ? "Запускається" : "Увімкнено";
}
const char *portalOtaLabel(const char *value) {
  if (!std::strcmp(value, "Up to date")) return "Встановлено актуальну версію";
  if (!std::strcmp(value, "Not checked")) return "Перевірку ще не виконано";
  if (!std::strcmp(value, "Checking S3 firmware manifest")) return "Перевіряємо GitHub";
  if (!std::strcmp(value, "New firmware verified; waiting for LEDs")) return "Підготовка до встановлення";
  if (!std::strcmp(value, "Firmware verified; restarting")) return "Оновлення перевірено, перезапуск";
  if (!std::strcmp(value, "Update check queued")) return "Перевірку заплановано";
  if (!std::strcmp(value, "Manifest HTTPS request failed")) return "GitHub тимчасово недоступний";
  if (!std::strcmp(value, "Firmware HTTPS request failed")) return "Не вдалося завантажити прошивку";
  if (!std::strcmp(value, "Firmware transfer or raw SHA-256 verification failed")) return "Помилка передачі або контрольної суми";
  return value;
}
void writePortalStats(Html &html, const View &view) {
  html.render("templates/stats/remaining-open.html", [&](unsigned token) {
    if (!token) html.text(view.control.quotaProvider == quota_provider::Provider::Claude ? "Claude Code · 5 год" : "Codex");
  });
  if (!view.control.quotaKnown) html.text("Немає даних");
  else if (!view.control.quotaFresh) html.format("%u%% · останні дані", view.control.remainingPercent);
  else html.format("%u%%", view.control.remainingPercent);
  html.render("templates/stats/providers.html", [&](unsigned token) {
    const auto &quota = token == 0 ? view.control.codexQuota : view.control.claudeQuota;
    if (!quota.known) html.text("Немає даних");
    else html.format(quota.fresh ? "%u%%" : "%u%% · останні дані", unsigned(quota.remaining));
  });
  html.render("templates/stats/activity.html", [&](unsigned token) {
    switch (token) {
      case 0: html.format("%u", view.control.activityPercent); break;
    }
  });
  html.render("templates/stats/agents.html", [&](unsigned token) {
    switch (token) {
      case 0: html.format("%u", portalAgentCount(view)); break;
      case 1: html.format("%u", unsigned(kAgentCapacity)); break;
    }
  });
  html.file("templates/stats/wifi-open.html"); html.text(view.control.connected ? "Підключено" : "Точка доступу");
  html.file("templates/stats/stats-close.html");
}
void writeSchedule(Html &html, const View &view) {
  html.file("templates/schedule/section-open.html");
  const time_t timestamp = std::time(nullptr);
  if (timestamp < 1700000000) html.text("очікуємо NTP");
  else {
    tm local{}; localtime_r(&timestamp, &local);
    char clock[32]; std::strftime(clock, sizeof(clock), "%d.%m.%Y %H:%M", &local); html.text(clock);
  }
  html.file("templates/schedule/form-open.html");
  html.render("templates/schedule/enabled.html", [&](unsigned token) {
    switch (token) {
      case 0: html.attribute(view.scheduleEnabled ? "@checked" : ""); break;
    }
  });
  html.file("templates/schedule/start-input-open.html");
  html.format("%02u:%02u", view.start / 60, view.start % 60);
  html.file("templates/schedule/end-input-open.html");
  html.format("%02u:%02u", view.end / 60, view.end % 60);
  html.file("templates/schedule/days-open.html");
  static constexpr const char *days[] = {"Нд", "Пн", "Вт", "Ср", "Чт", "Пт", "Сб"};
  for (unsigned day = 1; day <= 7; ++day) {
    const unsigned index = day % 7;
    html.render("templates/schedule/day-checkbox.html", [&](unsigned token) {
    switch (token) {
      case 0: html.format("%u", index); break;
      case 1: html.attribute(view.days & (1U << index) ? "@checked" : ""); break;
      case 2: html.format("%s", days[index]); break;
    }
  });
  }
  html.file("templates/schedule/presets.html");
  if (view.power != 0) html.file("templates/schedule/manual-notice.html");
  html.file("templates/schedule/form-close.html");
}
void writeAgentSummaryRow(Html &html, const AgentInput &agent) {
  html.render("templates/summary/agent-open.html", [&](unsigned token) {
    switch (token) {
      case 0: html.format("%08lX", (unsigned long)agent.clientId); break;
      case 1: html.format("%06lX", (unsigned long)agent.rgb); break;
    }
  });
  html.escaped(agent.name);
  html.file("templates/summary/status-open.html");
  if (agent.present) html.text(" online");
  html.file("templates/summary/status-value-open.html"); html.text(agent.present ? "На зв’язку" : "Не на зв’язку");
  html.render("templates/summary/activity-close.html", [&](unsigned token) {
    switch (token) {
      case 0: html.format("%u", agent.present ? agent.activity : 0); break;
      case 1: html.format("%u", agent.present ? agent.activity : 0); break;
      case 2: html.format("%u", agent.present ? agent.activity : 0); break;
    }
  });
}
void writeAgentSummary(Html &html, const View &view) {
  html.file("templates/summary/section-open.html");
  html.file("templates/summary/list-open.html");
  unsigned known = 0;
  for (const bool online : {true, false}) for (const auto &agent : view.control.agents) {
    if (agent.present != online || (!agent.present && !agent.name[0])) continue;
    ++known;
    writeAgentSummaryRow(html, agent);
  }
  if (!known) html.file("templates/summary/empty.html");
  html.file("templates/agents/list-close.html");
  html.file("templates/summary/template-open.html");
  writeAgentSummaryRow(html, AgentInput{});
  html.file("templates/agents/template-close.html");
  html.file("templates/summary/section-close.html");
}
void writeOverview(Html &html, const View &view) {
  writePortalStats(html, view);
  html.file("templates/overview/matrix-preview.html");
  html.file("templates/overview/workspace-open.html");
  html.text(portalMatrixState(view)); html.file("templates/overview/power-heading.html");
  html.file("templates/overview/power-form-open.html");
  for (unsigned mode = 0; mode < 3; ++mode)
    html.render("templates/overview/power-button.html", [&](unsigned token) {
    switch (token) {
      case 0: html.format("%s", mode == 0 ? "auto" : mode == 1 ? "on" : "off"); break;
      case 1: html.format("%s", view.power == mode ? "true" : "false"); break;
      case 2: html.attribute(view.restarting || view.control.maintenance == Maintenance::Ota ? "@disabled" : ""); break;
      case 3: html.format("%s", mode == 0 ? "Авто" : mode == 1 ? "Увімкнути" : "Вимкнути"); break;
    }
  });
  html.file("templates/overview/brightness-form-open.html");
  html.render("templates/overview/brightness-output.html", [&](unsigned token) {
    switch (token) {
      case 0: html.format("%u", (unsigned(view.control.brightness) * 100 + 127) / 255); break;
    }
  });
  html.render("templates/overview/brightness-input.html", [&](unsigned token) {
    switch (token) {
      case 0: html.format("%u", view.control.brightness); break;
    }
  });
  html.file("templates/overview/brightness-actions.html");
  html.file("templates/overview/column-next.html"); writeSchedule(html, view); html.file("templates/overview/workspace-close.html");
  html.render("templates/overview/external-light.html", [&](unsigned token) {
    switch (token) {
      case 0: html.format("%u", view.light.first); break;
      case 1: html.format("%u", view.light.second); break;
      case 2: html.attribute(view.light.followMatrix ? "@checked" : ""); break;
      case 3: html.text(!view.lightReady ? "Помилка виходів. Підсвітку вимкнено." :
          !view.lightAllowed && (view.light.first || view.light.second) ? "Вимкнено разом із матрицею або на час обслуговування." :
          view.light.first || view.light.second ? "Увімкнено" : "Вимкнено"); break;
      case 4: html.text(view.lightSaveFailed ? "Не вдалося зберегти. Повторюємо спробу." : view.lightDirty ? "Збереження…" : ""); break;
      case 5: html.attribute(!view.lightReady || view.restarting || view.control.maintenance == Maintenance::Ota ? "@disabled" : ""); break;
    }
  });
  writeAgentSummary(html, view);
}
void writeSettings(Html &html, const View &view) {
  html.file("templates/settings/workspace-open.html");
  html.text(view.control.connected ? "Підключено" : "Точка доступу"); html.file("templates/settings/wifi-heading-close.html");
  html.render("templates/settings/ip.html", [&](unsigned token) {
    switch (token) {
      case 0: html.format("%s", view.control.ip); break;
    }
  });
  html.file("templates/settings/wifi-form-open.html");
  html.escaped(view.ssid);
  html.file("templates/settings/password-input.html");
  html.file("templates/settings/wifi-actions.html");
  html.file("templates/settings/ota-open.html");
  html.escaped(portalOtaLabel(view.ota));
  html.file("templates/settings/ota-progress-open.html");
  if (!view.otaTotal) html.file("templates/hidden.html");
  html.render("templates/settings/ota-progress-value.html", [&](unsigned token) {
    switch (token) {
      case 0: html.format("%u", view.otaTotal ? unsigned(uint64_t(view.otaDone) * 100 / view.otaTotal) : 0); break;
    }
  });
  html.render("templates/settings/firmware-version.html", [&](unsigned token) {
    switch (token) {
      case 0: html.format("%llu", (unsigned long long)PROJECT_FIRMWARE_VERSION); break;
    }
  });
  html.file("templates/settings/ota-actions.html");
  html.render("templates/settings/memorial.html", [&](unsigned token) {
    if (token == 0) html.attribute(view.memorialEnabled ? "@checked" : "");
    else if (token == 1) html.text(view.control.memorialActive ? "Відтворюється" : "Не відтворюється");
    else if (token == 2) html.attribute(!view.control.on || view.control.memorialActive || view.restarting ||
        view.control.maintenance != Maintenance::None ? "@disabled" : "");
    else if (token == 3) html.attribute(!view.control.memorialActive || view.restarting ||
        view.control.maintenance != Maintenance::None ? "@disabled" : "");
    else if (token == 4) html.text(view.memorialSaveFailed ? "Не вдалося зберегти. Спробуйте ще раз." :
        view.dirty ? "Збереження…" : "");
    else if (token == 5) html.text(!view.control.on ? "Матриця вимкнена. Для відтворення ввімкніть її." :
        std::time(nullptr) < 1700000000 ? "Очікування часу NTP. Ручне відтворення доступне." : "");
  });
  html.render("templates/settings/black-hole.html", [&](unsigned token) {
    const bool active = view.control.blackHolePreview &&
        uint32_t(uptimeMs() - view.control.blackHolePreviewStartedAt) < 60000U;
    if (token == 0) html.text(active ? "Відтворюється" : "Не відтворюється");
    else if (token == 1) html.attribute(!view.control.on || view.control.memorialActive ||
        view.render.memorialTransition || view.restarting ||
        view.control.maintenance != Maintenance::None ? "@disabled" : "");
    else if (token == 2) html.attribute(!active || view.restarting ||
        view.control.maintenance != Maintenance::None ? "@disabled" : "");
  });
  html.render("templates/settings/air-alert.html", [&](unsigned token) {
    if (token == 0) html.attribute(view.airEnabled ? "@checked" : "");
    else if (token == 1) html.text(view.airStatus);
    else if (token == 3) { html.text(view.airSource); html.text(". "); html.text(view.airDetails); }
    else if (token == 4) html.attribute(!view.control.on || view.restarting ||
        view.control.maintenance != Maintenance::None ? "@disabled" : "");
    else if (token == 5) html.attribute(!view.airManual || view.restarting ||
        view.control.maintenance != Maintenance::None ? "@disabled" : "");
    else if (token == 8) html.text(view.cityDemo ? "Ручний тест блимання · 60 с" : view.cityReported ? "Є свіже повідомлення про об’єкт над містом" :
        view.cityFailed ? "Джерело Повітряних сил тимчасово недоступне" :
        view.cityFresh ? "Свіжих повідомлень про об’єкти над містом не знайдено" :
        "Очікування даних Повітряних сил");
    else if (token == 9) html.text(view.cityFailed ? "Зв’язок із каналом Повітряних сил втрачено. Підтверджений допис діє до завершення 5 хв від публікації." : "");
    else if (token == 6) {
      if (view.airAge == UINT32_MAX) html.text("Оновлення: ще немає даних");
      else html.format("Оновлено %lu с тому%s", (unsigned long)view.airAge, view.airFresh ? "" : " · дані застаріли");
    } else if (token == 7) {
      if (view.airManual) html.format("Ручний тест · %lu с", (unsigned long)view.airRemaining);
      else html.text("Ручний тест не активний");
    }
  });
  html.render("templates/settings/buzzer.html", [&](unsigned token) {
    if (token == 0) html.attribute(!view.buzzerReady || view.restarting ||
        view.control.maintenance != Maintenance::None ? "@disabled" : "");
    else if (token == 1) html.format("%u", unsigned(view.buzzerVolume));
    else if (token == 2) html.text(view.buzzerSaveFailed ? "Не вдалося зберегти. Повторюємо спробу." : view.buzzerDirty ? "Збереження…" : "");
  });
  html.render("templates/settings/sky-colors.html", [&](unsigned token) {
    char color[8];
    std::snprintf(color, sizeof(color), "#%06lx", static_cast<unsigned long>(
        token == 0 ? view.control.codexSkyColor : view.control.claudeSkyColor));
    html.text(color);
  });
  html.file("templates/settings/system-open.html");
  html.render("templates/settings/device-id.html", [&](unsigned token) {
    switch (token) {
      case 0: html.format("%06lX", (unsigned long)view.deviceId); break;
    }
  });
  html.file("templates/settings/uptime-open.html");
  const uint32_t seconds = uptimeMs() / 1000;
  html.format("%lu д %02lu:%02lu:%02lu", (unsigned long)(seconds / 86400), (unsigned long)(seconds / 3600 % 24), (unsigned long)(seconds / 60 % 60), (unsigned long)(seconds % 60));
  html.file("templates/settings/heap-open.html");
  html.format("%u КБ", unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024));
  html.render("templates/settings/rssi.html", [&](unsigned token) {
    switch (token) {
      case 0: html.format("%d", view.rssi); break;
    }
  });
  html.render("templates/settings/frame-rate.html", [&](unsigned token) {
    switch (token) {
      case 0: html.format("%u", unsigned(PROJECT_FPS)); break;
    }
  });
  html.render("templates/settings/matrix-pin.html", [&](unsigned token) {
    switch (token) {
      case 0: html.format("%u", unsigned(PROJECT_DATA_PIN)); break;
    }
  });
  html.file("templates/settings/diagnostics.html");
  html.file("templates/settings/maintenance-open.html");
  if (view.restarting) html.file("templates/disabled.html");
  html.file("templates/settings/maintenance-close.html");
}
void writeTokenCount(Html &html, uint64_t value) {
  char digits[21]; std::snprintf(digits, sizeof(digits), "%llu", (unsigned long long)value);
  const size_t length = std::strlen(digits);
  for (size_t i = 0; i < length; ++i) {
    if (i && (length - i) % 3 == 0) html.text(" ");
    const char digit[2] = {digits[i], 0}; html.text(digit);
  }
}
void writeRanking(Html &html, const View &view, bool lifetime) {
  const auto &store = lifetime ? view.lifetime : view.weekly;
  html.file(lifetime ? "templates/lifetime/open.html" : "templates/weekly/open.html");
  if (lifetime) html.text("За доступними журналами агентів");
  else if (store.week) html.format("%02lu.%02lu.%04lu · понеділок–неділя, Київ", (unsigned long)(store.week % 100),
      (unsigned long)(store.week / 100 % 100), (unsigned long)(store.week / 10000));
  else html.text("Очікування синхронізації часу");
  html.file(lifetime ? "templates/lifetime/table.html" : "templates/weekly/table.html");
  const weekly::Entry *sorted[weekly::capacity] = {};
  size_t count = 0; double total = 0;
  for (const auto &entry : store.entries) if (entry.clientId) { sorted[count++] = &entry; total += double(entry.tokens); }
  std::sort(sorted, sorted + count, [](const weekly::Entry *a, const weekly::Entry *b) {
    return a->tokens != b->tokens ? a->tokens > b->tokens : a->clientId < b->clientId;
  });
  for (size_t i = 0; i < count; ++i) {
    const auto &entry = *sorted[i];
    html.format("<tr><td>%u</td><td><span class='weekly-agent-color' style='background:#%06lX' aria-hidden='true'></span>",
        unsigned(i + 1), (unsigned long)entry.rgb);
    html.escaped(entry.name);
    html.text("</td><td>"); writeTokenCount(html, entry.tokens);
    const unsigned tenths = total ? unsigned(1000.0 * double(entry.tokens) / total + 0.5) : 0;
    html.format("</td><td>%u", tenths / 10);
    if (tenths % 10) html.format(",%u", tenths % 10);
    html.text("%</td></tr>");
  }
  html.file(lifetime ? "templates/lifetime/close.html" : "templates/weekly/close.html");
  html.format("<p class='muted' data-%s-empty%s>%s</p></section>", lifetime ? "lifetime" : "weekly",
      count ? " hidden" : "", lifetime ? "Ще немає даних із журналів агентів." : "Ще немає даних за цей тиждень.");
}
void writeWeekly(Html &html, const View &view) { writeRanking(html, view, false); }
void writeLifetime(Html &html, const View &) { html.file("templates/history/calendar.html"); }
esp_err_t page(httpd_req_t *request) {
  HandlerScope scope; View view;
  if (!snapshot(view)) return error(request, "503 Service Unavailable", "Controller busy; retry.");
  httpd_resp_set_type(request, "text/html; charset=utf-8"); httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  const bool agentsPage = !std::strcmp(request->uri, "/agents");
  const bool settingsPage = !std::strcmp(request->uri, "/settings");
  Html html(request);
  html.file("templates/shell/document-head.html");
  // Bootstrap and its watchdog fit in the first HTML chunk: no blocking script fetch.
  html.file("assets/js/boot.js");
  html.file("templates/shell/document-assets.html");
  html.file("templates/shell/shell-open.html");
  html.render("templates/shell/device-meta.html", [&](unsigned token) {
    switch (token) {
      case 0: html.format("%06lX", (unsigned long)view.deviceId); break;
      case 1: html.format("%s", view.control.ip); break;
    }
  });
  html.file("templates/shell/header-status.html");
  html.file("templates/shell/nav-open.html");
  html.render("templates/shell/nav-links.html", [&](unsigned token) {
    switch (token) {
      case 0: html.attribute(!agentsPage && !settingsPage ? "@current" : ""); break;
      case 1: html.attribute(agentsPage ? "@current" : ""); break;
      case 2: html.attribute(settingsPage ? "@current" : ""); break;
    }
  });
  html.file("templates/shell/main-open.html");
  html.text(agentsPage ? "Агенти" : settingsPage ? "Налаштування" : "Огляд");
  html.file("templates/shell/heading-close.html");
  if (view.message[0]) { html.file("templates/shell/notice-open.html"); html.escaped(view.message); html.file("templates/shell/paragraph-close.html"); }
  if (view.dirty) html.file("templates/shell/saving-notice.html");
  if (view.restarting) html.file("templates/shell/restart-notice.html");
  if (!view.render.assetsReady || view.render.ledError) html.file("templates/shell/matrix-error-notice.html");
  if (agentsPage) { writeWeekly(html, view); writeLifetime(html, view); html.file("templates/shell/section-open.html"); writeAgents(html, view); html.file("templates/summary/section-close.html"); }
  else if (settingsPage) writeSettings(html, view);
  else writeOverview(html, view);
  html.file("templates/shell/footer.html");
  if (!agentsPage && !settingsPage) html.file("templates/shell/preview-script.html");
  html.file("templates/shell/document-close.html");
  return html.end();
}
esp_err_t deviceImage(httpd_req_t *request) {
  HandlerScope scope;
  return serveWebFile(request, "images/device.webp", "image/webp");
}
esp_err_t favicon(httpd_req_t *request) {
  HandlerScope scope;
  return serveWebFile(request, "images/favicon.svg", "image/svg+xml");
}
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
    return error(request, "503 Service Unavailable", "Preview stream is already in use.");
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
  // Routes are explicitly registered; no user-supplied filesystem paths.
  const bool css = !std::strcmp(request->uri, "/assets/css/portal.css") || !std::strcmp(request->uri, "/assets/css/preview.css");
  const bool js = !std::strcmp(request->uri, "/assets/js/portal.js") || !std::strcmp(request->uri, "/assets/js/preview.js") || !std::strcmp(request->uri, "/assets/js/boot.js") || !std::strcmp(request->uri, "/assets/js/token-history.js");
  if (!css && !js) return error(request, "404 Not Found", "Not found");
  return serveWebFile(request, request->uri + 1, css ? "text/css; charset=utf-8" : "text/javascript; charset=utf-8");
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
  number(root, "uptime_ms", uptimeMs()); number(root, "frames", view.render.frames);
  number(root, "heap", heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  number(root, "rssi", view.rssi); number(root, "activity", view.control.activityPercent);
  number(root, "agents_count", portalAgentCount(view)); number(root, "brightness", view.control.brightness);
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
    };
    addQuota("codex", view.control.codexQuota);
    addQuota("claude", view.control.claudeQuota);
  }
  boolean(root, "quota_known", view.control.quotaKnown);
  boolean(root, "quota_fresh", view.control.quotaFresh);
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
  for (unsigned kind = 0; kind < 2; ++kind) {
  const auto &store = kind ? view.lifetime : view.weekly;
  cJSON *ranking = cJSON_AddObjectToObject(root, kind ? "lifetime" : "weekly");
  if (!ranking) okay = false;
  else {
    if (!kind) number(ranking, "week", store.week);
    boolean(ranking, "dirty", kind ? view.lifetimeDirty : view.weeklyDirty);
    boolean(ranking, "save_failed", kind ? view.lifetimeSaveFailed : view.weeklySaveFailed);
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
  char body[2048];
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
  return httpd_resp_send(request, body, int(recoveryOffset) + recoveryCount);
}
void diagnosticMonitorTask(void *) {
  TickType_t wake = xTaskGetTickCount();
  int32_t previousLedError = 0;
  uint32_t checkpointAt = 0;
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
  if (!parseTelemetry(command.packet, agent, legacy, remaining, activity) &&
      !parseWeeklyTelemetry(command.packet, weeklyEntry, week) &&
      !quota_provider::parse(command.packet, PROJECT_ACCOUNT_FINGERPRINT, claudePacket)) return error(request, "400 Bad Request", "Invalid telemetry.");
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
  } else if (!std::strcmp(uri, "/sky-colors")) {
    command.kind = CommandKind::SkyColors;
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
      command.kind == CommandKind::SkyColors || command.kind == CommandKind::Wifi || command.kind == CommandKind::Update || command.kind == CommandKind::Restart || command.kind == CommandKind::BuzzerTest || command.kind == CommandKind::BuzzerVolume ||
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
  config.max_uri_handlers = 48; config.max_open_sockets = 4; config.lru_purge_enable = true;
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
  for (const char *uri : {"/save", "/power", "/brightness", "/external-light", "/buzzer-test", "/buzzer-volume", "/schedule", "/sky-colors", "/agent-color", "/restart", "/update", "/memorial", "/black-hole", "/air-alert"})
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

// All configuration mutations, flash operations, and telemetry-table writes belong to this task.
void serviceCommands() {
  RecoveryOwner recoveryOwner; if (!recoveryOwner) return;
  NetworkCommand command;
  for (unsigned count = 0; count < 8 && xQueueReceive(commandQueue, &command, 0) == pdTRUE; ++count) {
    switch (command.kind) {
      case CommandKind::AirAlertSave: {
        AirAlertSettings next = model.airSettings;
        next.enabled = command.enabled;
        if (next.enabled != model.airSettings.enabled || model.airSaveFailed) {
          model.airSettings = next; model.airDirty = true;
          ++model.airGeneration; model.airNextAt = uptimeMs();
          model.air.stopDemo(); model.city.stopDemo(); model.cityNextAt = uptimeMs();
        }
        break;
      }
      case CommandKind::AirAlertTest:
        if (model.control.on) {
          model.air.startDemo(command.value == 5 ? air_alert::Level::Yellow : air_alert::Level(command.value), uptimeMs());
          if (command.value == 5) model.city.startDemo(uptimeMs());
          else model.city.stopDemo();
        }
        else message("Матриця вимкнена. Перевірку тривоги не запущено.");
        break;
      case CommandKind::AirAlertStop: model.air.stopDemo(); model.city.stopDemo(); break;
      case CommandKind::BlackHolePlay:
        if (model.control.on && scheduledOn() && !model.restarting &&
            model.control.maintenance == Maintenance::None && !model.memorial.active &&
            !model.control.memorialActive && !model.render.memorialTransition) {
          model.blackHolePreview = true;
          model.blackHolePreviewStartedAt = uptimeMs();
        }
        break;
      case CommandKind::BlackHoleStop:
        model.blackHolePreview = false;
        break;
      case CommandKind::MemorialSave:
        if (model.memorialEnabled != command.enabled || model.memorialSaveFailed) {
          model.memorialEnabled = command.enabled; model.memorialDirty = true;
        }
        break;
      case CommandKind::MemorialPlay:
        if (!scheduledOn()) message("Матриця вимкнена. Спочатку ввімкніть її.");
        else if (!model.memorial.start(uptimeMs())) message("Хвилина мовчання вже відтворюється.");
        break;
      case CommandKind::MemorialStop:
        model.memorial.update(uptimeMs(), memorialClock(), model.memorialEnabled, scheduledOn());
        model.memorial.stop(); break;
      case CommandKind::AgentPoll:
        if (knownAgent(model.control, command.clientId) || (command.agentActionAck && uninstallAcknowledgmentWindow(model.agentUpdates, command.clientId, uptimeMs())))
          model.agentUpdates.poll(command.clientId, command.agentVersion, uptimeMs(), command.agentNonceCounter,
            command.agentFailedVersion, command.agentManagement, command.agentActionAck, command.agentActionResult);
        break;
      case CommandKind::AgentRename:
      case CommandKind::AgentUninstall:
        if (!model.actionSequence) {
          for (unsigned i = 0; i < 12; ++i) model.actionSequence = model.actionSequence * 16 + unsigned(hexDigit(agentUpdateBoot[i]));
        }
        if (knownAgent(model.control, command.clientId) && model.agentUpdates.requestAction(command.clientId,
            command.kind == CommandKind::AgentRename ? agent_update::Action::Rename : agent_update::Action::Uninstall,
            ++model.actionSequence, command.agentName, uptimeMs())) message("Команду надіслано; очікування агента.");
        else message("Канал керування агентом недоступний.");
        break;
      case CommandKind::AgentUpgrade:
        if (knownAgent(model.control, command.clientId) && model.agentUpdates.request(command.clientId, agent_release::version, uptimeMs()))
          message("Agent update queued; waiting for confirmation.");
        else message("Agent update channel is not ready; request not queued.");
        break;
      case CommandKind::Wifi: {
        const bool sameSsid = !std::strcmp(command.ssid, model.settings.ssid);
        if (command.password[0] || !sameSsid) copy(model.settings.password, command.password);
        copy(model.settings.ssid, command.ssid); model.wifiChanged = true; dirty(); break;
      }
      case CommandKind::Power: model.settings.power = command.value; dirty(); break;
      case CommandKind::Brightness: model.settings.brightness = command.value; dirty(); break;
      case CommandKind::ExternalLight:
        if (command.start <= 100 && command.end <= 100 && external_light::ready() &&
            (model.light.first != command.start || model.light.second != command.end || model.light.followMatrix != command.enabled)) {
          model.light.first = uint8_t(command.start); model.light.second = uint8_t(command.end);
          model.light.followMatrix = command.enabled; model.lightDirty = true;
        }
        break;
      case CommandKind::BuzzerTest:
        if (!model.restarting && model.control.maintenance == Maintenance::None)
          (void)buzzer::start(uptimeMs(), model.buzzerVolume);
        else if (!model.restarting && model.control.maintenance == Maintenance::Persist)
          model.buzzerPending = true;
        break;
      case CommandKind::BuzzerVolume:
        if (command.value <= 100 && command.value != model.buzzerVolume) {
          model.buzzerVolume = command.value; model.buzzerDirty = true; model.buzzerDirtyAt = uptimeMs();
        }
        break;
      case CommandKind::Schedule:
        model.settings.scheduleEnabled = command.enabled; model.settings.start = command.start;
        model.settings.end = command.end; model.settings.days = command.days; dirty(); break;
      case CommandKind::SkyColors:
        if (command.codexSkyColor <= 0xffffff && command.claudeSkyColor <= 0xffffff &&
            (model.control.codexSkyColor != command.codexSkyColor || model.control.claudeSkyColor != command.claudeSkyColor)) {
          model.control.codexSkyColor = command.codexSkyColor;
          model.control.claudeSkyColor = command.claudeSkyColor;
          model.skyColorsDirty = true;
        }
        break;
      case CommandKind::Color: {
        size_t agentSlot = kAgentCapacity;
        for (size_t i = 0; i < kAgentCapacity; ++i)
          if ((model.control.agents[i].present || model.control.agents[i].name[0]) && model.control.agents[i].clientId == command.clientId) { agentSlot = i; break; }
        ColorOverride *slot = nullptr;
        for (auto &entry : model.settings.colors) if (entry.used && entry.clientId == command.clientId) { slot = &entry; break; }
        bool historical = false;
        for (const auto &entry : model.weekly.entries)
          if (entry.clientId == command.clientId && entry.name[0]) { historical = true; break; }
        if (agentSlot == kAgentCapacity && !slot && !historical) { message("Unknown agent; waiting for its first heartbeat."); break; }
        if (command.automatic) {
          if (slot) { *slot = {}; dirty(); }
          if (agentSlot != kAgentCapacity) model.control.agents[agentSlot].rgb = distinctColor(model.requestedRgb[agentSlot], command.clientId);
        } else {
          if (!slot) for (auto &entry : model.settings.colors) if (!entry.used) { slot = &entry; break; }
          if (!slot) { message("Color override storage is full; reset an unused override first."); break; }
          *slot = {command.clientId, command.rgb, 1, {0, 0, 0}};
          if (agentSlot != kAgentCapacity) model.control.agents[agentSlot].rgb = command.rgb;
          dirty();
        }
        break;
      }
      case CommandKind::Restart:
        if (!model.restarting) { model.restarting = true; model.restartAt = uptimeMs(); message("Restart queued."); }
        break;
      case CommandKind::Update:
        if (!model.manifestBusy) { model.updateRequested = true; model.otaAt = uptimeMs(); otaMessage("Update check queued"); }
        else message("Update check already running.");
        break;
    }
  }
}

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
  if (due(now, model.otaAt)) queueManifestCheck();
}
} // namespace

void networkTask(void *) {
  markNetworkProgress(network_progress::Stage::Starting);
  viewMutex = xSemaphoreCreateMutexStatic(&viewMutexStorage);
  wifiEvents = xQueueCreateStatic(16, sizeof(WifiEvent), wifiEventBytes, &wifiEventStorage);
  telemetryQueue = xQueueCreateStatic(4, sizeof(TelemetryRequest), telemetryBytes, &telemetryStorage);
  telemetryReplies = xQueueCreateStatic(4, sizeof(TelemetryReply), telemetryReplyBytes, &telemetryReplyStorage);
  manifestJobs = xQueueCreateStatic(1, sizeof(ManifestJob), manifestJobBytes, &manifestJobStorage);
  manifestResults = xQueueCreateStatic(1, sizeof(ManifestResult), manifestResultBytes, &manifestResultStorage);
  if (!viewMutex || !wifiEvents || !telemetryQueue || !telemetryReplies || !manifestJobs || !manifestResults) { ESP_LOGE(kTag, "Cannot initialize network IPC"); vTaskDelete(nullptr); return; }
  // Complete all boot-time storage and PHY initialization before permitting RMT output.
  loadSettings();
  daily_runtime::initialize(model.nvsReady);
  loadExternalLight();
  loadBuzzerVolume();
  loadSkyColors();
  bootloader_random_enable();
  web_auth::initialize();
  bootloader_random_disable();
  loadAgentUpdateKey();
  loadWeekly();
  loadMemorialSettings();
  loadAirAlertSettings();
  loadTelegramAlertState();
  const bool webReady = initializeWebFiles();
  if (!webReady) { message("Web filesystem initialization failed."); ESP_LOGE(kTag, "Web filesystem unavailable"); }
  diagnostics::Metadata diagnosticBoot;
  if (webReady && diagnostics::getMetadata(diagnosticBoot) &&
      !diagnostic_persistence::initialize("/www/diagnostics", PROJECT_FIRMWARE_VERSION,
          diagnosticBoot.boot_id, diagnosticBoot.reset_cause))
    ESP_LOGW(kTag, "Persistent diagnostic archive startup incomplete; RAM journal available");
  recoveryBudget.boot(retainedRecovery, esp_reset_reason() == ESP_RST_SW);
  recoveryBudgetStatus.store(1U | (retainedRecovery.used << 1) | (retainedRecovery.reason << 16));
  model.wifiReady = initializeWifi();
  if (!model.wifiReady) {
    diagnostics::event(diagnostics::Level::Error, diagnostics::Module::Wifi, uint16_t(0x0202));
    message("Wi-Fi initialization failed."); ESP_LOGE(kTag, "Wi-Fi initialization failed");
  }
  initializeUdp();
  model.airWorkerReady = air_alert::startPolling();
  esp_ota_img_states_t state;
  model.confirmationPending = esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY;
  if (!webReady && model.confirmationPending) {
    ESP_LOGE(kTag, "Rejecting OTA image: web tree could not be installed");
    esp_ota_mark_app_invalid_rollback_and_reboot();
  }
  model.otaAt = uptimeMs() + 30000;
  updateControl(); serviceExternalLight(); publishView();
  model.httpReady = initializeHttp();
  if (!model.httpReady) {
    diagnostic_log::Snapshot failure; failure.reason = 503; failure.detail = 1;
    diagnostics::event(diagnostics::Level::Error, diagnostics::Module::Http, diagnostics::Code::RequestFailed, failure);
    message("HTTP server initialization failed."); ESP_LOGE(kTag, "HTTP server initialization failed");
  }
  if (xTaskCreatePinnedToCore(diagnosticMonitorTask, "diagnostics", 4096, nullptr, 1, nullptr, 0) != pdPASS) {
    diagnostics::event(diagnostics::Level::Error, diagnostics::Module::System, uint16_t(0x0100));
    ESP_LOGW(kTag, "Diagnostic monitor unavailable");
  }
  model.manifestWorkerReady = xTaskCreatePinnedToCore(manifestTask, "manifest", 16384, nullptr, 2, nullptr, 0) == pdPASS;
  TickType_t wake = xTaskGetTickCount();
  bool renderReported = false;
  uint32_t httpRetryAt = uptimeMs() + 5000;
  for (;;) {
    markNetworkProgress(network_progress::Stage::RenderStatus);
    ++model.loops;
    RenderStatus render;
    if (xQueueReceive(renderStatusQueue, &render, 0) == pdTRUE) {
      model.render = render;
      if (!renderReported) {
        ESP_LOGI(kTag, "First render status: frames=%lu led_error=%ld watchdog=%d assets=%d on=%d",
                 (unsigned long)render.frames, (long)render.ledError,
                 render.watchdogReady, render.assetsReady, model.control.on);
        renderReported = true;
      }
    }
    markNetworkProgress(network_progress::Stage::Wifi);
    serviceWifi();
    serviceWifiRecovery();
    markNetworkProgress(network_progress::Stage::Commands);
    serviceCommands();
    markNetworkProgress(network_progress::Stage::Weekly);
    serviceWeekly();
    markNetworkProgress(network_progress::Stage::Telemetry);
    serviceTelemetry();
    markNetworkProgress(network_progress::Stage::Discovery);
    discover();
    markNetworkProgress(network_progress::Stage::ManifestResults);
    serviceManifestResults();
    markNetworkProgress(network_progress::Stage::AirAlert);
    serviceAirAlert();
    markNetworkProgress(network_progress::Stage::Control);
    updateControl();
    markNetworkProgress(network_progress::Stage::ExternalLight);
    serviceExternalLight();
    markNetworkProgress(network_progress::Stage::Buzzer);
    serviceBuzzer();
    markNetworkProgress(network_progress::Stage::Publish);
    publishView();
    markNetworkProgress(network_progress::Stage::Maintenance);
    serviceMaintenance();
    if (!model.httpReady && !model.http && model.control.maintenance == Maintenance::None &&
        !model.releasePending && !web_auth::busy() && due(uptimeMs(), httpRetryAt)) {
      model.httpReady = initializeHttp();
      httpRetryAt = uptimeMs() + 5000;
    }
    // A blocking flash transfer must not leave thousands of overdue polling ticks.
    if (xTaskGetTickCount() - wake > pdMS_TO_TICKS(100)) wake = xTaskGetTickCount();
    completeNetworkProgress();
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(20));
  }
}
