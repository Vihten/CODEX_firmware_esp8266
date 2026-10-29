#include "buzzer.hpp"
#include "driver/gpio.h"
#include "driver/ledc.h"

namespace buzzer {
namespace {
constexpr gpio_num_t pin = static_cast<gpio_num_t>(kPin);
constexpr ledc_mode_t mode = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t timer = LEDC_TIMER_1;
constexpr ledc_channel_t channel = LEDC_CHANNEL_2;
bool attempted = false, initialized = false, active = false;
bool timerAttempted = false, channelAttempted = false;
uint32_t startedAt = 0;

bool driveLow() {
  // Preload LOW before enabling GPIO output or disconnecting peripheral routing.
  const esp_err_t level = gpio_set_level(pin, 0);
  gpio_config_t config = {};
  config.pin_bit_mask = 1ULL << kPin;
  config.mode = GPIO_MODE_OUTPUT;
  config.pull_up_en = GPIO_PULLUP_DISABLE;
  config.pull_down_en = GPIO_PULLDOWN_ENABLE;
  config.intr_type = GPIO_INTR_DISABLE;
  const esp_err_t configured = gpio_config(&config);
  return level == ESP_OK && configured == ESP_OK;
}

bool fail() {
  attempted = true;
  initialized = active = false;
  if (channelAttempted) (void)ledc_stop(mode, channel, 0);
  // Detach PWM even when stop/deconfiguration fails.
  (void)driveLow();
  if (channelAttempted) {
    ledc_channel_config_t config = {};
    config.speed_mode = mode;
    config.channel = channel;
    config.deconfigure = true;
    (void)ledc_channel_config(&config);
  }
  if (timerAttempted) {
    (void)ledc_timer_pause(mode, timer);
    ledc_timer_config_t config = {};
    config.speed_mode = mode;
    config.timer_num = timer;
    config.deconfigure = true;
    (void)ledc_timer_config(&config);
  }
  // IDF channel deletion disables GPIO output; restore a driven LOW afterward.
  (void)driveLow();
  return false;
}
}  // namespace

bool ready() { return initialized; }

bool initialize() {
  if (attempted) return initialized;
  attempted = true;
  if (!driveLow()) return fail();
  ledc_timer_config_t timerConfig = {};
  timerConfig.speed_mode = mode;
  timerConfig.duty_resolution = LEDC_TIMER_10_BIT;
  timerConfig.timer_num = timer;
  timerConfig.freq_hz = 4000;
  timerConfig.clk_cfg = LEDC_AUTO_CLK;
  timerAttempted = true;
  if (ledc_timer_config(&timerConfig) != ESP_OK) return fail();
  ledc_channel_config_t channelConfig = {};
  channelConfig.gpio_num = kPin;
  channelConfig.speed_mode = mode;
  channelConfig.channel = channel;
  channelConfig.timer_sel = timer;
  channelConfig.duty = 0;
  channelConfig.hpoint = 0;
  channelAttempted = true;
  if (ledc_channel_config(&channelConfig) != ESP_OK) return fail();
  initialized = true;
  return true;
}

bool start(uint32_t now) {
  if (!initialized || active) return false;
  if (ledc_set_duty(mode, channel, 512) != ESP_OK ||
      ledc_update_duty(mode, channel) != ESP_OK) return fail();
  startedAt = now;
  active = true;
  return true;
}

void service(uint32_t now, bool allowed) {
  if (!initialized || !active) return;
  if (allowed && uint32_t(now - startedAt) < kDurationMs) return;
  if (ledc_stop(mode, channel, 0) != ESP_OK) {
    (void)fail();
    return;
  }
  active = false;
}
}  // namespace buzzer
