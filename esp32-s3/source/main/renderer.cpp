// Export template: prepare_esp32s3.dart inserts the authoritative legacy scene.
#include "runtime.hpp"
#include "led_output.hpp"
#include "subpixel.hpp"
#include "preview.hpp"
#include "memorial_draw.hpp"
#include "air_alert_visual.hpp"
#include "quota_recovery.hpp"
#include "ota_visual.hpp"
#include "external_light.hpp"
#include "light_sequence.hpp"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_log.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
#define NUM_LEDS PROJECT_PIXELS
#define MATRIX_WIDTH PROJECT_WIDTH
#define MATRIX_HEIGHT PROJECT_HEIGHT
#define FRAME_COUNT PROJECT_FRAMES
#define FRAME_BYTES (NUM_LEDS * 3UL)
#define EFFECT_BYTES NUM_LEDS
#define EVENT_FPS PROJECT_FPS
#define EVENT_MODE 1
#define ACTIVITY_EFFECTS 1
#define DIRECT_AGENT_CAPACITY kAgentCapacity
#define DIRECT_AGENT_TIMEOUT_MS 7000UL
#define PROGMEM
#define PI 3.14159265358979323846
#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))
#define constrain(x, low, high) ((x) < (low) ? (low) : ((x) > (high) ? (high) : (x)))
#define pgm_read_byte(address) (*reinterpret_cast<const uint8_t *>(address))
#define pgm_read_dword(address) (*reinterpret_cast<const uint32_t *>(address))

uint32_t millis() { return uptimeMs(); }
LedOutput strip;
ControlFrame control;
RenderStatus status;
uint16_t effectSubTick = 0;
uint32_t sceneFrameMilli = 0;
uint32_t quotaFrameMilli = 0, skySampleAt = 0;
bool skySampleReady = false;
external_light::Sequence lightSequence;
bool matrixDark = true;
bool lightPresentationActive = false;
bool lightOpeningReady = true;
air_alert::Visual airAlertVisual;
quota_recovery::Motion quotaRecovery;

enum SeekMode { SeekSet };
class FrameReader {
 public:
  FrameReader(const uint8_t *bytes, size_t length) : bytes_(bytes), length_(length) {}
  explicit operator bool() const { return bytes_ != nullptr; }
  size_t size() const { return length_; }
  bool seek(size_t offset, SeekMode) {
    if (offset > length_) return false;
    cursor_ = offset;
    return true;
  }
  size_t read(uint8_t *target, size_t count) {
    if (!bytes_ || count > length_ - cursor_) return 0;
    std::memcpy(target, bytes_ + cursor_, count);
    cursor_ += count;
    return count;
  }
  void close() { bytes_ = nullptr; }
 private:
  const uint8_t *bytes_;
  size_t length_, cursor_ = 0;
};

// @LEGACY_PREFIX@
// @LEGACY_AGENTS@
// @LEGACY_EFFECTS@
// @LEGACY_IP_FONT@

uint32_t updateSkySample(uint16_t target, bool frozen) {
  const uint32_t now = millis();
  const uint32_t delta = min(uint32_t(250), uint32_t(now - skySampleAt));
  skySampleAt = now;
  if (status.memorialTransition) return quotaFrameMilli;
  const uint32_t wanted = uint32_t(target) * 1000;
  if (skySampleReady && !sceneTargetOn && sceneProgress < SCENE_DURATION_MS)
    return quotaFrameMilli;
  if (!skySampleReady || sceneProgress < SCENE_DURATION_MS) {
    skySampleReady = true; quotaFrameMilli = wanted;
  } else {
    if (quotaRecovery.pending() && !frozen) quotaRecovery.begin(quotaFrameMilli, wanted);
    if (quotaRecovery.active()) {
      quotaFrameMilli = quotaRecovery.advance(delta, wanted);
      return quotaFrameMilli;
    }
    if (!frozen) {
      const int32_t distance = (int32_t(wanted) - int32_t(quotaFrameMilli) + 300000) % 200000 - 100000;
      const int32_t step = min(uint32_t(std::abs(distance)), delta * 4);
      const int32_t phase = int32_t(quotaFrameMilli) + (distance < 0 ? -step : step);
      quotaFrameMilli = uint32_t((phase + 200000) % 200000);
    }
  }
  return quotaFrameMilli;
}

int16_t smoothSkyWave(uint16_t period, uint16_t phase) {
  const uint64_t time = (uint64_t(effectTick) + phase) * 256 + effectSubTick;
  const uint32_t position = (time % (uint32_t(period) * 256)) * 256 / period;
  const uint8_t index = position >> 8, fraction = position & 255;
  return (int32_t(sineAt(index)) * (256 - fraction) + int32_t(sineAt(uint8_t(index + 1))) * fraction) / 256;
}

void modulateSky(uint8_t *rgb, uint8_t type, uint8_t x, uint8_t y) {
  int scale = 255;
  if (type == 1) {
    const uint16_t seed = x * 37 + y * 101 + 17;
    const uint8_t periods[] = {51, 63, 81, 99};
    const uint8_t period = periods[seed % 4];
    scale = 180 + 75 * smoothSkyWave(period, (seed * 7) % period) / 127;
  } else if (type == 3) scale = 218 + 37 * smoothSkyWave(210, x * 5 + y * 3) / 127;
  else if (type >= 64) scale = 220 + 35 * smoothSkyWave(144, type - 64) / 127;
  for (unsigned channel = 0; channel < 3; ++channel) rgb[channel] = (uint16_t(rgb[channel]) * scale + 127) / 255;
}

subpixel::Pixel meteorLayer[NUM_LEDS];
uint16_t meteorTouched[128];
uint16_t meteorTouchedCount = 0;

void stopMemorialMeteors() {
  // Discard flights and pending bursts; nothing observed during silence is queued.
  for (auto &meteor : meteors) meteor = {};
  for (auto &agent : directAgents) agent.schedule = {};
  legacyMeteorSchedule = {};
  meteorPairPrimed = false;
  meteorMaintenancePending = false;
  for (unsigned index = 0; index < meteorTouchedCount; ++index)
    meteorLayer[meteorTouched[index]] = {};
  meteorTouchedCount = 0;
}

uint8_t calculateMemorialBrightnessGain() {
  if (!status.assetsReady) return 255;
  const unsigned referenceFrame = min(unsigned(FRAME_COUNT - 1), unsigned(100 - min(control.remainingPercent, uint8_t(100))));
  const uint8_t *reference = sceneVisible && sceneProgress == SCENE_DURATION_MS
      ? entrancePixels : assets.animation + referenceFrame * FRAME_BYTES;
  const auto colors = memorial::palette(4000, 10000);
  uint32_t referenceLight = 0, memorialLight = 0;
  for (int y = 0; y < MATRIX_HEIGHT; ++y) {
    for (int x = 0; x < MATRIX_WIDTH; ++x) {
      const unsigned index = y * MATRIX_WIDTH + x;
      uint8_t rgb[3];
      std::memcpy(rgb, reference + index * 3, 3);
      const uint8_t type = reference == entrancePixels ? effectMask[index] :
          assets.effects[referenceFrame * EFFECT_BYTES + index];
      preserveNightSkyPixel(rgb, index, type, control.brightness);
      referenceLight += memorial::luminance({rgb[0], rgb[1], rgb[2]});
      memorialLight += memorial::luminance(memorial::pixel(x, y, MATRIX_WIDTH, MATRIX_HEIGHT, colors));
    }
  }
  // Match the base scene, excluding meteor overlays and the user's global
  // brightness. Capture once so twinkling or new telemetry cannot pump the gain.
  return memorial::matchingGain(referenceLight, memorialLight);
}

void smoothMeteorPixel(float x, float y, const uint8_t *normal, const uint8_t *dark,
                       uint8_t opacity = 100, bool wake = false) {
  const auto sample = subpixel::footprint(x, y);
  for (unsigned tap = 0; tap < 4; ++tap) {
    const int16_t px = sample.x + (tap & 1), py = sample.y + (tap >> 1);
    if (px < 0 || px >= MATRIX_WIDTH || py < 0 || py >= MATRIX_HEIGHT) continue;
    const uint16_t coverage = (sample.weight[tap] * opacity + 50) / 100;
    if (!coverage || (wake && isLightObject(px, py))) continue;
    const uint16_t index = py * MATRIX_WIDTH + px;
    auto &pixel = meteorLayer[index];
    if (!pixel.alpha) {
      if (meteorTouchedCount == 128) continue;
      meteorTouched[meteorTouchedCount++] = index;
    }
    const uint8_t *rgb = isLightObject(px, py) ? dark : normal;
    pixel.add(rgb[0], rgb[1], rgb[2], coverage);
  }
}

void composeMeteorLayer(bool additive = false) {
  uint8_t *pixels = strip.getPixels();
  for (unsigned touched = 0; touched < meteorTouchedCount; ++touched) {
    const uint16_t index = meteorTouched[touched];
    const auto &sample = meteorLayer[index];
    if (pixels) {
      uint8_t *rgb = pixels + physicalIndex(index % MATRIX_WIDTH, index / MATRIX_WIDTH) * 3;
      rgb[0] = subpixel::composite(rgb[0], sample.green, sample.alpha, strip.getBrightness(), additive);
      rgb[1] = subpixel::composite(rgb[1], sample.red, sample.alpha, strip.getBrightness(), additive);
      rgb[2] = subpixel::composite(rgb[2], sample.blue, sample.alpha, strip.getBrightness(), additive);
    }
    meteorLayer[index] = {};
  }
  meteorTouchedCount = 0;
}

float smoothMeteorAge(const MeteorState &meteor) {
  return uint32_t(effectTick - meteor.spawnFrame) + effectSubTick / 256.0f;
}

void drawSmoothMeteorWake(const MeteorState &meteor) {
  const float age = smoothMeteorAge(meteor);
  const unsigned tick = uint32_t(effectTick - meteor.spawnFrame);
  const unsigned tail = 1 + ((meteor.tailLength - 1) & ~1);
  const uint16_t total = MATRIX_WIDTH + meteor.tailLength + 3;
  uint8_t normal[3], dark[3];
  meteorGlowPalette(meteor.rgb, 100, 0, normal, dark);
  for (unsigned offset = 0; offset < 18 && offset <= tick; ++offset) {
    const unsigned born = tick - offset;
    if (born < tail + 2 || born > MATRIX_WIDTH + tail + 1 || (born + meteor.sequence * 3) % 4) continue;
    const float elapsed = age - born;
    const float remaining = max(0.0f, 18.0f - elapsed);
    const uint8_t opacity = (remaining * remaining * 8 + 162) / 324;
    const int step = int(born) - tail - 2;
    const uint32_t seed = eventHash(meteor.sequence * 257UL + born * 17UL + 9000UL);
    const float drift = elapsed < 9 ? 0 : (elapsed - 9) / 9 * ((seed & 1) ? 1 : -1);
    smoothMeteorPixel(MATRIX_WIDTH - step, subpixel::trajectory(step, total, meteor.sequence, meteor.attributed) + drift,
                      normal, dark, opacity, true);
  }
  composeMeteorLayer(true);
}

void drawSmoothMeteor(const MeteorState &meteor) {
  const float age = smoothMeteorAge(meteor);
  const unsigned tick = uint32_t(effectTick - meteor.spawnFrame);
  const uint16_t total = MATRIX_WIDTH + meteor.tailLength + 3;
  const int span = max(1, (meteor.tailLength - 1) & ~1);
  for (int tail = meteor.tailLength - 1; tail >= -1; --tail) {
    if (tail >= 0 && !meteorTailPixelVisible(tail, meteor.tailLength)) continue;
    const bool tip = tail == span;
    const uint8_t brightness = tail < 0 ? 100 : meteorTailBrightness(tail, meteor.tailLength, tick, meteor.sequence);
    if (!brightness) continue;
    uint8_t normal[3], dark[3];
    meteorGlowPalette(meteor.rgb, tip ? 18 : brightness, tail < 0 ? 100 : (span - tail) * 70 / span, normal, dark);
    const float step = age - (1 + tail);
    smoothMeteorPixel(MATRIX_WIDTH - step, subpixel::trajectory(step, total, meteor.sequence, meteor.attributed),
                      normal, dark, tip ? (brightness * 100 + 9) / 18 : 100);
  }
  const unsigned cadence = meteor.level == LOAD_LOW ? 7 : meteor.level == LOAD_MEDIUM ? 6 : meteor.level == LOAD_HIGH ? 5 : 4;
  unsigned drawn = 0;
  for (unsigned offset = 0; offset < 12 && offset <= tick && drawn < (meteor.level == LOAD_LOW ? 2U : 3U); ++offset) {
    const unsigned born = tick - offset;
    if (born < meteor.tailLength + 2U || born > MATRIX_WIDTH + meteor.tailLength + 1U || (born + meteor.sequence * 3) % cadence) continue;
    const uint32_t seed = eventHash(meteor.sequence * 257UL + born * 17UL);
    const unsigned lifetime = 9 + seed % 4;
    const float elapsed = age - born;
    if (elapsed >= lifetime) continue;
    const float step = int(born) - (1 + meteorSparkTailIndex(meteor.tailLength, seed));
    const float x = MATRIX_WIDTH - step - elapsed * (((seed >> 5) % 2) + 1) / 3;
    const float y = subpixel::trajectory(step, total, meteor.sequence, meteor.attributed)
        + (((seed >> 7) & 1) ? 1 : -1) * (1 + elapsed / 4);
    const float distance = x - (MATRIX_WIDTH - age);
    if (distance >= 1 && distance < meteor.tailLength + 1 &&
        !meteorTailPixelVisible(int(distance) - 1, meteor.tailLength) &&
        std::fabs(y - subpixel::trajectory(MATRIX_WIDTH - x, total, meteor.sequence, meteor.attributed)) < .75f) continue;
    const float remaining = lifetime - elapsed;
    uint8_t normal[3], dark[3]; meteorGlowPalette(meteor.rgb, 100, 30, normal, dark);
    smoothMeteorPixel(x, y, normal, dark, (remaining * remaining * 72 + lifetime * lifetime / 2) / (lifetime * lifetime));
    ++drawn;
  }
  composeMeteorLayer();
}

bool matrixRequestsLight() { return control.on && lightOpeningReady; }
bool otaCheckPending() { return control.maintenance != Maintenance::None; }
void drawEventEffects() {
  // The legacy network maintenance interval is now an explicit IPC request.
  lastMeteorMaintenanceAt = millis();
  meteorMaintenancePending = false;
  updateMeteorScheduler(!status.memorialTransition && sceneProgress == SCENE_DURATION_MS && matrixRequestsLight() && !otaCheckPending());
  for (const auto &meteor : meteors) if (meteor.active) drawSmoothMeteorWake(meteor);
  for (const auto &meteor : meteors) if (meteor.active) drawSmoothMeteor(meteor);
}

void applyControlAgents() {
  // Match by stable identity before assigning new slots; queue order may change.
  bool retained[DIRECT_AGENT_CAPACITY] = {};
  for (size_t slot = 0; slot < DIRECT_AGENT_CAPACITY; ++slot) {
    for (const auto &input : control.agents) {
      if (input.present && directAgents[slot].occupied &&
          input.clientId == directAgents[slot].clientId) retained[slot] = true;
    }
    if (!retained[slot]) directAgents[slot] = {};
  }
  for (const auto &input : control.agents) {
    if (!input.present) continue;
    size_t slot = DIRECT_AGENT_CAPACITY;
    for (size_t candidate = 0; candidate < DIRECT_AGENT_CAPACITY; ++candidate) {
      if (directAgents[candidate].occupied && directAgents[candidate].clientId == input.clientId) {
        slot = candidate;
        break;
      }
    }
    if (slot == DIRECT_AGENT_CAPACITY) {
      for (size_t candidate = 0; candidate < DIRECT_AGENT_CAPACITY; ++candidate) {
        if (!directAgents[candidate].occupied) { slot = candidate; break; }
      }
    }
    if (slot == DIRECT_AGENT_CAPACITY) continue;
    auto &agent = directAgents[slot];
    agent.occupied = true;
    agent.clientId = input.clientId;
    agent.rgb = input.rgb;
    agent.reportedRgb = input.rgb;
    agent.activity = input.activity;
    agent.hasRemaining = input.remaining >= 0 && input.remaining <= 100;
    agent.remaining = agent.hasRemaining ? input.remaining : 100;
    agent.lastSeenAt = input.seenAt;
    std::memcpy(agent.name, input.name, sizeof(agent.name));
    agent.name[sizeof(agent.name) - 1] = 0;
  }
  remainingPercent = min(control.remainingPercent, uint8_t(100));
  activityPercent = min(control.activityPercent, uint8_t(100));
}

void drawIpLine(const char *text, int16_t y) {
  const size_t length = std::strlen(text);
  const int16_t width = length ? length * 6 - 1 : 0;
  const int16_t origin = max(0, (MATRIX_WIDTH - width) / 2);
  for (size_t character = 0; character < length; ++character) {
    const uint8_t glyph = ipGlyphIndex(text[character]);
    const uint32_t color = text[character] == '.'
        ? strip.Color(255, 118, 24) : strip.Color(54, 214, 255);
    for (uint8_t column = 0; column < 5; ++column) {
      for (uint8_t row = 0; row < 7; ++row) {
        const int16_t x = origin + character * 6 + column;
        const int16_t pixelY = y + row;
        if ((IP_FONT[glyph][column] & (1 << row)) && x >= 0 && x < MATRIX_WIDTH &&
            pixelY >= 0 && pixelY < MATRIX_HEIGHT) strip.setPixelColor(physicalIndex(x, pixelY), color);
      }
    }
  }
}

void drawIpAddress() {
  char first[16], second[16] = {};
  std::memcpy(first, control.ip, sizeof(first));
  first[sizeof(first) - 1] = 0;
  char *dot = std::strchr(first, '.');
  dot = dot ? std::strchr(dot + 1, '.') : nullptr;
  if (dot) {
    std::strcpy(second, dot + 1);
    *dot = 0;
  }
  strip.clear();
  drawIpLine(first, second[0] ? 2 : 7);
  if (second[0]) drawIpLine(second, 11);
}

void drawAirAlertIndicator(bool alternateScreen) {
  const auto overlay = airAlertVisual.overlay();
  if (!air_alert::indicatorVisible(overlay.amount, control.on,
          control.maintenance != Maintenance::None, control.brightness)) return;
  uint8_t *pixels = strip.getPixels();
  if (!pixels) return;
  for (unsigned y = 0; y < air_alert::kIconHeight && y < MATRIX_HEIGHT; ++y)
    for (unsigned x = 0; x < MATRIX_WIDTH; ++x) {
      if (!air_alert::cornerPixel(x, y, MATRIX_WIDTH, MATRIX_HEIGHT)) continue;
      const unsigned index = y * MATRIX_WIDTH + x;
      const bool fixed = pgm_read_byte(&ENTRANCE_FIXED[index]) != 0;
      // Service screens retain compact corners rather than covering their text.
      if (alternateScreen && !fixed) continue;
      const unsigned paneWidth = min(air_alert::kIconWidth, unsigned(MATRIX_WIDTH / 2));
      const auto level = airAlertVisual.glyphLevel();
      unsigned localX = x < MATRIX_WIDTH / 2 ? x : x - (MATRIX_WIDTH - paneWidth);
      if (x >= MATRIX_WIDTH / 2 && (level == 1 || level == 2)) localX = paneWidth - 1 - localX;
      const bool ink = alternateScreen || air_alert::iconPixel(level, localX, y);
      if (!ink && !air_alert::iconBackdrop(level, localX, y)) continue;
      // Preserve bright objects around the silhouette, but never punch holes in it.
      if (!ink && !fixed && effectMask[index]) continue;
      uint8_t *grb = pixels + physicalIndex(x, y) * 3;
      grb[0] = air_alert::composite(grb[0], ink ? overlay.green : 0, overlay.amount, control.brightness);
      grb[1] = air_alert::composite(grb[1], ink ? overlay.red : 0, overlay.amount, control.brightness);
      grb[2] = air_alert::composite(grb[2], ink ? overlay.blue : 0, overlay.amount, control.brightness);
    }
}

bool renderScene(FrameReader &animationFile, FrameReader &effectMaskFile,
                 uint16_t &frame, bool finishingMeteorFlight) {
#if EVENT_MODE
// @LEGACY_SCENE@
  return true;
}

void publishStatus() {
  status.sceneVisible = sceneVisible;
  status.sceneProgress = sceneProgress;
  status.activeMeteors = 0;
  for (const auto &meteor : meteors) if (meteor.active) ++status.activeMeteors;
  status.stackFreeBytes = uxTaskGetStackHighWaterMark(nullptr);
  xQueueOverwrite(renderStatusQueue, &status);
}

bool showFrame(bool dark = false) {
  const bool completed = strip.show();
  status.ledError = strip.lastError();
  // Only an explicit, successfully transmitted clear is a closing ACK. Dark
  // artwork, zero brightness, or a failed transfer must not end the sequence.
  matrixDark = completed && dark;
  if (completed) lightPresentationActive = !dark &&
      control.maintenance != Maintenance::Ota && control.maintenance != Maintenance::Restart;
  if (completed) {
    ++status.frames;
    if (control.maintenance == Maintenance::None)
      capturePreview(strip.getPixels(), strip.getBrightness(), status.frames, millis(), physicalIndex);
  }
  return completed;
}

ota_visual::Fill<ota_visual::perimeter(MATRIX_WIDTH, MATRIX_HEIGHT)> otaFill;
uint32_t otaFillEpoch = 0;
void renderOtaProgress() {
  if (control.maintenance != Maintenance::Ota || !control.otaVisualSequence ||
      (status.otaVisualSequence == control.otaVisualSequence && !control.otaVisualAnimate)) return;
  // Continuous motion is granted only during network reads. Core 0 revokes it
  // and waits for an exact one-shot ACK before any subsequent flash operation.
  const uint32_t now = millis();
  if (otaFillEpoch != control.maintenanceEpoch) {
    otaFill.reset(now);
    otaFillEpoch = control.maintenanceEpoch;
  }
  otaFill.update(control.otaVisualPercent, now);
  strip.setBrightness(control.brightness);
  strip.clear();
  if (control.on) {
    for (unsigned y = 0; y < MATRIX_HEIGHT; ++y)
      for (unsigned x = 0; x < MATRIX_WIDTH; ++x) {
        const int edge = ota_visual::position(x, y, MATRIX_WIDTH, MATRIX_HEIGHT);
        const auto color = ota_visual::pixel(x, y, MATRIX_WIDTH, MATRIX_HEIGHT,
            control.otaVisualPercent, now, edge < 0 ? 0 : otaFill.coverage(unsigned(edge)));
        strip.setPixelColor(physicalIndex(x, y), strip.Color(color.red, color.green, color.blue));
      }
  }
  if (showFrame() && strip.isQuiescent()) status.otaVisualSequence = control.otaVisualSequence;
}

void feedWatchdog() {
  if (status.watchdogReady) esp_task_wdt_reset();
}
}  // namespace

void renderTask(void *) {
  // Core 0 completes boot NVS/Wi-Fi initialization before publishing this frame.
  xQueueReceive(controlQueue, &control, portMAX_DELAY);
  const auto watchdogResult = esp_task_wdt_add(nullptr);
  status.watchdogReady = watchdogResult == ESP_OK;
  if (!status.watchdogReady) ESP_LOGE("renderer", "Task watchdog subscription failed: %d", int(watchdogResult));
  applyControlAgents();
  FrameReader animationFile(assets.animation, assets.animationSize);
  FrameReader effectMaskFile(assets.effects, assets.effectsSize);
  status.assetsReady = animationFile && effectMaskFile &&
      animationFile.size() == FRAME_BYTES * FRAME_COUNT &&
      effectMaskFile.size() == EFFECT_BYTES * FRAME_COUNT;
  bool outputReady = status.watchdogReady && strip.begin();
  status.ledError = strip.lastError();
  ESP_LOGI("renderer", "LED init: GPIO=%d core=%d ready=%d error=%d watchdog=%d assets=%d on=%d",
           PROJECT_DATA_PIN, xPortGetCoreID(), outputReady, int(status.ledError),
           status.watchdogReady, status.assetsReady, control.on);
  if (outputReady) {
    strip.setBrightness(255);
    strip.clear();
    if (showFrame(true) && control.maintenance == Maintenance::None) {
      strip.setPixelColor(0, strip.Color(0, 0, 192));
      if (showFrame()) {
        vTaskDelay(pdMS_TO_TICKS(1200));
        feedWatchdog();
      }
    }
    strip.clear();
    showFrame(true);
    strip.setBrightness(control.brightness);
  }
  feedWatchdog();
  uint16_t frame = 0;
  uint32_t holdStarted = 0;
  bool ipHold = false, ipPresented = false;
  char displayedIp[16] = {};
  bool previousConnected = false;
  uint32_t acknowledgedEpoch = 0;
  bool parked = false;
  bool memorialWasActive = false;
  bool memorialSceneVisible = false, memorialSceneTargetOn = false;
  uint32_t memorialSceneProgress = 0, memorialQuota = 0;
  uint32_t memorialStart = 0, memorialCloseMs = 0, memorialSlideMs = 0;
  bool memorialExiting = false;
  uint32_t memorialPosition = 0, memorialForwardFrom = 0;
  uint32_t memorialExitAt = 0, memorialExitFrom = 0;
  uint32_t memorialParkedAt = 0;
  bool memorialForceOff = false;
  uint8_t memorialPaletteGain = 255;
  uint32_t previousRenderAt = millis();
  int64_t effectOrigin = esp_timer_get_time();
  int64_t deadline = effectOrigin;
  for (;;) {
    feedWatchdog();
    const int64_t started = esp_timer_get_time();
    const bool fresh = xQueueReceive(controlQueue, &control, 0) == pdTRUE;
    const bool lightForcedOff = control.lightForcedOff ||
        !outputReady || status.ledError != 0 ||
        control.maintenance == Maintenance::Ota || control.maintenance == Maintenance::Restart;
    const bool lightFollows = control.lightFollowMatrix && external_light::ready() &&
        (control.lightFirst || control.lightSecond);
    if (lightForcedOff) lightPresentationActive = false;
    lightSequence.update(millis(), lightFollows, control.on, matrixDark,
                         lightForcedOff, control.maintenance == Maintenance::Persist);
    const bool lightApplied = external_light::applyScaled(control.lightFirst,
        control.lightSecond, lightSequence.gain());
    lightOpeningReady = !lightApplied || !lightFollows ||
        lightSequence.readyToOpen() || lightPresentationActive;
    status.lightAllowed = lightApplied && !lightForcedOff && lightSequence.gain() != 0;
    status.lightFirstOutput = lightApplied
        ? external_light::outputPercent(control.lightFirst, lightSequence.gain()) : 0;
    status.lightSecondOutput = lightApplied
        ? external_light::outputPercent(control.lightSecond, lightSequence.gain()) : 0;
    // A false status with this epoch proves that Core 1 consumed the release.
    if (control.maintenance == Maintenance::None) status.maintenanceEpoch = control.maintenanceEpoch;
    if (control.maintenance == Maintenance::None && parked) {
      parked = false;
      if (memorialExiting) memorialExitAt += uint32_t(millis() - memorialParkedAt);
      sceneTimelineAt = skySampleAt = millis();
      effectOrigin = started - (int64_t(effectTick) * 256 + effectSubTick) * 1000000 / (EVENT_FPS * 256);
    }
    if (parked) {
      quotaRecovery.observe(control.quotaRecoveryEpoch, true, false);
      if (outputReady) renderOtaProgress();
      // Keep the original maintenance ACK alive. No PSRAM or artwork reads;
      // Core 0 revokes animation and waits for the stop-frame ACK before flash.
      status.quiescent = strip.isQuiescent();
      status.maintenanceEpoch = acknowledgedEpoch;
    } else if (outputReady) {
      if (fresh) applyControlAgents();
      const uint32_t now = millis();
      airAlertVisual.update(control.airAlertLevel, now);
      const bool memorialActive = control.on && lightOpeningReady && control.memorialActive &&
          memorial::active(now, control.memorialStartedAt, control.memorialDurationMs);
      const bool previousMemorialTransition = status.memorialTransition;
      const bool memorialOff = !control.on && previousMemorialTransition;
      if (memorialOff) memorialForceOff = true;
      if (control.on) memorialForceOff = false;
      const bool newMemorial = memorialActive &&
          (!memorialWasActive || memorialStart != control.memorialStartedAt);
      if (memorialWasActive && !memorialActive && control.on) {
        memorialExiting = true;
        memorialExitAt = now;
        memorialExitFrom = memorialPosition;
      }
      if (newMemorial) {
        stopMemorialMeteors();
        memorialStart = control.memorialStartedAt;
        if (!previousMemorialTransition) {
          memorialSceneVisible = sceneVisible;
          memorialSceneTargetOn = sceneTargetOn;
          memorialSceneProgress = sceneProgress;
          memorialQuota = quotaFrameMilli;
          memorialPaletteGain = calculateMemorialBrightnessGain();
          // Reserve at least half the remaining minute for the settled scene.
          memorialSlideMs = min(uint32_t(900), control.memorialDurationMs / 4);
          const uint32_t closeBudget = control.memorialDurationMs / 2 - memorialSlideMs;
          memorialCloseMs = sceneVisible && status.assetsReady
              ? min(sceneProgress, closeBudget) : 0;
          memorialPosition = 0;
        }
        // A play request during the return reverses direction at the last
        // displayed position rather than replacing it with a different frame.
        memorialForwardFrom = memorialPosition;
        memorialExiting = false;
      }
      if (memorialOff) memorialExiting = false;
      status.memorialTransition = memorialActive || memorialExiting;
      const bool memorialTransition = status.memorialTransition || previousMemorialTransition;
      quotaRecovery.observe(control.quotaRecoveryEpoch,
          control.on && control.connected && control.accountLinked && sceneVisible &&
          sceneTargetOn && sceneProgress >= SCENE_DURATION_MS && !memorialTransition,
          control.maintenance == Maintenance::None);
      if (memorialTransition) {
        // Rebase through the exit frame too: neither scene nor effects catch up.
        sceneTimelineAt = skySampleAt = now;
        effectOrigin = started - (int64_t(effectTick) * 256 + effectSubTick) * 1000000 / (EVENT_FPS * 256);
        if (ipHold && ipPresented) holdStarted += uint32_t(now - previousRenderAt);
        lastMeteorMaintenanceAt = now;
      }
      previousRenderAt = now;
      const uint32_t previousTick = effectTick;
      if (!memorialTransition) {
        effectTick = uint64_t(started - effectOrigin) * EVENT_FPS / 1000000;
        effectSubTick = (uint64_t(started - effectOrigin) * EVENT_FPS % 1000000) * 256 / 1000000;
      }
      effectFrameTicks = effectTick - previousTick;
      const bool maintenance = control.maintenance != Maintenance::None;
      const bool flying = meteorFlightInProgress();
      const bool maintenanceReady = maintenance && !flying && !sceneEntranceActive();
      if (maintenanceReady || (maintenance && !flying && memorialTransition)) {
        // Persist freezes the last completed frame. OTA and restart ACK only
        // after a synchronous black transfer has completed successfully.
        bool complete = strip.isQuiescent();
        if (control.maintenance == Maintenance::Ota || control.maintenance == Maintenance::Restart) {
          strip.clear();
          complete = showFrame(true) && strip.isQuiescent();
          if (complete) { sceneVisible = false; sceneProgress = 0; }
        }
        if (complete) {
          acknowledgedEpoch = control.maintenanceEpoch;
          status.otaVisualSequence = 0;
          memorialParkedAt = now;
          parked = true;
          status.quiescent = true;
          status.maintenanceEpoch = acknowledgedEpoch;
        }
      } else if (status.memorialTransition && !maintenance) {
        strip.setBrightness(control.brightness);
        const uint32_t elapsed = uint32_t(now - memorialStart);
        const uint32_t total = memorialCloseMs + memorialSlideMs;
        if (memorialExiting) {
          const uint32_t reversed = min(memorialExitFrom, uint32_t(now - memorialExitAt));
          memorialPosition = memorialExitFrom - reversed;
        } else {
          memorialPosition = uint32_t(min(uint64_t(total), uint64_t(memorialForwardFrom) + elapsed));
        }
        if ((memorialPosition < memorialCloseMs || (memorialExiting && memorialPosition == 0)) &&
            memorialSceneVisible && status.assetsReady) {
          sceneVisible = true;
          sceneTargetOn = memorialExiting;
          sceneProgress = memorialCloseMs ? memorialSceneProgress -
              uint64_t(memorialSceneProgress) * memorialPosition / memorialCloseMs : memorialSceneProgress;
          const bool complete = renderScene(animationFile, effectMaskFile, frame, false);
          quotaFrameMilli = memorialQuota;
          if (!complete) {
            status.assetsReady = false;
            sceneVisible = false;
            memorialCloseMs = memorialPosition;
            strip.clear();
          }
        } else {
          sceneProgress = 0;
          sceneVisible = false;
          const uint32_t slide = memorialPosition >= memorialCloseMs ? memorialPosition - memorialCloseMs : 0;
          const float offset = memorial::slideOffset(slide, memorialSlideMs, MATRIX_HEIGHT);
          // Movement now owns disappearance. Keep the palette fully lit, with
          // its original flame phase, instead of fading to black before exit.
          const uint8_t paletteGain = memorial::readableGain(memorialPaletteGain, control.brightness);
          memorial::drawSliding(MATRIX_WIDTH, MATRIX_HEIGHT, 4000 + elapsed % 4000,
                               10000, offset, [paletteGain](int x, int y, memorial::Pixel color) {
            color = memorial::scalePixel(color, paletteGain);
            strip.setPixelColor(physicalIndex(x, y), strip.Color(color.red, color.green, color.blue));
          });
        }
        drawAirAlertIndicator(true);
        const bool shown = showFrame();
        if (memorialExiting && memorialPosition == 0 && shown) {
          memorialExiting = false;
          sceneVisible = memorialSceneVisible && status.assetsReady;
          sceneTargetOn = memorialSceneTargetOn;
          sceneProgress = memorialSceneProgress;
          quotaFrameMilli = memorialQuota;
        }
      } else if (memorialForceOff && !maintenance) {
        sceneVisible = false;
        sceneTargetOn = false;
        sceneProgress = 0;
        strip.clear();
        if (showFrame(true)) memorialForceOff = false;
      } else if (!status.assetsReady) {
        strip.clear();
        showFrame(true);
      } else {
        strip.setBrightness(control.brightness);
        if (control.connected && (!previousConnected || std::strncmp(displayedIp, control.ip, sizeof(displayedIp)) != 0)) {
          ipHold = true;
          ipPresented = false;
        }
        previousConnected = control.connected;
        if (!control.connected) { ipHold = false; ipPresented = false; }
        if (ipHold && ipPresented && millis() - holdStarted >= 5000UL) ipHold = false;
        const bool requestedOn = control.on && lightOpeningReady;
        if (sceneVisible) advanceSceneTimeline(maintenance ? sceneTargetOn
            : requestedOn || (flying && sceneProgress == SCENE_DURATION_MS));
        if (!requestedOn && (!sceneVisible || sceneProgress == 0)) {
          sceneVisible = false;
          strip.clear();
          showFrame(true);
        } else if (!flying && requestedOn && (!control.connected || !control.accountLinked || ipHold)) {
          sceneVisible = false;
          drawIpAddress();
          drawAirAlertIndicator(true);
          if (showFrame()) {
            std::memcpy(displayedIp, control.ip, sizeof(displayedIp));
            if (control.connected && ipHold && !ipPresented) {
              holdStarted = millis();
              ipPresented = true;
            }
          }
        } else {
          if (!sceneVisible) {
            sceneVisible = true;
            sceneTargetOn = true;
            sceneProgress = 0;
            sceneTimelineAt = millis();
          }
          if (renderScene(animationFile, effectMaskFile, frame, flying)) {
            drawAirAlertIndicator(false);
            showFrame();
          }
          else {
            status.assetsReady = false;
            sceneVisible = false;
            sceneProgress = 0;
            strip.clear();
            showFrame(true);
          }
        }
      }
      memorialWasActive = memorialActive;
    } else if (control.maintenance != Maintenance::None && strip.isQuiescent()) {
      // Failed initialization can ACK only when the adapter confirms no DMA.
      parked = true;
      acknowledgedEpoch = control.maintenanceEpoch;
      status.otaVisualSequence = 0;
      status.maintenanceEpoch = acknowledgedEpoch;
      status.quiescent = true;
    }
    if (!parked) status.quiescent = false;
    status.lastFrameUs = esp_timer_get_time() - started;
    publishStatus();
    feedWatchdog();
    // Integer deadlines carry the remainder, giving exactly 30 deadlines/sec.
    static uint32_t fraction = 0;
    deadline += 1000000 / PROJECT_FPS;
    fraction += 1000000 % PROJECT_FPS;
    if (fraction >= PROJECT_FPS) { ++deadline; fraction -= PROJECT_FPS; }
    const int64_t now = esp_timer_get_time();
    if (now >= deadline) {
      ++status.missedDeadlines;
      deadline = now;
      vTaskDelay(1);
    } else {
      const uint32_t waitMs = (deadline - now + 999) / 1000;
      vTaskDelay(max(TickType_t(1), pdMS_TO_TICKS(waitMs)));
    }
  }
}
