#include "external_light.hpp"
#include "driver/gpio.h"
#include "driver/ledc.h"

namespace external_light {
namespace {
constexpr gpio_num_t pins[] = {static_cast<gpio_num_t>(kFirstPin),
                               static_cast<gpio_num_t>(kSecondPin)};
constexpr ledc_channel_t channels[] = {LEDC_CHANNEL_0, LEDC_CHANNEL_1};
bool attempted = false, initialized = false, timerAttempted = false;
unsigned channelsAttempted = 0;
uint32_t duties[2] = {};

bool driveLow() {
  // Preload BOTH output latches before enabling GPIO output or detaching PWM.
  const esp_err_t first = gpio_set_level(pins[0], 0);
  const esp_err_t second = gpio_set_level(pins[1], 0);
  gpio_config_t config = {};
  config.pin_bit_mask = (1ULL << kFirstPin) | (1ULL << kSecondPin);
  config.mode = GPIO_MODE_OUTPUT;
  config.pull_up_en = GPIO_PULLUP_DISABLE;
  config.pull_down_en = GPIO_PULLDOWN_ENABLE;
  config.intr_type = GPIO_INTR_DISABLE;
  const esp_err_t configured = gpio_config(&config);
  return first == ESP_OK && second == ESP_OK && configured == ESP_OK;
}

bool fail() {
  initialized = false;
  attempted = true;
  for (unsigned i = 0; i < channelsAttempted; ++i)
    (void)ledc_stop(LEDC_LOW_SPEED_MODE, channels[i], 0);
  // gpio_config disconnects peripheral routing even if LEDC cleanup fails.
  (void)driveLow();
  for (unsigned i = 0; i < channelsAttempted; ++i) {
    ledc_channel_config_t config = {};
    config.speed_mode = LEDC_LOW_SPEED_MODE;
    config.channel = channels[i];
    config.deconfigure = true;
    (void)ledc_channel_config(&config);
  }
  if (timerAttempted) {
    (void)ledc_timer_pause(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0);
    ledc_timer_config_t config = {};
    config.speed_mode = LEDC_LOW_SPEED_MODE;
    config.timer_num = LEDC_TIMER_0;
    config.deconfigure = true;
    (void)ledc_timer_config(&config);
  }
  // IDF channel deconfiguration disables GPIO output; restore a driven LOW.
  (void)driveLow();
  duties[0] = duties[1] = 0;
  return false;
}

uint32_t dutyFor(uint8_t percent) {
  return (uint32_t(percent > 100 ? 100 : percent) * 1023U + 50U) / 100U;
}
}  // namespace

bool ready() { return initialized; }

bool initialize() {
  if (attempted) return initialized;
  attempted = true;
  if (!driveLow()) return fail();
  ledc_timer_config_t timer = {};
  timer.speed_mode = LEDC_LOW_SPEED_MODE;
  timer.duty_resolution = LEDC_TIMER_10_BIT;
  timer.timer_num = LEDC_TIMER_0;
  timer.freq_hz = 1000;
  timer.clk_cfg = LEDC_AUTO_CLK;
  timerAttempted = true;
  if (ledc_timer_config(&timer) != ESP_OK) return fail();
  for (unsigned i = 0; i < 2; ++i) {
    ledc_channel_config_t channel = {};
    channel.gpio_num = pins[i];
    channel.speed_mode = LEDC_LOW_SPEED_MODE;
    channel.channel = channels[i];
    channel.timer_sel = LEDC_TIMER_0;
    channel.duty = 0;
    channel.hpoint = 0;
    channelsAttempted = i + 1;
    if (ledc_channel_config(&channel) != ESP_OK) return fail();
  }
  initialized = true;
  return true;
}

bool apply(uint8_t first, uint8_t second, bool allowed) {
  if (!initialized) {
    // A premature call must not prevent the later, explicit initialization.
    if (!attempted && !driveLow()) return fail();
    return false;
  }
  const uint32_t next[] = {allowed ? dutyFor(first) : 0,
                           allowed ? dutyFor(second) : 0};
  for (unsigned i = 0; i < 2; ++i) {
    if (next[i] == duties[i]) continue;
    // stop gives an immediate LOW; set/update resumes a previously stopped channel.
    if (next[i] == 0) {
      if (ledc_stop(LEDC_LOW_SPEED_MODE, channels[i], 0) != ESP_OK)
        return fail();
    } else if (ledc_set_duty(LEDC_LOW_SPEED_MODE, channels[i], next[i]) != ESP_OK ||
               ledc_update_duty(LEDC_LOW_SPEED_MODE, channels[i]) != ESP_OK) {
      return fail();
    }
    duties[i] = next[i];
  }
  return true;
}
}  // namespace external_light
