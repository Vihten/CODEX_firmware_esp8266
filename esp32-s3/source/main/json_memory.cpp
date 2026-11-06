#include "json_memory.hpp"
#include "cJSON.h"
#include "esp_heap_caps.h"

namespace {
void *allocateJson(size_t size) {
  // Never borrow the radio/driver reserve when PSRAM is unavailable or full.
  return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
}

bool initializeJsonMemory() {
  if (!heap_caps_get_total_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) return false;
  cJSON_Hooks hooks = {};
  hooks.malloc_fn = allocateJson;
  hooks.free_fn = heap_caps_free;
  cJSON_InitHooks(&hooks);
  return true;
}
