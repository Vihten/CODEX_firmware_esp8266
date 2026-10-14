#include "led_output.hpp"

#include <cstring>
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"

static_assert(PROJECT_PIXELS > 0, "The strip must contain pixels");
static_assert(uint64_t(PROJECT_PIXELS) == uint64_t(PROJECT_WIDTH) * PROJECT_HEIGHT,
              "PROJECT_PIXELS must match the matrix dimensions");
static_assert(uint64_t(PROJECT_PIXELS) <= UINT32_MAX / 3,
              "Pixel storage must fit the driver's 32-bit byte offsets");

LedOutput::~LedOutput() {
  release();
}

void LedOutput::release() {
  ready_ = false;
  if (strip_) {
    const esp_err_t error = led_strip_del(strip_);
    if (error == ESP_OK) {
      strip_ = nullptr;
      quiescent_ = true;
    } else {
      lastError_ = error;
    }
  }
  heap_caps_free(pixels_);
  pixels_ = nullptr;
}

bool LedOutput::begin() {
  if (xPortGetCoreID() != 1 || (owner_ && owner_ != xTaskGetCurrentTaskHandle())) {
    lastError_ = ESP_ERR_INVALID_STATE;
    return false;
  }
  if (ready_) {
    lastError_ = ESP_OK;
    return true;
  }
  if (beginAttempted_) return false;
  beginAttempted_ = true;
  owner_ = xTaskGetCurrentTaskHandle();
  pixels_ = static_cast<uint8_t *>(
      heap_caps_calloc(kPixelBytes, 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!pixels_) {
    lastError_ = ESP_ERR_NO_MEM;
    return false;
  }

  led_strip_config_t config = {};
  config.strip_gpio_num = PROJECT_DATA_PIN;
  config.max_leds = numPixels();
  config.led_model = LED_MODEL_WS2812;
  // Explicit fields keep this valid C++ with both v3.0 and newer v3 headers.
  config.color_component_format.format.r_pos = 1;
  config.color_component_format.format.g_pos = 0;
  config.color_component_format.format.b_pos = 2;
  config.color_component_format.format.w_pos = 3;
  config.color_component_format.format.num_components = 3;
  led_strip_rmt_config_t rmt = {};
  rmt.clk_src = RMT_CLK_SRC_DEFAULT;
  rmt.resolution_hz = 10000000;
  rmt.mem_block_symbols = 512;
  rmt.flags.with_dma = true;
  lastError_ = led_strip_new_rmt_device(&config, &rmt, &strip_);
  if (lastError_ != ESP_OK) {
    release();
    return false;
  }
  // led_strip v3 RMT owns its pixel bytes inline in the returned allocation.
  // Its calloc can select PSRAM under some sdkconfigs: reject that allocation.
  if (!esp_ptr_internal(strip_)) {
    lastError_ = ESP_ERR_NO_MEM;
    release();
    return false;
  }

  ready_ = true;
  clear();
  if (!show()) {
    release();
    return false;
  }
  return true;
}

void LedOutput::setBrightness(uint8_t brightness) {
  const uint8_t next = static_cast<uint8_t>(brightness + 1);
  if (next == brightness_) return;
  // Match Adafruit's intentionally lossy rescaling of previously stored bytes.
  const uint8_t old = static_cast<uint8_t>(brightness_ - 1);
  const uint16_t scale = old == 0 ? 0 : brightness == 255 ? 65535 / old
      : ((uint16_t(next) << 8) - 1) / old;
  if (pixels_) {
    for (size_t i = 0; i < kPixelBytes; ++i) {
      pixels_[i] = static_cast<uint8_t>((uint32_t(pixels_[i]) * scale) >> 8);
    }
  }
  brightness_ = next;
}

void LedOutput::setPixelColor(uint32_t index, uint32_t color) {
  setPixelColor(index, static_cast<uint8_t>(color >> 16),
                static_cast<uint8_t>(color >> 8), static_cast<uint8_t>(color));
}

void LedOutput::setPixelColor(uint32_t index, uint8_t red, uint8_t green, uint8_t blue) {
  if (!ready_ || index >= numPixels()) return;
  if (brightness_) {
    red = static_cast<uint8_t>((uint16_t(red) * brightness_) >> 8);
    green = static_cast<uint8_t>((uint16_t(green) * brightness_) >> 8);
    blue = static_cast<uint8_t>((uint16_t(blue) * brightness_) >> 8);
  }
  uint8_t *pixel = pixels_ + size_t(index) * 3;
  pixel[0] = green;
  pixel[1] = red;
  pixel[2] = blue;
}

uint32_t LedOutput::getPixelColor(uint32_t index) const {
  if (!ready_ || index >= numPixels()) return 0;
  const uint8_t *pixel = pixels_ + size_t(index) * 3;
  if (brightness_) {
    return (((uint32_t(pixel[1]) << 8) / brightness_) << 16) |
           (((uint32_t(pixel[0]) << 8) / brightness_) << 8) |
           ((uint32_t(pixel[2]) << 8) / brightness_);
  }
  return Color(pixel[1], pixel[0], pixel[2]);
}

void LedOutput::clear() {
  if (pixels_) std::memset(pixels_, 0, kPixelBytes);
}

bool LedOutput::show() {
  if (!ready_) return false;
  if (xPortGetCoreID() != 1 || owner_ != xTaskGetCurrentTaskHandle()) {
    lastError_ = ESP_ERR_INVALID_STATE;
    return false;
  }
  for (uint32_t i = 0; i < numPixels(); ++i) {
    const uint8_t *pixel = pixels_ + size_t(i) * 3;
    lastError_ = led_strip_set_pixel(strip_, i, pixel[1], pixel[0], pixel[2]);
    if (lastError_ != ESP_OK) {
      ready_ = false;
      return false;
    }
  }
  // The RMT driver waits on its completion semaphore, yielding the renderer.
  // Do not mask interrupts, spin on timing, or modify its in-flight pixel bytes.
  quiescent_ = false;
  lastError_ = led_strip_refresh(strip_);
  if (lastError_ == ESP_OK) {
    quiescent_ = true;
  } else {
    ready_ = false;
  }
  return lastError_ == ESP_OK;
}
