#pragma once

#include <cstddef>
#include <cstdint>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "project_config.hpp"

// Renderer-owned, non-copyable GRB strip. All access, including raw pixel edits,
// belongs to the renderer task. Construction does not allocate or touch GPIO.
class LedOutput {
public:
  LedOutput() = default;
  ~LedOutput();
  LedOutput(const LedOutput &) = delete;
  LedOutput &operator=(const LedOutput &) = delete;
  LedOutput(LedOutput &&) = delete;
  LedOutput &operator=(LedOutput &&) = delete;

  // Call on renderer core 1. Allocates once and synchronously sends black.
  // A failed initialization stays failed; it never repeatedly churns the heap.
  bool begin();
  bool show();
  esp_err_t lastError() const { return lastError_; }
  // Confirmed RMT idle/no channel; readiness or an error cannot prove this.
  bool isQuiescent() const { return quiescent_; }

  uint32_t numPixels() const { return static_cast<uint32_t>(PROJECT_PIXELS); }
  void setBrightness(uint8_t brightness);
  uint8_t getBrightness() const { return static_cast<uint8_t>(brightness_ - 1); }
  void setPixelColor(uint32_t index, uint32_t color);
  void setPixelColor(uint32_t index, uint8_t red, uint8_t green, uint8_t blue);
  uint32_t getPixelColor(uint32_t index) const;
  static constexpr uint32_t Color(uint8_t red, uint8_t green, uint8_t blue) {
    return (uint32_t(red) << 16) | (uint32_t(green) << 8) | blue;
  }
  void clear();

  // Stable internal-DRAM pointer after begin; already scaled GRB, 3 bytes/pixel.
  // Raw edits are transmitted by show without a second brightness multiplication.
  uint8_t *getPixels() { return ready_ ? pixels_ : nullptr; }
  const uint8_t *getPixels() const { return ready_ ? pixels_ : nullptr; }

private:
  static constexpr size_t kPixelBytes = size_t(PROJECT_PIXELS) * 3;
  void release();

  uint8_t *pixels_ = nullptr;
  led_strip_handle_t strip_ = nullptr;
  TaskHandle_t owner_ = nullptr;
  esp_err_t lastError_ = ESP_ERR_INVALID_STATE;
  uint8_t brightness_ = 0; // Adafruit encoding: 0 means full brightness.
  bool beginAttempted_ = false;
  bool ready_ = false;
  bool quiescent_ = true;
};
