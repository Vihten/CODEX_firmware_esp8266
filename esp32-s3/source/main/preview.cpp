#include "preview.hpp"

#include <atomic>
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static_assert(PROJECT_WIDTH > 0 && PROJECT_WIDTH <= 128 && PROJECT_HEIGHT > 0 && PROJECT_HEIGHT <= 128,
              "Preview dimensions must fit the matrix protocol");
static_assert(PROJECT_PIXELS == PROJECT_WIDTH * PROJECT_HEIGHT, "Preview size mismatch");

namespace {
StaticSemaphore_t mutexStorage;
SemaphoreHandle_t mutex = nullptr;
std::atomic<uint32_t> requestedAt{0};
std::atomic<bool> requested{false};
uint8_t packet[kPreviewFrameBytes] = {};
uint32_t capturedAt = 0;
bool valid = false;

void little16(uint8_t *target, uint16_t value) {
  target[0] = uint8_t(value); target[1] = uint8_t(value >> 8);
}
void little32(uint8_t *target, uint32_t value) {
  for (unsigned byte = 0; byte < 4; ++byte) target[byte] = uint8_t(value >> (byte * 8));
}
}

bool initializePreview() {
  if (!mutex) mutex = xSemaphoreCreateMutexStatic(&mutexStorage);
  return mutex != nullptr;
}

void capturePreview(const uint8_t *grb, uint8_t brightness, uint32_t sequence,
                    uint32_t now, PreviewMapping mapping) {
  if (!mutex || !grb || !mapping || !requested.load(std::memory_order_acquire) ||
      uint32_t(now - requestedAt.load(std::memory_order_relaxed)) > 1500) return;
  if (xSemaphoreTake(mutex, 0) != pdTRUE) return;
  if (valid && uint32_t(now - capturedAt) < 66) { xSemaphoreGive(mutex); return; }
  std::memcpy(packet, "LFP1", 4);
  little16(packet + 4, PROJECT_WIDTH); little16(packet + 6, PROJECT_HEIGHT);
  little32(packet + 8, sequence); little32(packet + 12, now);
  packet[16] = brightness; packet[17] = 1;
  for (unsigned y = 0; y < PROJECT_HEIGHT; ++y) {
    for (unsigned x = 0; x < PROJECT_WIDTH; ++x) {
      const size_t source = size_t(mapping(uint8_t(x), uint8_t(y))) * 3;
      const size_t destination = kPreviewHeaderBytes + (y * PROJECT_WIDTH + x) * 3;
      packet[destination] = grb[source + 1];
      packet[destination + 1] = grb[source];
      packet[destination + 2] = grb[source + 2];
    }
  }
  capturedAt = now; valid = true;
  xSemaphoreGive(mutex);
}

bool copyPreview(uint8_t *output, size_t capacity, uint32_t now) {
  if (!mutex || !output || capacity < kPreviewFrameBytes) return false;
  requestedAt.store(now, std::memory_order_relaxed);
  requested.store(true, std::memory_order_release);
  if (xSemaphoreTake(mutex, pdMS_TO_TICKS(2)) != pdTRUE) return false;
  const bool fresh = valid && uint32_t(now - capturedAt) <= 500;
  if (fresh) std::memcpy(output, packet, sizeof(packet));
  xSemaphoreGive(mutex);
  return fresh;
}
