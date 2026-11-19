#include "led_output.hpp"

#include <cstring>
#include "esp_heap_caps.h"
#include "esp_attr.h"

static_assert(PROJECT_PIXELS > 0, "The strip must contain pixels");
static_assert(uint64_t(PROJECT_PIXELS) == uint64_t(PROJECT_WIDTH) * PROJECT_HEIGHT,
              "PROJECT_PIXELS must match the matrix dimensions");
static_assert(uint64_t(PROJECT_PIXELS) <= UINT32_MAX / 3,
              "Pixel storage must fit the driver's 32-bit byte offsets");

namespace {
constexpr uint32_t kResolutionHz = 10000000;
constexpr uint32_t kRecoveryCooldownMs = 200;
constexpr uint8_t kMaxRecoveryAttempts = 3;
constexpr uint8_t kMaxStopAttempts = 3;
constexpr uint8_t kHealthyTransfersToRearm = 120;
// WS2812 worst case is 24 * 1.5 us per pixel plus the 280 us latch.
// Add scheduling margin. The queue holds only one transfer.
constexpr int kDrainTimeoutMs = int((uint64_t(PROJECT_PIXELS) * 36 + 280 + 999) / 1000) + 40;

size_t IRAM_ATTR encodeWs2812(const void *data, size_t bytes, size_t offset,
                            size_t available, rmt_symbol_word_t *symbols, bool *done, void *) {
  const auto *grb = static_cast<const uint8_t *>(data);
  const size_t bitCount = bytes * 8;
  size_t written = 0;
  // Match the previous WS2812 timings at 10 MHz. No flash data is read in the ISR.
  while (written < available && offset < bitCount) {
    const bool one = (grb[offset / 8] >> (7 - offset % 8)) & 1;
    rmt_symbol_word_t symbol = {};
    symbol.level0 = 1;
    symbol.duration0 = one ? 9 : 3;
    symbol.duration1 = one ? 6 : 9;
    symbols[written++] = symbol;
    ++offset;
  }
  *done = written < available;
  if (*done) {
    rmt_symbol_word_t reset = {};
    reset.duration0 = reset.duration1 = 1400;
    symbols[written++] = reset;
  }
  return written;
}
} // namespace

LedOutput::~LedOutput() {
  if (!enabled_ || (owned() && stop())) {
    if (owned()) destroyChannel();
    if (encoder_) rmt_del_encoder(encoder_);
    heap_caps_free(txPixels_);
  }
  // A channel with unproven stop may still read the payload and encoder.
  heap_caps_free(pixels_);
}

bool LedOutput::owned() const {
  return xPortGetCoreID() == 1 && owner_ == xTaskGetCurrentTaskHandle();
}

bool LedOutput::fail(esp_err_t error) {
  lastError_ = error;
  ready_ = false;
  healthyTransfers_ = 0;
  return false;
}

bool LedOutput::stop() {
  if (!enabled_) return true;
  // ESP32-S3 supports async hardware stop. IDF stops and disconnects GDMA
  // before a successful disable returns, even if a transfer timed out.
  const esp_err_t error = rmt_disable(channel_);
  if (error != ESP_OK) return fail(error);
  enabled_ = false;
  transferUncertain_ = false;
  return true;
}

bool LedOutput::destroyChannel() {
  if (!channel_) return true;
  if (enabled_ || teardownFailed_) return false;
  const esp_err_t error = rmt_del_channel(channel_);
  if (error != ESP_OK) {
    // IDF may have partially destroyed the channel. Never reuse or re-delete it.
    teardownFailed_ = true;
    return fail(error);
  }
  channel_ = nullptr;
  return true;
}

bool LedOutput::begin() {
  if (xPortGetCoreID() != 1 || (owner_ && !owned())) {
    lastError_ = ESP_ERR_INVALID_STATE;
    return false;
  }
  if (beginAttempted_) return ready_;
  beginAttempted_ = true;
  owner_ = xTaskGetCurrentTaskHandle();
  pixels_ = static_cast<uint8_t *>(
      heap_caps_calloc(kPixelBytes, 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!pixels_) return fail(ESP_ERR_NO_MEM);
  txPixels_ = static_cast<uint8_t *>(
      heap_caps_calloc(kPixelBytes, 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!txPixels_) {
    heap_caps_free(pixels_);
    pixels_ = nullptr;
    return fail(ESP_ERR_NO_MEM);
  }
  return start();
}

bool LedOutput::start() {
  if (!encoder_) {
    rmt_simple_encoder_config_t config = {};
    config.callback = encodeWs2812;
    // The callback can fill a buffer of any size, including one symbol.
    config.min_chunk_size = 1;
    const esp_err_t error = rmt_new_simple_encoder(&config, &encoder_);
    if (error != ESP_OK) return fail(error);
  }

  rmt_tx_channel_config_t rmt = {};
  rmt.gpio_num = static_cast<gpio_num_t>(PROJECT_DATA_PIN);
  rmt.clk_src = RMT_CLK_SRC_DEFAULT;
  rmt.resolution_hz = kResolutionHz;
  rmt.mem_block_symbols = 512;
  rmt.trans_queue_depth = 1;
  rmt.flags.with_dma = true;
  const esp_err_t error = rmt_new_tx_channel(&rmt, &channel_);
  if (error != ESP_OK) return fail(error);
  ready_ = true;
  clear();
  return show();
}

bool LedOutput::canRecover() const {
  return !ready_ && txPixels_ && !teardownFailed_ && recoveryAttempts_ < kMaxRecoveryAttempts &&
      (!transferUncertain_ || stopAttempts_ < kMaxStopAttempts);
}

bool LedOutput::recover(uint32_t now, bool restoreOutput) {
  if (!owned() || ready_) return false;
  const bool canStop = transferUncertain_ && stopAttempts_ < kMaxStopAttempts;
  const bool canRestore = restoreOutput && canRecover();
  if (!canStop && !canRestore) return false;
  if ((stopAttempts_ || recoveryAttempts_) && now - lastRecoveryAt_ < kRecoveryCooldownMs)
    return false;
  lastRecoveryAt_ = now;
  // Even when rebuilding is exhausted, stopping the final failed transfer can
  // still prove quiescence for persistence and other maintenance.
  if (canStop) {
    ++stopAttempts_;
    if (!stop()) return false;
  }
  if (!canRestore) return false;
  ++recoveryAttempts_;
  if (!destroyChannel()) return false;
  return start();
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
  envelope_.reset();
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

bool LedOutput::wait() {
  if (!owned()) {
    lastError_ = ESP_ERR_INVALID_STATE;
    return false;
  }
  if (!ready_) return false;
  if (transferUncertain_) {
    const esp_err_t error = rmt_tx_wait_all_done(channel_, kDrainTimeoutMs);
    if (error != ESP_OK) return fail(error);
    // TX_DONE and the completion queue prove the transaction finished.
    // Leave a healthy channel enabled to avoid reconnecting GDMA per frame.
    transferUncertain_ = false;
    // A successful recovery probe alone must not replenish a retry burst.
    if (recoveryAttempts_ && ++healthyTransfers_ == kHealthyTransfersToRearm) {
      recoveryAttempts_ = healthyTransfers_ = 0;
    }
  }
  lastError_ = ESP_OK;
  return true;
}

bool LedOutput::show(bool synchronous) {
  if (!ready_ || !wait()) return false;
  const uint8_t effectiveBrightness = getBrightness();
  // Estimate the exact corrected frame, including raw renderer overlays. Two
  // passes avoid a temporary frame allocation and leave staging/readback intact.
  uint64_t channelSum = 0;
  for (size_t i = 0; i < kPixelBytes; ++i)
    channelSum += led_output_policy::correct(pixels_[i], gamma_, nonzeroFloor_, effectiveBrightness);
  // Commit temporal history only with a successfully submitted frame.
  auto nextEnvelope = envelope_;
  const uint32_t scale = nextEnvelope.update(
      led_output_policy::scaleFor(channelSum, numPixels()));
  const auto output = [this, scale, effectiveBrightness](uint8_t value) {
    return led_output_policy::scaled(
        led_output_policy::correct(value, gamma_, nonzeroFloor_, effectiveBrightness), scale);
  };
  uint64_t outputSum = 0;
  for (size_t i = 0; i < kPixelBytes; ++i) {
    txPixels_[i] = output(pixels_[i]);
    outputSum += txPixels_[i];
  }

  // Treat even an unexpected enable/submit failure as potentially active.
  transferUncertain_ = true;
  stopAttempts_ = 0;
  if (!enabled_) {
    enabled_ = true;
    const esp_err_t error = rmt_enable(channel_);
    if (error != ESP_OK) return fail(error);
  }
  rmt_transmit_config_t config = {};
  config.flags.queue_nonblocking = true;
  const esp_err_t error = rmt_transmit(channel_, encoder_, txPixels_, kPixelBytes, &config);
  if (error != ESP_OK) return fail(error);
  if (synchronous && !wait()) return false;

  envelope_ = nextEnvelope;
  outputScaleQ16_ = scale;
  // Count the actual rounded payload bytes, not the unquantized scale estimate.
  const uint64_t matrixMa = uint64_t(numPixels()) * led_output_policy::kIdlePerPixelMa +
      (outputSum * led_output_policy::kChannelMa + 254) / 255;
  const uint64_t supplyMa = matrixMa + led_output_policy::kControllerMa + led_output_policy::kExternalMa;
  estimatedMatrixCurrentMa_ = matrixMa > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(matrixMa);
  estimatedSupplyCurrentMa_ = supplyMa > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(supplyMa);
  return true;
}
