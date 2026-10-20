// Export template: prepare_esp32s3.dart inserts the authoritative legacy scene.
#include "runtime.hpp"
#include "led_output.hpp"
#include "subpixel.hpp"
#include "preview.hpp"
#include "memorial_draw.hpp"
#include "air_alert_visual.hpp"
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
air_alert::Visual airAlertVisual;

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

uint8_t remainingPercent = 100;
uint8_t activityPercent = 0;
uint32_t effectTick = 0;
uint32_t effectFrameTicks = 1;
uint8_t effectMask[NUM_LEDS];
uint16_t physicalIndex(uint8_t x, uint8_t y);
#if EVENT_MODE
constexpr uint32_t SCENE_DURATION_MS = 4000UL;
bool sceneVisible = false;
bool sceneTargetOn = false;
uint32_t sceneTimelineAt = 0;
uint32_t sceneProgress = 0;
uint8_t entrancePixels[NUM_LEDS * 3];
uint8_t entranceMask[NUM_LEDS];
const uint8_t ENTRANCE_FIXED[NUM_LEDS] PROGMEM = {1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
const uint8_t PYRAMID_MASK[NUM_LEDS] PROGMEM = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
bool entranceMirror = false;
float entrancePhasePosition = 0;
float entranceSmooth(float value) {
  const float t = constrain(value, 0.0f, 1.0f);
  return t * t * (3 - 2 * t);
}
float entranceStrength() {
  return entranceSmooth((float(SCENE_DURATION_MS) - sceneProgress) / 1000.0f);
}
struct EntranceOrigin {
  float rayX, rayY, complete, reach;
};
EntranceOrigin entranceOrigin(float value) {
  const float progress = constrain(value, 0.0f, 1.0f);
  const float ray = PI * (1 - progress);
  return {cosf(ray), sinf(ray), entranceSmooth((progress - .8f) / .2f), progress * 100};
}
float entranceOriginVisibility(const EntranceOrigin &origin, float dx, float dy) {
  const float cross = origin.rayX * max(0.0f, dy) - origin.rayY * dx;
  const float angular = entranceSmooth((cross - .25f) / 1.5f);
  const float radial = entranceSmooth((origin.reach * origin.reach - dx * dx - dy * dy) / max(2.0f, 4 * origin.reach));
  return (angular + (1 - angular) * origin.complete) * radial;
}
// Keep dark blue visible at the usual 20% setting without cancelling dimming.
void preserveNightSkyPixel(uint8_t *rgb, uint16_t index, uint8_t effectType, uint8_t brightness) {
  if (!brightness || effectType || pgm_read_byte(&ENTRANCE_FIXED[index]) ||
      rgb[2] < 2 || rgb[2] > 4 || rgb[1] >= rgb[2] || rgb[0] > rgb[1]) return;
  const uint8_t minimumBlue = 5;
  if (rgb[2] < minimumBlue) rgb[2] = uint8_t(minimumBlue);
}
float entranceClosingFrom = 0, entranceClosingTo = 0;
uint32_t entranceClosingAt = 0;
bool entranceClosingTarget = false;
float entranceClosingAmount() {
  const uint32_t now = millis();
  const bool target = !sceneTargetOn;
  // Both trajectories share their endpoints: no crossfade is needed there.
  if (sceneProgress == 0 || sceneProgress >= SCENE_DURATION_MS) {
    entranceClosingTarget = target;
    entranceClosingFrom = entranceClosingTo = target ? 1 : 0;
    entranceClosingAt = now;
    return entranceClosingTo;
  }
  const float amount = entranceSmooth((now - entranceClosingAt) / 160.0f);
  const float current = entranceClosingFrom + (entranceClosingTo - entranceClosingFrom) * amount;
  if (target != entranceClosingTarget) {
    entranceClosingTarget = target;
    entranceClosingFrom = current;
    entranceClosingTo = target ? 1 : 0;
    entranceClosingAt = now;
  }
  return current;
}
const int32_t ENTRANCE_ANCHOR = 65000;
const int32_t ENTRANCE_PHASE[121] PROGMEM = {
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,66,262,587,1037,1611,2306,3119,4049,5094,6250,7516,8889,10367,11948,13628,15407,17282,19250,21309,23457,25691,28009,30409,32889,35446,38077,40781,43556,46398,49306,52277,55309,58399,61546,64747,68000,71302,74651,78045,81481,84958,88472,92022,95605,99219,102861,106530,110222,113936,117670,121420,125185,128963,132750,136545,140346,144149,147954,151757,155556,159348,163133,166906,170667,174412,178139,181846,185531,189191,192824,196428,200000,203418,206223,208178,209316,209832,209984,210000,209893,208999,206978,204078,200898,198133,196332,195647,195613,196019,197192,198876,200499,201522,201798,201718,201167,200297,199648,199500,199605,199895,200000
};
const int32_t EXIT_PHASE[121] PROGMEM = {
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,49,193,432,764,1188,1703,2306,2996,3772,4632,5576,6601,7705,8889,10149,11486,12896,14379,15933,17557,19250,21009,22834,24723,26674,28687,30759,32889,35076,37318,39614,41962,44361,46809,49306,51848,54436,57068,59741,62455,65209,68000,70827,73690,76585,79513,82471,85458,88472,91513,94578,97666,100776,103907,107056,110222,113405,116601,119811,123032,126263,129503,132750,136003,139259,142519,145780,149040,152300,155556,158807,162052,165290,168520,171738,174945,178139,181318,184480,187626,190751,193857,196940,200000,202952,205475,207388,208679,209442,209821,209965,209998,210000,209885,209236,207901,206033,203967,202099,200764,200115,200000
};

int32_t entranceMotionPhase(uint32_t elapsed) {
  const float direction = entranceClosingAmount();
  if (elapsed >= SCENE_DURATION_MS) return 200000L;
  const uint32_t phaseTime = elapsed * 30;
  const uint16_t key = phaseTime / 1000;
  const int32_t fraction = phaseTime % 1000;
  const int32_t a = (int32_t)pgm_read_dword(&ENTRANCE_PHASE[key]);
  const int32_t b = (int32_t)pgm_read_dword(&ENTRANCE_PHASE[key + 1]);
  const int32_t c = (int32_t)pgm_read_dword(&EXIT_PHASE[key]);
  const int32_t d = (int32_t)pgm_read_dword(&EXIT_PHASE[key + 1]);
  const int32_t opening = a + (b - a) * fraction / 1000;
  const int32_t closing = c + (d - c) * fraction / 1000;
  // Share the retargetable direction blend with the sky mask on interruption.
  return int32_t(opening + (closing - opening) * direction + .5f);
}

bool sceneEntranceActive() {
  return sceneVisible && (sceneTargetOn ? sceneProgress < SCENE_DURATION_MS : sceneProgress > 0);
}

void advanceSceneTimeline(bool targetOn) {
  const uint32_t now = millis();
  const uint32_t delta = now - sceneTimelineAt;
  sceneTimelineAt = now;
  if (targetOn != sceneTargetOn) {
    sceneTargetOn = targetOn;
    entranceClosingAmount();
    return;
  }
  entranceClosingAmount();
  if (targetOn) {
    const uint32_t left = SCENE_DURATION_MS - sceneProgress;
    sceneProgress += delta < left ? delta : left;
  } else {
    sceneProgress -= delta < sceneProgress ? delta : sceneProgress;
  }
}

void drawSceneVisibility() {
  if (sceneProgress >= 600UL) return;
  const float rise = min(1.0f, sceneProgress / 600.0f);
  const float offset = 3 * (1 - rise) * (1 - rise) * (1 - rise);
  const float sky = 0;
  for (int16_t y = 0; y < MATRIX_HEIGHT; y++) {
    const float sourceY = y - offset;
    const int16_t y0 = floorf(sourceY);
    const float fraction = sourceY - y0;
    for (int16_t x = 0; x < MATRIX_WIDTH; x++) {
      const uint16_t index = y * MATRIX_WIDTH + x;
      const int32_t i0 = y0 * MATRIX_WIDTH + x;
      const int32_t i1 = i0 + MATRIX_WIDTH;
      const float a0 = i0 >= 0 && i0 < NUM_LEDS && pgm_read_byte(&PYRAMID_MASK[i0]) ? 1 - fraction : 0;
      const float a1 = i1 >= 0 && i1 < NUM_LEDS && pgm_read_byte(&PYRAMID_MASK[i1]) ? fraction : 0;
      const uint32_t background = strip.getPixelColor(physicalIndex(x, y));
      const float base = pgm_read_byte(&PYRAMID_MASK[index]) ? 0 : sky * (1 - a0 - a1);
      uint8_t rgb[3];
      for (uint8_t channel = 0; channel < 3; channel++) {
        const uint8_t shift = (2 - channel) * 8;
        float value = ((background >> shift) & 255) * base;
        if (a0 > 0) value += entrancePixels[i0 * 3 + channel] * a0;
        if (a1 > 0) value += entrancePixels[i1 * 3 + channel] * a1;
        rgb[channel] = constrain((int)(value + .5f), 0, 255);
      }
      strip.setPixelColor(physicalIndex(x, y), strip.Color(rgb[0], rgb[1], rgb[2]));
    }
  }
}

void drawSceneEntrance() {
  if (sceneProgress >= SCENE_DURATION_MS) return;
  memcpy(entranceMask, effectMask, sizeof(effectMask));
  const float phase = entrancePhasePosition;
  const float nx = sinf(-PI * phase);
  const float ny = -1.85f * cosf(-PI * phase);
  const float closing = entranceClosingAmount();
  const EntranceOrigin openingOrigin = entranceOrigin((.65f - phase) / .65f);
  const float gate = entranceStrength() * entranceSmooth(phase / .2f);
  for (uint16_t y = 0; y < MATRIX_HEIGHT; y++) {
    const float row = (y * 20.0f / MATRIX_HEIGHT - 18.5f) * ny + 2;
    for (uint16_t x = 0; x < MATRIX_WIDTH; x++) {
      const uint16_t index = y * MATRIX_WIDTH + x;
      if (pgm_read_byte(&ENTRANCE_FIXED[index])) continue;
      const uint16_t reflected = entranceMirror ? y * MATRIX_WIDTH + MATRIX_WIDTH - 1 - x : index;
      if (entranceMirror && pgm_read_byte(&ENTRANCE_FIXED[reflected])) continue;
      const float day = entranceSmooth(((x * 50.0f / MATRIX_WIDTH - 24.5f) * nx + row) / 4);
      float visibility = phase >= 0 ? 1 - gate * (1 - day) : 1;
      const float dx = x * 50.0f / MATRIX_WIDTH - 24.5f;
      const float dy = (18.5f - y * 20.0f / MATRIX_HEIGHT) * 1.85f;
      if (phase >= 0) {
        visibility *= entranceOriginVisibility(openingOrigin, dx, dy);
      }
      // Let the complete night edge rotate below the last matrix row.
      const float closed = phase >= 0 ? 0 : phase <= -1 ? 1 :
          entranceSmooth(-((x * 50.0f / MATRIX_WIDTH - 24.5f) * nx +
              (y * 20.0f / MATRIX_HEIGHT - 20) * ny) /
              (1.5f * min(50.0f / MATRIX_WIDTH, 20.0f / MATRIX_HEIGHT)));
      visibility += (closed - visibility) * closing;
      const uint8_t *rgb = &entrancePixels[reflected * 3];
      uint8_t visibleRgb[3] = {uint8_t(rgb[0] * visibility + .5f),
          uint8_t(rgb[1] * visibility + .5f), uint8_t(rgb[2] * visibility + .5f)};
      preserveNightSkyPixel(visibleRgb, index, entranceMask[reflected], strip.getBrightness());
      strip.setPixelColor(physicalIndex(x, y), strip.Color(visibleRgb[0], visibleRgb[1], visibleRgb[2]));
      effectMask[index] = visibility <= 0 ? 0 : entranceMask[reflected];
    }
  }
}
#endif
const int8_t SINE_LUT[256] PROGMEM = {0, 3, 6, 9, 12, 16, 19, 22, 25, 28, 31, 34, 37, 40, 43, 46, 49, 51, 54, 57, 60, 63, 65, 68, 71, 73, 76, 78, 81, 83, 85, 88, 90, 92, 94, 96, 98, 100, 102, 104, 106, 107, 109, 111, 112, 113, 115, 116, 117, 118, 120, 121, 122, 122, 123, 124, 125, 125, 126, 126, 126, 127, 127, 127, 127, 127, 127, 127, 126, 126, 126, 125, 125, 124, 123, 122, 122, 121, 120, 118, 117, 116, 115, 113, 112, 111, 109, 107, 106, 104, 102, 100, 98, 96, 94, 92, 90, 88, 85, 83, 81, 78, 76, 73, 71, 68, 65, 63, 60, 57, 54, 51, 49, 46, 43, 40, 37, 34, 31, 28, 25, 22, 19, 16, 12, 9, 6, 3, 0, -3, -6, -9, -12, -16, -19, -22, -25, -28, -31, -34, -37, -40, -43, -46, -49, -51, -54, -57, -60, -63, -65, -68, -71, -73, -76, -78, -81, -83, -85, -88, -90, -92, -94, -96, -98, -100, -102, -104, -106, -107, -109, -111, -112, -113, -115, -116, -117, -118, -120, -121, -122, -122, -123, -124, -125, -125, -126, -126, -126, -127, -127, -127, -127, -127, -127, -127, -126, -126, -126, -125, -125, -124, -123, -122, -122, -121, -120, -118, -117, -116, -115, -113, -112, -111, -109, -107, -106, -104, -102, -100, -98, -96, -94, -92, -90, -88, -85, -83, -81, -78, -76, -73, -71, -68, -65, -63, -60, -57, -54, -51, -49, -46, -43, -40, -37, -34, -31, -28, -25, -22, -19, -16, -12, -9, -6, -3};

int8_t sineAt(uint8_t index) {
  return (int8_t)pgm_read_byte(&SINE_LUT[index]);
}

uint16_t physicalIndex(uint8_t x, uint8_t y) {
  x = MATRIX_WIDTH - 1 - x;
  if (y & 1) return y * MATRIX_WIDTH + (MATRIX_WIDTH - 1 - x);
  return y * MATRIX_WIDTH + x;
}

void showError(uint8_t r, uint8_t g, uint8_t b) {
  for (uint16_t i = 0; i < NUM_LEDS; i++) {
    strip.setPixelColor(i, strip.Color(r, g, b));
  }
  strip.show();
}

uint32_t eventHash(uint32_t value) {
  value = ((value >> 16) ^ value) * 0x45D9F3B;
  const uint64_t mixed = (uint64_t)((value >> 16) ^ value) * 0x45D9F3B;
  return ((mixed >> 16) ^ mixed) & 0x7FFFFFFF;
}

struct MeteorSchedule {
  uint32_t nextBurst;
  uint32_t nextFlight;
  uint16_t burstSequence;
  uint16_t candidateFrames;
  uint8_t level;
  uint8_t candidate;
  uint8_t pending;
  uint8_t stagger;
};


struct DirectAgentState {
  bool occupied;
  uint32_t clientId;
  char name[49];
  uint32_t rgb;
  uint32_t reportedRgb;
  MeteorSchedule schedule;
  uint8_t activity;
  uint8_t remaining;
  bool hasRemaining;
  uint32_t lastSeenAt;
};

DirectAgentState directAgents[DIRECT_AGENT_CAPACITY] = {};
bool directAgentMode = false;

enum MeteorLoadLevel : uint8_t { LOAD_IDLE = 0, LOAD_LOW = 1, LOAD_MEDIUM = 2, LOAD_HIGH = 3, LOAD_INTENSE = 4, LOAD_STORM = 5 };

struct MeteorState {
  bool active;
  uint32_t spawnFrame;
  uint16_t sequence;
  uint8_t level;
  uint8_t tailLength;
  uint32_t rgb;
  bool attributed;
  uint32_t clientId;
};

const uint8_t METEOR_CAPACITY = 12;
MeteorState meteors[METEOR_CAPACITY] = {};
MeteorSchedule legacyMeteorSchedule = {};
uint8_t nextMeteorAgent = 0;
uint16_t meteorSequence = 0;
const uint8_t METEOR_PAIR_SPACING_FRAMES = 18;
uint32_t nextMeteorPairAt = 0;
bool meteorPairPrimed = false;
bool meteorMaintenancePending = false;
uint32_t lastMeteorMaintenanceAt = 0;

bool meteorFlightInProgress() {
  for (uint8_t index = 0; index < METEOR_CAPACITY; index++) {
    if (meteors[index].active) return true;
  }
  return false;
}

uint8_t rawLoadLevel(uint8_t activity) {
  if (activity == 0) return LOAD_IDLE;
  if (activity <= 20) return LOAD_LOW;
  if (activity <= 40) return LOAD_MEDIUM;
  if (activity <= 60) return LOAD_HIGH;
  if (activity <= 80) return LOAD_INTENSE;
  return LOAD_STORM;
}

uint8_t desiredLoadLevel(uint8_t activity, uint8_t stableLoadLevel) {
  if (activity == 0) return LOAD_IDLE;
  if (stableLoadLevel > 0 && activity >= ((int)stableLoadLevel - 1) * 20 - 3 &&
      activity <= stableLoadLevel * 20 + 3) return stableLoadLevel;
  return rawLoadLevel(activity);
}

void updateStableLoadLevel(MeteorSchedule &schedule, uint8_t activity) {
  if (activity == 0) {
    schedule = {};
    return;
  }
  const uint8_t desired = desiredLoadLevel(activity, schedule.level);
  if (desired == schedule.level) {
    schedule.candidate = schedule.level;
    schedule.candidateFrames = 0;
    return;
  }
  if (desired != schedule.candidate) {
    schedule.candidate = desired;
    schedule.candidateFrames = 1;
  } else if (schedule.candidateFrames < EVENT_FPS * 2) {
#if 1
    const uint32_t frames = (uint32_t)schedule.candidateFrames + effectFrameTicks;
    schedule.candidateFrames = frames > EVENT_FPS * 2 ? EVENT_FPS * 2 : frames;
#else
    schedule.candidateFrames++;
#endif
  }
  if (schedule.candidateFrames >= EVENT_FPS * 2) {
    schedule.level = desired;
    schedule.candidateFrames = 0;
    schedule.pending = 0;
    schedule.nextBurst = effectTick;
  }
}

uint8_t tailLengthForLevel(uint8_t level) {
  return level == LOAD_LOW ? 5 : (level == LOAD_MEDIUM ? 7 : (level == LOAD_HIGH ? 9 : (level == LOAD_INTENSE ? 10 : (level == LOAD_STORM ? 11 : 0))));
}

uint8_t maximumActiveForLevel(uint8_t level) {
  const uint8_t counts[] = {0, 1, 2, 3, 5, 7};
  return counts[min(level, (uint8_t)5)];
}

uint8_t burstStaggerFrames(uint8_t level, uint16_t burstSequence) {
  return 0;
}

uint8_t agentBurstSizeForUsers(uint8_t level, uint8_t activeUsers) {
  const uint8_t base = maximumActiveForLevel(level);
  return activeUsers >= 2 ? min(base, (uint8_t)3) : base;
}

uint8_t activeMeteorLimitForUsers(uint8_t activeUsers) {
  return activeUsers >= 2 ? 8 : METEOR_CAPACITY;
}

uint8_t activeMeteorsForOwner(uint32_t clientId) {
  uint8_t count = 0;
  for (uint8_t index = 0; index < METEOR_CAPACITY; index++) {
    const MeteorState &meteor = meteors[index];
    if (meteor.active && meteor.attributed && meteor.clientId == clientId) count++;
  }
  return count;
}

uint16_t spawnIntervalFrames(uint8_t level, uint16_t sequence) {
  const uint16_t minimumFrames[] = {0, 540, 240, 135, 110, 90};
  const uint16_t maximumFrames[] = {0, 660, 360, 180, 150, 120};
  if (level == LOAD_IDLE) return 0;
  const uint16_t minimum = (minimumFrames[level] * EVENT_FPS + 15UL) / 30;
  const uint16_t maximum = (maximumFrames[level] * EVENT_FPS + 15UL) / 30;
  return minimum + eventHash(sequence + 5000) % (maximum - minimum + 1);
}

int8_t meteorY(int16_t step, uint16_t totalSteps, uint16_t sequence) {
  const int16_t clampedStep = constrain(step, 0, totalSteps);
  if (sequence % 2 == 1) return 2 + ((uint32_t)clampedStep * 13 + totalSteps / 2) / totalSteps;
  const uint8_t ellipseIndex = ((uint32_t)clampedStep * 128 + totalSteps / 2) / totalSteps;
  const uint8_t variant = sequence % 3;
  const uint8_t phaseIndex = variant == 1 ? 85 : (variant == 2 ? 171 : 0);
  const uint8_t waveIndex = (((uint32_t)clampedStep * 512 + totalSteps / 2) / totalSteps + phaseIndex) & 0xFF;
  const int8_t offset = variant == 0 ? -1 : (variant == 1 ? 0 : 1);
  const int16_t scaled = (9 + offset) * 508 - 10 * sineAt(ellipseIndex) + sineAt(waveIndex);
  return (scaled + 254) / 508;
}

int8_t agentMeteorY(int16_t step, uint16_t totalSteps, uint16_t sequence) {
  const int32_t progress = constrain(step, 0, totalSteps);
  const uint8_t variant = (sequence / 6) % 3;
  int32_t offset = 0;
  if (variant == 1) offset = (2 * progress + totalSteps / 2) / totalSteps;
  if (variant == 2) {
    const int32_t denominator = (int32_t)totalSteps * totalSteps;
    offset = -(4 * progress * (totalSteps - progress) + denominator / 2) / denominator;
  }
  return constrain(2 + 2 * (sequence % 6) + offset, 0, MATRIX_HEIGHT - 1);
}

int8_t flightMeteorY(const MeteorState &meteor, int16_t step, uint16_t totalSteps) {
  return meteor.attributed ? agentMeteorY(step, totalSteps, meteor.sequence)
      : meteorY(step, totalSteps, meteor.sequence);
}

bool isLightObject(int16_t x, int16_t y) {
  if (x < 0 || x >= MATRIX_WIDTH || y < 0 || y >= MATRIX_HEIGHT) return false;
  const uint8_t value = effectMask[y * MATRIX_WIDTH + x];
  return value == 2 || value == 3 || value >= 64;
}

void setMeteorPixel(
  int16_t x,
  int16_t y,
  uint8_t red,
  uint8_t green,
  uint8_t blue,
  uint8_t darkRed,
  uint8_t darkGreen,
  uint8_t darkBlue,
  uint8_t opacity = 100
) {
  if (x < 0 || x >= MATRIX_WIDTH || y < 0 || y >= MATRIX_HEIGHT) return;
  const bool light = isLightObject(x, y);
  uint8_t color[3] = {light ? darkRed : red, light ? darkGreen : green, light ? darkBlue : blue};
  if (opacity < 100) {
    const uint32_t background = strip.getPixelColor(physicalIndex(x, y));
    for (uint8_t channel = 0; channel < 3; channel++) {
      const uint8_t base = (background >> ((2 - channel) * 8)) & 255;
      color[channel] = (base * (100 - opacity) + color[channel] * opacity + 50) / 100;
    }
  }
  strip.setPixelColor(
    physicalIndex(x, y),
    strip.Color(color[0], color[1], color[2])
  );
}

void agentMeteorPalette(uint32_t rgb, uint8_t percent, uint8_t *normal, uint8_t *dark) {
  for (uint8_t channel = 0; channel < 3; channel++) {
    normal[channel] = (((rgb >> ((2 - channel) * 8)) & 255) * percent + 50) / 100;
    dark[channel] = (((rgb >> ((2 - channel) * 8)) & 255) * percent + 200) / 400;
  }
}

void meteorGlowPalette(uint32_t rgb, uint8_t percent, uint8_t heat, uint8_t *normal, uint8_t *dark) {
  const uint16_t maximum = max((rgb >> 16) & 255UL, max((rgb >> 8) & 255UL, rgb & 255UL));
  for (uint8_t channel = 0; channel < 3; channel++) {
    const int16_t normalized = maximum
        ? (((rgb >> ((2 - channel) * 8)) & 255) * 255 + maximum / 2) / maximum : 0;
    const uint16_t tinted = maximum
        ? normalized + ((255 - normalized) * heat + 200) / 400 : 0;
    normal[channel] = (tinted * percent + 50) / 100;
    dark[channel] = (tinted * percent + 200) / 400;
  }
}

uint8_t meteorTailBrightness(uint8_t index, uint8_t length, uint16_t age, uint16_t sequence) {
  const int16_t span = max(1, (length - 1) & ~1);
  if (index == span) {
    const int16_t phase = (age + sequence * 7UL) % 36;
    const int16_t pulse = max(0, 18 - abs(phase - 18) - 4);
    return (18 * pulse + 7) / 14;
  }
  const int16_t remaining = constrain(span - index, 0, span);
  const int16_t base = 6 + (remaining * remaining * 66 + span * span / 2) / (span * span);
  const int16_t wave = sineAt((age * 6 - index * 19 + sequence * 31) & 255);
  return (base * (100 + wave * 18 / 127) + 50) / 100;
}

bool meteorTailPixelVisible(uint8_t index, uint8_t length) {
  const int16_t span = max(1, (length - 1) & ~1);
  const int16_t solidCount = max(1, span * 3 / 5);
  return index < length && (index < solidCount || index % 2 == 0);
}

uint8_t meteorSparkTailIndex(uint8_t length, uint32_t seed) {
  const int16_t first = (((length * 3 + 4) / 5) + 1) & ~1;
  const int16_t last = (length - 1) & ~1;
  const uint8_t count = max(1, (last - first) / 2 + 1);
  return min(first, last) + 2 * (seed % count);
}

uint8_t meteorSparkBrightness(uint8_t age, uint8_t lifetime) {
  const int16_t remaining = max(0, lifetime - age);
  return (remaining * remaining * 72 + lifetime * lifetime / 2) / (lifetime * lifetime);
}

int8_t meteorSparkDriftX(uint8_t age, uint32_t seed) {
  return (age * (((seed >> 5) % 2) + 1) + 2) / 3;
}

int8_t meteorSparkDriftY(uint8_t age, uint32_t seed) {
  return (((seed >> 7) & 1) == 0 ? -1 : 1) * (1 + (age + 2) / 4);
}

uint8_t meteorWakeOpacity(uint8_t age) {
  const int16_t remaining = max(0, 18 - age);
  return (remaining * remaining * 8 + 162) / 324;
}

int8_t meteorWakeDriftY(uint8_t age, uint32_t seed) {
  return age < 9 ? 0 : ((seed & 1) == 0 ? -1 : 1);
}

uint8_t meteorWakeChannel(uint8_t background, uint8_t glow, uint8_t opacity) {
  const uint16_t value = background + ((uint16_t)glow * opacity + 50) / 100;
  return value > 255 ? 255 : value;
}

uint8_t meteorWakeStoredChannel(uint8_t background, uint8_t glow, uint8_t opacity, uint8_t brightness) {
  const uint16_t contribution = ((uint16_t)glow * opacity + 50) / 100;
  const uint16_t increment = brightness == 255 ? contribution : (contribution * (brightness + 1)) >> 8;
  const uint16_t ceiling = brightness == 255 ? 255 : brightness;
  if (!increment || background >= ceiling) return background;
  return min(ceiling, (uint16_t)(background + increment));
}

void drawMeteorSparks(const MeteorState &meteor, uint16_t age, uint16_t totalSteps) {
  const uint8_t cadence = meteor.level == LOAD_LOW ? 7 : (meteor.level == LOAD_MEDIUM ? 6 : (meteor.level == LOAD_HIGH ? 5 : 4));
  const uint8_t maximum = meteor.level == LOAD_LOW ? 2 : 3;
  uint8_t drawn = 0;
  for (uint8_t sparkAge = 0; sparkAge < 12 && drawn < maximum; sparkAge++) {
    if (age < sparkAge) continue;
    const uint16_t eventFrame = age - sparkAge;
    if (eventFrame < meteor.tailLength + 2 || eventFrame > MATRIX_WIDTH + meteor.tailLength + 1) continue;
    if ((eventFrame + meteor.sequence * 3) % cadence != 0) continue;
    const uint32_t seed = eventHash(meteor.sequence * 257UL + eventFrame * 17UL);
    const uint8_t lifetime = 9 + seed % 4;
    if (sparkAge >= lifetime) continue;
    const uint8_t tailIndex = meteorSparkTailIndex(meteor.tailLength, seed);
    const int16_t step = (int16_t)eventFrame - (1 + tailIndex);
    const int16_t x = MATRIX_WIDTH - step - meteorSparkDriftX(sparkAge, seed);
    const int16_t y = flightMeteorY(meteor, step, totalSteps) + meteorSparkDriftY(sparkAge, seed);
    const int16_t distance = x - (MATRIX_WIDTH - (int16_t)age);
    const bool gap = distance >= 1 && distance < meteor.tailLength + 1 &&
        !meteorTailPixelVisible(distance - 1, meteor.tailLength);
    if (gap && y == flightMeteorY(meteor, MATRIX_WIDTH - x, totalSteps)) continue;
    uint8_t normal[3], dark[3];
    meteorGlowPalette(meteor.rgb, 100, 30, normal, dark);
    setMeteorPixel(x, y, normal[0], normal[1], normal[2], dark[0], dark[1], dark[2],
        meteorSparkBrightness(sparkAge, lifetime));
    drawn++;
  }
}

void drawMeteorWake(const MeteorState &meteor) {
  uint8_t *pixels = strip.getPixels();
  if (!pixels) return;
  const uint32_t age = effectTick - meteor.spawnFrame;
  const uint8_t tailDistance = 1 + ((meteor.tailLength - 1) & ~1);
  const uint16_t totalSteps = MATRIX_WIDTH + meteor.tailLength + 3;
  uint8_t glow[3], dark[3];
  meteorGlowPalette(meteor.rgb, 100, 0, glow, dark);
  for (uint8_t wakeAge = 0; wakeAge < 18; wakeAge++) {
    if (age < wakeAge) continue;
    const uint32_t born = age - wakeAge;
    if (born < tailDistance + 2UL || born > MATRIX_WIDTH + tailDistance + 1UL ||
        (born + meteor.sequence * 3) % 4 != 0) continue;
    const uint8_t opacity = meteorWakeOpacity(wakeAge);
    if (!opacity) continue;
    const int16_t step = born - tailDistance - 2;
    const uint32_t seed = eventHash(meteor.sequence * 257UL + born * 17UL + 9000UL);
    const int16_t x = MATRIX_WIDTH - step;
    const int16_t y = flightMeteorY(meteor, step, totalSteps) + meteorWakeDriftY(wakeAge, seed);
    if (x < 0 || x >= MATRIX_WIDTH || y < 0 || y >= MATRIX_HEIGHT || isLightObject(x, y)) continue;
    const uint32_t index = physicalIndex(x, y);
    // The strip is NEO_GRB. Preserve stored bytes to avoid lossy brightness round trips.
    uint8_t *pixel = pixels + index * 3;
    const uint8_t brightness = strip.getBrightness();
    pixel[0] = meteorWakeStoredChannel(pixel[0], glow[1], opacity, brightness);
    pixel[1] = meteorWakeStoredChannel(pixel[1], glow[0], opacity, brightness);
    pixel[2] = meteorWakeStoredChannel(pixel[2], glow[2], opacity, brightness);
  }
}

void drawMeteor(const MeteorState &meteor) {
  const uint16_t age = effectTick - meteor.spawnFrame;
  const uint16_t totalSteps = MATRIX_WIDTH + meteor.tailLength + 3;
  const int16_t span = max(1, (meteor.tailLength - 1) & ~1);
  for (int8_t tailIndex = meteor.tailLength - 1; tailIndex >= 0; tailIndex--) {
    if (!meteorTailPixelVisible(tailIndex, meteor.tailLength)) continue;
    const int16_t step = (int16_t)age - (1 + tailIndex);
    const uint8_t brightness = meteorTailBrightness(tailIndex, meteor.tailLength, age, meteor.sequence);
    if (brightness == 0) continue;
    const bool isTip = tailIndex == span;
    uint8_t normal[3];
    uint8_t dark[3];
    meteorGlowPalette(meteor.rgb, isTip ? 18 : brightness,
        (span - tailIndex) * 70 / span, normal, dark);
    setMeteorPixel(
      MATRIX_WIDTH - step,
      flightMeteorY(meteor, step, totalSteps),
      normal[0], normal[1], normal[2], dark[0], dark[1], dark[2],
      isTip ? (brightness * 100 + 9) / 18 : 100
    );
  }
  const int16_t headX = MATRIX_WIDTH - age;
  const int16_t headY = flightMeteorY(meteor, age, totalSteps);
  uint8_t normal[3], dark[3];
  meteorGlowPalette(meteor.rgb, 100, 100, normal, dark);
  setMeteorPixel(headX, headY, normal[0], normal[1], normal[2], dark[0], dark[1], dark[2]);
  drawMeteorSparks(meteor, age, totalSteps);
}

void prepareMeteorBurst(MeteorSchedule &schedule, uint8_t activeUsers) {
  if (schedule.level == LOAD_IDLE || schedule.pending > 0 ||
      (int32_t)(effectTick - schedule.nextBurst) < 0) return;
  schedule.pending = agentBurstSizeForUsers(schedule.level, activeUsers);
  schedule.stagger = burstStaggerFrames(schedule.level, schedule.burstSequence);
  schedule.nextFlight = effectTick;
  schedule.nextBurst = effectTick + spawnIntervalFrames(schedule.level, schedule.burstSequence++);
}

void updateMeteorScheduler(bool allowSpawns = true) {
  if (millis() - lastMeteorMaintenanceAt >= 10000UL) meteorMaintenancePending = true;
  allowSpawns = allowSpawns && !meteorMaintenancePending;
  uint8_t activeCount = 0;
  for (uint8_t index = 0; index < METEOR_CAPACITY; index++) {
    if (!meteors[index].active) continue;
    const uint32_t age = effectTick - meteors[index].spawnFrame;
    if (age > MATRIX_WIDTH + meteors[index].tailLength + 22UL) {
      meteors[index].active = false;
    } else {
      activeCount++;
    }
  }
  if (activeCount == 0) meteorPairPrimed = false;
#if EVENT_MODE
  bool direct = false;
  uint8_t activeUsers = 0;
  for (uint8_t index = 0; index < DIRECT_AGENT_CAPACITY; index++) {
    DirectAgentState &agent = directAgents[index];
    if (agent.occupied && millis() - agent.lastSeenAt >= DIRECT_AGENT_TIMEOUT_MS) agent.occupied = false;
    direct |= agent.occupied;
    updateStableLoadLevel(agent.schedule, agent.occupied && allowSpawns ? agent.activity : 0);
    if (agent.occupied && agent.schedule.level != LOAD_IDLE) activeUsers++;
  }
  updateStableLoadLevel(legacyMeteorSchedule, !direct && allowSpawns ? activityPercent : 0);
  for (uint8_t index = 0; index < DIRECT_AGENT_CAPACITY; index++) {
    prepareMeteorBurst(directAgents[index].schedule, activeUsers);
  }
  prepareMeteorBurst(legacyMeteorSchedule, activeUsers);
  if (!allowSpawns) return;
  if (meteorPairPrimed && (int32_t)(effectTick - nextMeteorPairAt) < 0) return;
  const uint8_t activeLimit = activeMeteorLimitForUsers(activeUsers);
  uint8_t launched = 0;
  // Allocate one flight per ready source per round. The cursor survives a full pool.
  const uint8_t sourceCount = DIRECT_AGENT_CAPACITY + 1;
  for (uint8_t slot = 0; slot < METEOR_CAPACITY && activeCount < activeLimit && launched < 2; slot++) {
    if (meteors[slot].active) continue;
    for (uint8_t attempt = 0; attempt < sourceCount; attempt++) {
      const uint8_t source = nextMeteorAgent;
      nextMeteorAgent = (nextMeteorAgent + 1) % sourceCount;
      MeteorSchedule &schedule = source == DIRECT_AGENT_CAPACITY
          ? legacyMeteorSchedule : directAgents[source].schedule;
      if (schedule.pending == 0 || (int32_t)(effectTick - schedule.nextFlight) < 0) continue;
      if (activeUsers >= 2 && source != DIRECT_AGENT_CAPACITY &&
          activeMeteorsForOwner(directAgents[source].clientId) >= 3) continue;
      MeteorState &meteor = meteors[slot];
      meteor.active = true;
      meteor.spawnFrame = effectTick;
      meteor.sequence = meteorSequence++;
      meteor.level = schedule.level;
      meteor.tailLength = tailLengthForLevel(schedule.level);
      meteor.rgb = source == DIRECT_AGENT_CAPACITY ? 0xFFD052UL : directAgents[source].rgb;
      meteor.attributed = source != DIRECT_AGENT_CAPACITY;
      meteor.clientId = meteor.attributed ? directAgents[source].clientId : 0;
      schedule.pending--;
      schedule.nextFlight = effectTick + schedule.stagger;
      activeCount++;
      launched++;
      nextMeteorPairAt = effectTick + METEOR_PAIR_SPACING_FRAMES;
      meteorPairPrimed = true;
      break;
    }
  }
#endif
}


const uint8_t IP_FONT[11][5] PROGMEM = {
  {0x3E, 0x51, 0x49, 0x45, 0x3E},
  {0x00, 0x42, 0x7F, 0x40, 0x00},
  {0x42, 0x61, 0x51, 0x49, 0x46},
  {0x21, 0x41, 0x45, 0x4B, 0x31},
  {0x18, 0x14, 0x12, 0x7F, 0x10},
  {0x27, 0x45, 0x45, 0x45, 0x39},
  {0x3C, 0x4A, 0x49, 0x49, 0x30},
  {0x01, 0x71, 0x09, 0x05, 0x03},
  {0x36, 0x49, 0x49, 0x49, 0x36},
  {0x06, 0x49, 0x49, 0x29, 0x1E},
  {0x00, 0x00, 0x60, 0x60, 0x00},
};

uint8_t ipGlyphIndex(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  return 10;
}



uint32_t updateSkySample(uint16_t target, bool frozen) {
  const uint32_t now = millis();
  const uint32_t delta = min(uint32_t(250), uint32_t(now - skySampleAt));
  skySampleAt = now;
  if (status.memorialTransition) return quotaFrameMilli;
  const uint32_t wanted = uint32_t(target) * 1000;
  if (!skySampleReady || sceneProgress < SCENE_DURATION_MS) {
    skySampleReady = true; quotaFrameMilli = wanted;
  } else if (!frozen) {
    const uint32_t distance = quotaFrameMilli > wanted ? quotaFrameMilli - wanted : wanted - quotaFrameMilli;
    const uint32_t step = min(distance, delta * 4);
    quotaFrameMilli = quotaFrameMilli > wanted ? quotaFrameMilli - step : quotaFrameMilli + step;
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

bool matrixRequestsLight() { return control.on; }
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
  for (size_t pixel = 0; pixel < NUM_LEDS; ++pixel) strip.setPixelColor(pixel, strip.Color(1, 4, 14));
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
  entranceMirror = false;
  if (!finishingMeteorFlight || sceneEntranceActive()) frame = 100 - remainingPercent;
  sceneFrameMilli = updateSkySample(frame, finishingMeteorFlight);
  frame = sceneFrameMilli / 1000;
  const uint32_t entranceElapsed = sceneProgress;
  const int32_t phase = entranceMotionPhase(entranceElapsed);
  if (entranceElapsed < SCENE_DURATION_MS) {
    const int64_t unwrapped = ENTRANCE_ANCHOR + int64_t(int32_t(sceneFrameMilli) - 200000L - ENTRANCE_ANCHOR) * phase / 200000L;
    entrancePhasePosition = unwrapped / 100000.0f;
    int32_t position = (unwrapped % 200000L + 200000L) % 200000L;
    entranceMirror = FRAME_COUNT != 201 && position > 100000L;
    if (entranceMirror) position = 200000L - position;
    sceneFrameMilli = position;
    frame = sceneFrameMilli / 1000;
  }
#endif
  const uint16_t sceneFraction = sceneFrameMilli % 1000;
  const uint16_t nextFrame = frame + 1 < FRAME_COUNT ? frame + 1 : frame;
  const uint32_t nextOffset = uint32_t(nextFrame) * FRAME_BYTES;
  const uint32_t nextMaskOffset = uint32_t(nextFrame) * EFFECT_BYTES;
  const uint32_t frameOffset = (uint32_t)frame * FRAME_BYTES;
  animationFile.seek(frameOffset, SeekSet);
#if EVENT_MODE
  const uint32_t effectOffset = (uint32_t)frame * EFFECT_BYTES;
  effectMaskFile.seek(effectOffset, SeekSet);
#endif
  uint8_t frameRow[MATRIX_WIDTH * 3];
#if EVENT_MODE
  uint8_t effectRow[MATRIX_WIDTH];
#endif
  for (uint8_t y = 0; y < MATRIX_HEIGHT; y++) {
    if (animationFile.read(frameRow, sizeof(frameRow)) != sizeof(frameRow)) {
      animationFile.close();
      return false;
    }
#if EVENT_MODE
    if (effectMaskFile.read(effectRow, sizeof(effectRow)) != sizeof(effectRow)) {
      effectMaskFile.close();
      return false;
    }
#endif
    for (uint8_t x = 0; x < MATRIX_WIDTH; x++) {
      uint8_t rgb[3] = {frameRow[x * 3], frameRow[x * 3 + 1], frameRow[x * 3 + 2]};
#if EVENT_MODE
      const uint8_t effectType = effectRow[x];
      const uint16_t pixelIndex = y * MATRIX_WIDTH + x;
      effectMask[pixelIndex] = effectType;
      modulateSky(rgb, effectType, x, y);
      if (sceneFraction) {
        const uint8_t nextType = assets.effects[nextMaskOffset + pixelIndex];
        uint8_t nextRgb[3];
        memcpy(nextRgb, assets.animation + nextOffset + pixelIndex * 3, 3);
        modulateSky(nextRgb, nextType, x, y);
        for (unsigned channel = 0; channel < 3; ++channel)
          rgb[channel] = subpixel::mix(rgb[channel], nextRgb[channel], sceneFraction);
        effectMask[pixelIndex] = subpixel::mask(effectType, nextType, sceneFraction);
      }
#endif
      #if EVENT_MODE
      if (pgm_read_byte(&PYRAMID_MASK[pixelIndex])) {
        const uint16_t glintPhase = (effectTick + x * 4) % 150;
        if (glintPhase < 48) {
          const float amount = sinf(glintPhase * PI / 48.0f) * .55f;
          const uint8_t highlight[3] = {255, 231, 172};
          for (uint8_t channel = 0; channel < 3; channel++) {
            rgb[channel] = rgb[channel] + (highlight[channel] - rgb[channel]) * amount;
          }
        }
      }
      const uint16_t entranceIndex = (y * MATRIX_WIDTH + x) * 3;
      memcpy(&entrancePixels[entranceIndex], rgb, 3);
      preserveNightSkyPixel(rgb, pixelIndex, effectMask[pixelIndex], strip.getBrightness());
      #endif
      strip.setPixelColor(
        physicalIndex(x, y),
        strip.Color(rgb[0], rgb[1], rgb[2])
      );
    }
  }
  #if EVENT_MODE
  drawSceneEntrance();
  #endif
  drawEventEffects();
  #if EVENT_MODE
  drawSceneVisibility();
  #endif

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

bool showFrame() {
  const bool completed = strip.show();
  status.ledError = strip.lastError();
  if (completed) {
    ++status.frames;
    if (control.maintenance == Maintenance::None)
      capturePreview(strip.getPixels(), strip.getBrightness(), status.frames, millis(), physicalIndex);
  }
  return completed;
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
    if (showFrame() && control.maintenance == Maintenance::None) {
      strip.setPixelColor(0, strip.Color(0, 0, 192));
      if (showFrame()) {
        vTaskDelay(pdMS_TO_TICKS(1200));
        feedWatchdog();
      }
    }
    strip.clear();
    showFrame();
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
    // A false status with this epoch proves that Core 1 consumed the release.
    if (control.maintenance == Maintenance::None) status.maintenanceEpoch = control.maintenanceEpoch;
    if (control.maintenance == Maintenance::None && parked) {
      parked = false;
      if (memorialExiting) memorialExitAt += uint32_t(millis() - memorialParkedAt);
      sceneTimelineAt = millis();
      effectOrigin = started - (int64_t(effectTick) * 256 + effectSubTick) * 1000000 / (EVENT_FPS * 256);
    }
    if (parked) {
      // Keep the original ACK alive until core 0 releases the request. No PSRAM,
      // scene drawing, LED transfers, or asset reads while flash may be active.
      status.quiescent = true;
      status.maintenanceEpoch = acknowledgedEpoch;
    } else if (outputReady) {
      if (fresh) applyControlAgents();
      const uint32_t now = millis();
      airAlertVisual.update(control.airAlertLevel, now);
      const bool memorialActive = control.on && control.memorialActive &&
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
          complete = showFrame() && strip.isQuiescent();
          if (complete) { sceneVisible = false; sceneProgress = 0; }
        }
        if (complete) {
          acknowledgedEpoch = control.maintenanceEpoch;
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
        if (showFrame()) memorialForceOff = false;
      } else if (!status.assetsReady) {
        strip.clear();
        showFrame();
      } else {
        strip.setBrightness(control.brightness);
        if (control.connected && (!previousConnected || std::strncmp(displayedIp, control.ip, sizeof(displayedIp)) != 0)) {
          ipHold = true;
          ipPresented = false;
        }
        previousConnected = control.connected;
        if (!control.connected) { ipHold = false; ipPresented = false; }
        if (ipHold && ipPresented && millis() - holdStarted >= 5000UL) ipHold = false;
        const bool requestedOn = control.on;
        if (sceneVisible) advanceSceneTimeline(maintenance ? sceneTargetOn
            : requestedOn || (flying && sceneProgress == SCENE_DURATION_MS));
        if (!requestedOn && (!sceneVisible || sceneProgress == 0)) {
          sceneVisible = false;
          strip.clear();
          showFrame();
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
            showFrame();
          }
        }
      }
      memorialWasActive = memorialActive;
    } else if (control.maintenance != Maintenance::None && strip.isQuiescent()) {
      // Failed initialization can ACK only when the adapter confirms no DMA.
      parked = true;
      acknowledgedEpoch = control.maintenanceEpoch;
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
