#include "runtime.hpp"
#include "web_files.hpp"
#include "web_auth.hpp"
#include "preview.hpp"
#include "network_progress.hpp"
#include "network_memory.hpp"
#include "http_failure.hpp"
#include "clock_recovery.hpp"
#include "recovery.hpp"
#include "esp_attr.h"
#include "diagnostics.hpp"
#include "diagnostic_persistence.hpp"
#include "diagnostic_http.hpp"
#include "weekly.hpp"
#include "quota_filter.hpp"
#include "quota_metadata.hpp"
#include "portal_strings.hpp"
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

// One translation unit intentionally owns Model, queues, snapshots and maintenance.
// Implementation units below share private state without exporting mutable globals.
// HTTP/event workers enqueue requests; only networkTask mutates configuration.
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
constexpr uint32_t kOtaCycleMs = 180000, kManifestBudgetMs = 30000;
constexpr uint32_t kOtaVisualAckMs = 1000, kOtaRecoveryMs = 2000, kBootConfirmationMs = 60000;
constexpr uint32_t kAirPollIntervalMs = 15000;
std::atomic<uint32_t> lastClockSyncAt{0};
std::atomic<bool> clockSyncSeen{false};
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
  uint32_t deadline = 0;
};
struct ManifestResult {
  Manifest manifest;
  bool newer = false;
  bool upToDate = false;
  esp_err_t reason = ESP_FAIL;
  unsigned retries = 0;
  uint32_t elapsed = 0;
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
  uint32_t clockRetries = 0;
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
  city_presence::Sound citySound;
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
  uint32_t otaDeadline = 0, otaRecoveryAt = 0, releaseAt = 0;
  uint32_t confirmationDeadline = 0, httpRetryAt = 0;
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
  quota_metadata::Entry codexMetadata[kAgentCapacity], claudeMetadata[kAgentCapacity];
  bool nvsReady = false, dirty = false, wifiChanged = false, stationConnecting = false;
  bool restarting = false, updateRequested = false, manifestReady = false;
  bool releasePending = false, confirmationPending = false;
  clock_recovery::State clockRecovery;
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
#include "network/settings_store.inc"

#include "network/telemetry.inc"

#include "network/wifi_manager.inc"

struct HandlerScope { HandlerScope() { ++activeHandlers; } ~HandlerScope() { --activeHandlers; } };
esp_err_t error(httpd_req_t *request, const char *status, const char *text) {
  const int code = std::atoi(status);
  if (code >= 500) {
    diagnostic_log::Snapshot failure; failure.reason = code;
    const auto cause = static_cast<uint32_t>(http_failure::classify(text));
    const auto route = static_cast<uint32_t>(http_failure::route(request->uri));
    // Pack curated route/cause IDs without repurposing the retry counter.
    failure.detail = int32_t((route << 16) | cause);
    diagnostics::event(diagnostics::Level::Error, diagnostics::Module::Http,
                       diagnostics::Code::RequestFailed, failure);
  }
  httpd_resp_set_status(request, status); httpd_resp_set_type(request, "text/plain; charset=utf-8");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store"); httpd_resp_set_hdr(request, "Connection", "close");
  const esp_err_t result = httpd_resp_send(request, text, HTTPD_RESP_USE_STRLEN);
  httpd_sess_trigger_close(request->handle, httpd_req_to_sockfd(request));
  return result;
}
#include "network/portal_html.inc"

#include "network/portal_api.inc"

#include "network/commands.inc"

#include "network/ota.inc"

void startHttpServer(bool retry) {
  model.httpReady = initializeHttp();
  if (model.httpReady) {
    if (retry) {
      diagnostics::event(diagnostics::Level::Info, diagnostics::Module::Http, diagnostics::Code::RequestRecovered);
      ESP_LOGI(kTag, "HTTP server restored");
    }
    return;
  }
  model.httpRetryAt = uptimeMs() + 5000;
  diagnostic_log::Snapshot failure; failure.reason = 503; failure.detail = 1;
  diagnostics::event(diagnostics::Level::Error, diagnostics::Module::Http, diagnostics::Code::RequestFailed, failure);
  if (!retry) message("HTTP server initialization failed.");
  ESP_LOGE(kTag, "HTTP server initialization failed; retry scheduled");
}

void serviceHttpRetry() {
  if (!model.httpReady && !model.http && model.control.maintenance == Maintenance::None &&
      !model.releasePending && !web_auth::busy() && due(uptimeMs(), model.httpRetryAt))
    startHttpServer(true);
}

void networkTask(void *) {
  markNetworkProgress(network_progress::Stage::Starting);
  model.confirmationDeadline = uptimeMs() + kBootConfirmationMs;
  esp_ota_img_states_t state;
  const esp_err_t stateResult = esp_ota_get_state_partition(esp_ota_get_running_partition(), &state);
  model.confirmationPending = stateResult == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY;
  if (model.confirmationPending) otaMessage("Waiting for OTA boot confirmation");
  else if (stateResult != ESP_OK && stateResult != ESP_ERR_NOT_FOUND && stateResult != ESP_ERR_NOT_SUPPORTED)
    ESP_LOGE(kTag, "Could not read running OTA state: %s", esp_err_to_name(stateResult));
  viewMutex = xSemaphoreCreateMutexStatic(&viewMutexStorage);
  wifiEvents = xQueueCreateStatic(16, sizeof(WifiEvent), wifiEventBytes, &wifiEventStorage);
  telemetryQueue = xQueueCreateStatic(4, sizeof(TelemetryRequest), telemetryBytes, &telemetryStorage);
  telemetryReplies = xQueueCreateStatic(4, sizeof(TelemetryReply), telemetryReplyBytes, &telemetryReplyStorage);
  manifestJobs = xQueueCreateStatic(1, sizeof(ManifestJob), manifestJobBytes, &manifestJobStorage);
  manifestResults = xQueueCreateStatic(1, sizeof(ManifestResult), manifestResultBytes, &manifestResultStorage);
  if (!viewMutex || !wifiEvents || !telemetryQueue || !telemetryReplies || !manifestJobs || !manifestResults) {
    if (model.confirmationPending) rejectPendingBoot("OTA boot failed: network IPC unavailable", ESP_ERR_NO_MEM);
    ESP_LOGE(kTag, "Cannot initialize network IPC"); vTaskDelete(nullptr); return;
  }
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
  if (!webReady && model.confirmationPending)
    rejectPendingBoot("OTA boot failed: web tree could not be installed", ESP_FAIL);
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
  if (model.confirmationPending && due(uptimeMs(), model.confirmationDeadline))
    rejectPendingBoot("OTA boot confirmation timed out during startup", ESP_ERR_TIMEOUT);
  model.otaAt = uptimeMs() + 30000;
  updateControl(); serviceExternalLight(); publishView();
  startHttpServer(false);
  if (xTaskCreatePinnedToCore(diagnosticMonitorTask, "diagnostics", 4096, nullptr, 1, nullptr, 0) != pdPASS) {
    diagnostics::event(diagnostics::Level::Error, diagnostics::Module::System, uint16_t(0x0100));
    ESP_LOGW(kTag, "Diagnostic monitor unavailable");
  }
  model.manifestWorkerReady = xTaskCreatePinnedToCore(manifestTask, "manifest", 16384, nullptr, 2, nullptr, 0) == pdPASS;
  TickType_t wake = xTaskGetTickCount();
  bool renderReported = false;
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
    serviceClock();
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
    serviceHttpRetry();
    // A blocking flash transfer must not leave thousands of overdue polling ticks.
    if (xTaskGetTickCount() - wake > pdMS_TO_TICKS(100)) wake = xTaskGetTickCount();
    completeNetworkProgress();
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(model.control.on ? 20 : 50));
  }
}
