#include "runtime.hpp"
#include "external_light.hpp"
#include "buzzer.hpp"
#include "diagnostics.hpp"
#include "json_memory.hpp"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_flash.h"
#include "esp_ota_ops.h"
#include "esp_system.h"

namespace {
bool pendingVerification() {
  esp_ota_img_states_t state;
  const esp_err_t result = esp_ota_get_state_partition(esp_ota_get_running_partition(), &state);
  if (result == ESP_OK) return state == ESP_OTA_IMG_PENDING_VERIFY;
  if (result != ESP_ERR_NOT_FOUND && result != ESP_ERR_NOT_SUPPORTED)
    ESP_LOGE("boot", "Could not read running OTA state: %s", esp_err_to_name(result));
  return false;
}

void startRenderer(void *argument) {
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  renderTask(argument);
  vTaskDelete(nullptr);
}

void startNetwork(void *argument) {
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  networkTask(*static_cast<const bool *>(argument));
  vTaskDelete(nullptr);
}
}

extern "C" void app_main() {
  // Power outputs must be off before assets, storage or network initialization.
  if (!external_light::initialize()) ESP_LOGE("boot", "External lighting disabled: PWM initialization failed");
  if (!buzzer::initialize()) ESP_LOGE("boot", "Buzzer disabled: PWM initialization failed");
  gpio_set_direction(static_cast<gpio_num_t>(PROJECT_DATA_PIN), GPIO_MODE_OUTPUT);
  gpio_set_level(static_cast<gpio_num_t>(PROJECT_DATA_PIN), 0);
  // Read once before allocations. A low-memory startup failure must still roll back.
  static bool trialBoot = pendingVerification();
  const auto failBoot = [](const char *reason) {
    ESP_LOGE("boot", "%s", reason);
    if (trialBoot) esp_restart();
  };
  uint32_t flashSize = 0;
  if (esp_flash_get_size(nullptr, &flashSize) != ESP_OK || flashSize < 16 * 1024 * 1024) {
    failBoot("This firmware requires 16 MB flash; refusing to start");
    return;
  }
  if (!initializeJsonMemory()) {
    failBoot("PSRAM unavailable for bounded web JSON storage");
    return;
  }
  if (!initializeRuntime()) {
    failBoot("Cannot create bounded IPC queues");
    return;
  }
  if (!diagnostics::initialize(PROJECT_FIRMWARE_VERSION))
    ESP_LOGW("boot", "Diagnostic journal unavailable; firmware continues");
  loadAssets();
  TaskHandle_t render = nullptr;
  // Neither owner may touch peripherals before both tasks exist.
  if (xTaskCreatePinnedToCore(startRenderer, "scene", 12288, nullptr, 4, &render, 1) != pdPASS) {
    failBoot("Cannot start renderer on core 1");
    return;
  }
  TaskHandle_t network = nullptr;
  if (xTaskCreatePinnedToCore(startNetwork, "network", 16384, &trialBoot, 3, &network, 0) != pdPASS) {
    vTaskDelete(render);
    gpio_set_level(static_cast<gpio_num_t>(PROJECT_DATA_PIN), 0);
    failBoot("Cannot start network owner on core 0");
    return;
  }
  xTaskNotifyGive(network);
  xTaskNotifyGive(render);
}
