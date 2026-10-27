#include "runtime.hpp"
#include "external_light.hpp"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_flash.h"

namespace {
void startRenderer(void *argument) {
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  renderTask(argument);
  vTaskDelete(nullptr);
}

void startNetwork(void *argument) {
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  networkTask(argument);
  vTaskDelete(nullptr);
}
}

extern "C" void app_main() {
  // Power outputs must be off before assets, storage or network initialization.
  if (!external_light::initialize()) ESP_LOGE("boot", "External lighting disabled: PWM initialization failed");
  gpio_set_direction(static_cast<gpio_num_t>(PROJECT_DATA_PIN), GPIO_MODE_OUTPUT);
  gpio_set_level(static_cast<gpio_num_t>(PROJECT_DATA_PIN), 0);
  uint32_t flashSize = 0;
  if (esp_flash_get_size(nullptr, &flashSize) != ESP_OK || flashSize < 16 * 1024 * 1024) {
    ESP_LOGE("boot", "This firmware requires 16 MB flash; refusing to start");
    return;
  }
  if (!initializeRuntime()) {
    ESP_LOGE("boot", "Cannot create bounded IPC queues");
    return;
  }
  loadAssets();
  TaskHandle_t render = nullptr;
  // Neither owner may touch peripherals before both tasks exist.
  if (xTaskCreatePinnedToCore(startRenderer, "scene", 12288, nullptr, 4, &render, 1) != pdPASS) {
    ESP_LOGE("boot", "Cannot start renderer on core 1");
    return;
  }
  TaskHandle_t network = nullptr;
  if (xTaskCreatePinnedToCore(startNetwork, "network", 16384, nullptr, 3, &network, 0) != pdPASS) {
    vTaskDelete(render);
    gpio_set_level(static_cast<gpio_num_t>(PROJECT_DATA_PIN), 0);
    ESP_LOGE("boot", "Cannot start network owner on core 0");
    return;
  }
  xTaskNotifyGive(network);
  xTaskNotifyGive(render);
}
