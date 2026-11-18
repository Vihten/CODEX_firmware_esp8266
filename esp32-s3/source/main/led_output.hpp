#pragma once

#include <cstddef>
#include <cstdint>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "project_config.hpp"

// Pure integer output policy. Current is a conservative model, not a sensor.
namespace led_output_policy {
constexpr uint32_t kSupplyMa = 8000; // 5 V / 8 A supply
constexpr uint32_t kHeadroomMa = 800;
constexpr uint32_t kControllerMa = 500;
constexpr uint32_t kExternalMa = 250; // Four ~50 mA LEDs / 37-ohm branches, plus margin
constexpr uint32_t kIdlePerPixelMa = 1;
constexpr uint32_t kChannelMa = 20; // 60 mA/pixel at full white, plus idle
constexpr uint32_t kUnity = 65536;

constexpr uint8_t correct(uint8_t value, bool gamma, uint8_t floor,
                          uint8_t effectiveBrightness = 255) {
  if (!value) return 0; // Never illuminate black or absent color channels.
  uint8_t curved = value;
  if (gamma) {
    if (!effectiveBrightness) return 0; // Floor must not resurrect brightness zero.
    // Staging already includes Adafruit brightness. Restore its normalized
    // channel (bounded for raw edits), curve, then reapply effective brightness.
    // Integer nearest rounding keeps a full channel at brightness 30 equal to 30.
    const uint32_t restored = (uint32_t(value) * 255 + effectiveBrightness / 2) /
        effectiveBrightness;
    const uint32_t normalized = restored > 255 ? 255 : restored;
    const uint32_t curvedFull = (normalized * normalized + 127) / 255;
    curved = static_cast<uint8_t>((curvedFull * effectiveBrightness + 127) / 255);
  }
  return curved < floor ? floor : curved;
}

constexpr uint32_t dynamicBudgetMa(uint32_t pixels) {
  const uint64_t fixed = uint64_t(kHeadroomMa) + kControllerMa + kExternalMa +
      uint64_t(pixels) * kIdlePerPixelMa;
  return fixed >= kSupplyMa ? 0 : static_cast<uint32_t>(kSupplyMa - fixed);
}

constexpr uint32_t scaleFor(uint64_t correctedChannelSum, uint32_t pixels) {
  const uint64_t available = uint64_t(dynamicBudgetMa(pixels)) * 255;
  const uint64_t demand = correctedChannelSum * kChannelMa;
  return demand == 0 || demand <= available ? kUnity
      : static_cast<uint32_t>(available * kUnity / demand);
}

// Peak-latched current gain: never brighten automatically between unequal
// scene/activity peaks. Only an actual user brightness change resets the latch.
// A new larger load still attacks immediately; black never resets the latch.
class Envelope {
 public:
  void reset() { scale_ = kUnity; }

  uint32_t update(uint32_t required) {
    if (required < scale_) scale_ = required;
    return scale_;
  }

 private:
  uint32_t scale_ = kUnity;
};

constexpr uint8_t scaled(uint8_t value, uint32_t scale) {
  return static_cast<uint8_t>(uint64_t(value) * scale / kUnity);
}
} // namespace led_output_policy

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
  // Async submits the driver buffer and returns immediately. The next show
  // drains it before copying staging pixels. Startup/black output stays sync.
  bool show(bool synchronous = true);
  // Renderer core/task only. Drains pending output, including after a failure.
  bool wait();
  esp_err_t lastError() const { return lastError_; }
  // Confirmed RMT idle/no channel; readiness or an error cannot prove this.
  bool isQuiescent() const { return quiescent_; }

  uint32_t numPixels() const { return static_cast<uint32_t>(PROJECT_PIXELS); }
  // Renderer task only; renderer selects gamma for appropriate content/modes.
  // Gamma 2.0 normalizes staged bytes by getBrightness(), curves, then reapplies
  // brightness. Raw paths must also contain brightness-scaled bytes; values above
  // effective brightness saturate only when gamma is enabled. Readback is unchanged.
  // A floor of 1 preserves surviving dim channels, never recovers staged zeros.
  // Final current limiting has priority over the floor (and may output zero).
  void setOutputCorrection(bool gamma, uint8_t nonzeroFloor = 1) {
    gamma_ = gamma;
    nonzeroFloor_ = nonzeroFloor;
  }
  // Actual changes reset the peak latch; repeated per-frame values do not.
  void setBrightness(uint8_t brightness);
  uint8_t getBrightness() const { return static_cast<uint8_t>(brightness_ - 1); }
  // Last successfully submitted frame (async need not have completed). Estimates
  // include rounded-up PWM current and idle; zero before the first submission.
  // Same renderer-task ownership as other accessors; no cross-task synchronization.
  // Transform a staging byte exactly as the last successful submission did.
  uint8_t submittedChannel(uint8_t value) const {
    return led_output_policy::scaled(led_output_policy::correct(
        value, gamma_, nonzeroFloor_, getBrightness()), outputScaleQ16_);
  }
  uint8_t getOutputLimitPercent() const {
    return uint8_t(outputScaleQ16_ * 100U / led_output_policy::kUnity);
  }
  uint32_t getOutputScaleQ16() const { return outputScaleQ16_; } // 65536 = unity
  uint32_t getEstimatedMatrixCurrentMa() const { return estimatedMatrixCurrentMa_; }
  uint32_t getEstimatedSupplyCurrentMa() const { return estimatedSupplyCurrentMa_; }
  void setPixelColor(uint32_t index, uint32_t color);
  void setPixelColor(uint32_t index, uint8_t red, uint8_t green, uint8_t blue);
  uint32_t getPixelColor(uint32_t index) const;
  static constexpr uint32_t Color(uint8_t red, uint8_t green, uint8_t blue) {
    return (uint32_t(red) << 16) | (uint32_t(green) << 8) | blue;
  }
  void clear();

  // Stable internal-DRAM staging after begin; scaled GRB, 3 bytes/pixel.
  // May be edited while the separate driver buffer is in flight.
  // show applies output correction/current limiting only to the driver copy,
  // without a second brightness multiplication or changing this staging buffer.
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
  bool gamma_ = false;
  uint8_t nonzeroFloor_ = 0;
  led_output_policy::Envelope envelope_;
  uint32_t outputScaleQ16_ = led_output_policy::kUnity;
  uint32_t estimatedMatrixCurrentMa_ = 0;
  uint32_t estimatedSupplyCurrentMa_ = 0;
  bool beginAttempted_ = false;
  bool ready_ = false;
  bool quiescent_ = true;
  bool pending_ = false;
};
