#include "runtime.hpp"
#include "preview.hpp"
#include <cstring>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"

QueueHandle_t controlQueue = nullptr;
QueueHandle_t renderStatusQueue = nullptr;
QueueHandle_t commandQueue = nullptr;
AssetStore assets;

namespace {
StaticQueue_t controlStorage, statusStorage, commandStorage;
alignas(4) uint8_t controlBytes[sizeof(ControlFrame)];
alignas(4) uint8_t statusBytes[sizeof(RenderStatus)];
alignas(4) uint8_t commandBytes[8 * sizeof(NetworkCommand)];
extern const uint8_t animationStart[] asm("_binary_animation_bin_start");
extern const uint8_t animationEnd[] asm("_binary_animation_bin_end");
extern const uint8_t effectsStart[] asm("_binary_effects_bin_start");
extern const uint8_t effectsEnd[] asm("_binary_effects_bin_end");
}

bool initializeRuntime() {
  controlQueue = xQueueCreateStatic(1, sizeof(ControlFrame), controlBytes, &controlStorage);
  renderStatusQueue = xQueueCreateStatic(1, sizeof(RenderStatus), statusBytes, &statusStorage);
  commandQueue = xQueueCreateStatic(8, sizeof(NetworkCommand), commandBytes, &commandStorage);
  return controlQueue && renderStatusQueue && commandQueue && initializePreview();
}

bool loadAssets() {
  constexpr size_t pixels = PROJECT_WIDTH * PROJECT_HEIGHT;
  const size_t animationSize = animationEnd - animationStart;
  const size_t effectsSize = effectsEnd - effectsStart;
  if (!esp_psram_is_initialized() || esp_psram_get_size() < 8 * 1024 * 1024 ||
      animationSize != PROJECT_FRAMES * pixels * 3 || effectsSize != PROJECT_FRAMES * pixels) {
    ESP_LOGE("assets", "PSRAM or embedded artwork size mismatch");
    return false;
  }
  auto *animation = static_cast<uint8_t *>(heap_caps_malloc(animationSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  auto *effects = static_cast<uint8_t *>(heap_caps_malloc(effectsSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!animation || !effects) {
    heap_caps_free(animation);
    heap_caps_free(effects);
    ESP_LOGE("assets", "Cannot allocate PSRAM artwork cache");
    return false;
  }
  std::memcpy(animation, animationStart, animationSize);
  std::memcpy(effects, effectsStart, effectsSize);
  assets = {animation, effects, animationSize, effectsSize};
  ESP_LOGI("assets", "Cached %u bytes in %u-byte PSRAM", unsigned(animationSize + effectsSize), unsigned(esp_psram_get_size()));
  return true;
}
