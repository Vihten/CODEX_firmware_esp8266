#pragma once

#include <cstddef>
#include <cstdint>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "project_config.hpp"

constexpr size_t kAgentCapacity = 12;
enum class Maintenance : uint8_t { None, Persist, Ota, Restart };

struct AgentInput {
  uint32_t clientId = 0;
  uint32_t rgb = 0;
  uint32_t seenAt = 0;
  int16_t remaining = -1;
  uint8_t activity = 0;
  bool present = false;
  char name[49] = {};
};

struct ControlFrame {
  uint8_t remainingPercent = 100;
  uint8_t activityPercent = 0;
  uint8_t brightness = PROJECT_BRIGHTNESS;
  bool connected = false;
  bool accountLinked = false;
  bool on = true;
  bool memorialActive = false;
  uint32_t memorialStartedAt = 0;
  uint32_t memorialDurationMs = 60000;
  Maintenance maintenance = Maintenance::None;
  uint32_t maintenanceEpoch = 0;
  char ip[16] = "192.168.4.1";
  AgentInput agents[kAgentCapacity] = {};
};

struct RenderStatus {
  uint32_t frames = 0;
  uint32_t missedDeadlines = 0;
  uint32_t lastFrameUs = 0;
  uint32_t maintenanceEpoch = 0;
  uint32_t sceneProgress = 0;
  uint32_t stackFreeBytes = 0;
  int32_t ledError = 0;
  uint8_t activeMeteors = 0;
  bool assetsReady = false;
  bool watchdogReady = false;
  bool quiescent = false;
  bool sceneVisible = false;
};

enum class CommandKind : uint8_t { Wifi, Power, Brightness, Schedule, Color, Restart, Update, AgentPoll, AgentUpgrade, AgentRename, AgentUninstall, MemorialSave, MemorialPlay, MemorialStop };
struct NetworkCommand {
  uint64_t agentVersion = 0;
  uint64_t agentNonceCounter = 0;
  uint64_t agentFailedVersion = 0;
  uint64_t agentActionAck = 0;
  bool agentManagement = false;
  uint8_t agentActionResult = 0;
  char agentName[49] = {};
  CommandKind kind = CommandKind::Update;
  uint32_t clientId = 0;
  uint32_t rgb = 0;
  uint16_t start = 0;
  uint16_t end = 0;
  uint8_t value = 0;
  uint8_t days = 0;
  bool enabled = false;
  bool automatic = false;
  char ssid[33] = {};
  char password[65] = {};
};

struct AssetStore {
  uint8_t *animation = nullptr;
  uint8_t *effects = nullptr;
  size_t animationSize = 0;
  size_t effectsSize = 0;
};

extern QueueHandle_t controlQueue;
extern QueueHandle_t renderStatusQueue;
extern QueueHandle_t commandQueue;
extern AssetStore assets;

bool initializeRuntime();
bool loadAssets();
void renderTask(void *argument);
void networkTask(void *argument);

inline uint32_t uptimeMs() {
  return static_cast<uint32_t>(xTaskGetTickCount() * portTICK_PERIOD_MS);
}
