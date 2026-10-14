// Export template: prepare_esp32s3.dart inserts the authoritative legacy scene.
#include "runtime.hpp"
#include "led_output.hpp"
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
bool sceneVisible = false;
bool sceneTargetOn = false;
uint32_t sceneTimelineAt = 0;
uint32_t sceneProgress = 0;
uint8_t entrancePixels[NUM_LEDS * 3];
uint8_t entranceMask[NUM_LEDS];
const uint8_t ENTRANCE_FIXED[NUM_LEDS] PROGMEM = {1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
const uint8_t PYRAMID_MASK[NUM_LEDS] PROGMEM = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
bool entranceMirror = false;
const int32_t ENTRANCE_PHASE[139] PROGMEM = {
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,88,349,781,1383,2152,3086,4183,5441,6857,8430,10157,12036,14065,16242,18565,21032,23640,26387,29271,32291,35443,38726,42138,45676,49339,53123,57028,61051,65189,69441,73804,78277,82857,87542,92330,97219,102206,107289,112467,117738,123098,128546,134080,139697,145396,151175,157030,162961,168964,175038,181181,187391,193664,200000,205611,209701,212327,213630,213803,213069,211664,209816,207734,205597,203552,201709,200142,198891,197971,197371,197061,197001,197142,197434,197828,198276,198740,199186,199591,199938,200216,200423,200560,200633,200651,200625,200564,200480,200384,200283,200186,200097,200020,199959,199912,199881,199864,199859,199864,199876,199894,199915,199936,199958,199977,199994,200008,200018,200025,200029,200031,200029,200024,200017,200011,200006,200003,200001,200000,200000
};

bool sceneEntranceActive() {
  return sceneVisible && (sceneTargetOn ? sceneProgress < 4600UL : sceneProgress > 0);
}

void advanceSceneTimeline(bool targetOn) {
  const uint32_t now = millis();
  const uint32_t delta = now - sceneTimelineAt;
  sceneTimelineAt = now;
  if (targetOn != sceneTargetOn) {
    sceneTargetOn = targetOn;
    return;
  }
  if (targetOn) {
    const uint32_t left = 4600UL - sceneProgress;
    sceneProgress += delta < left ? delta : left;
  } else {
    sceneProgress -= delta < sceneProgress ? delta : sceneProgress;
  }
}

void drawSceneVisibility() {
  if (sceneProgress >= 1600UL) return;
  const float rise = min(1.0f, sceneProgress / 600.0f);
  const float offset = 3 * (1 - rise) * (1 - rise) * (1 - rise);
  const float fade = constrain(((int32_t)sceneProgress - 600) / 1000.0f, 0.0f, 1.0f);
  const float sky = fade * fade * (3 - 2 * fade);
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
  if (!entranceMirror) return;
  memcpy(entranceMask, effectMask, sizeof(effectMask));
  for (uint16_t y = 0; y < MATRIX_HEIGHT; y++) {
    for (uint16_t x = 0; x < MATRIX_WIDTH; x++) {
      const uint16_t index = y * MATRIX_WIDTH + x;
      if (pgm_read_byte(&ENTRANCE_FIXED[index])) continue;
      const uint16_t reflected = y * MATRIX_WIDTH + MATRIX_WIDTH - 1 - x;
      if (pgm_read_byte(&ENTRANCE_FIXED[reflected])) continue;
      const uint8_t *rgb = &entrancePixels[reflected * 3];
      strip.setPixelColor(physicalIndex(x, y), strip.Color(rgb[0], rgb[1], rgb[2]));
      effectMask[index] = entranceMask[reflected];
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



bool matrixRequestsLight() { return control.on; }
bool otaCheckPending() { return control.maintenance != Maintenance::None; }
void drawEventEffects() {
  // The legacy network maintenance interval is now an explicit IPC request.
  lastMeteorMaintenanceAt = millis();
  meteorMaintenancePending = false;
  updateMeteorScheduler(sceneProgress == 4600UL && matrixRequestsLight() && !otaCheckPending());
  for (const auto &meteor : meteors) if (meteor.active) drawMeteorWake(meteor);
  for (const auto &meteor : meteors) if (meteor.active) drawMeteor(meteor);
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

bool renderScene(FrameReader &animationFile, FrameReader &effectMaskFile,
                 uint16_t &frame, bool finishingMeteorFlight) {
#if EVENT_MODE
  entranceMirror = false;
  if (!finishingMeteorFlight || sceneEntranceActive()) frame = 100 - remainingPercent;
  const uint32_t entranceElapsed = sceneProgress;
  if (entranceElapsed < 4600UL) {
    const uint32_t phaseTime = entranceElapsed * 30;
    const uint16_t key = phaseTime / 1000;
    const int32_t startPhase = (int32_t)pgm_read_dword(&ENTRANCE_PHASE[key]);
    const int32_t endPhase = (int32_t)pgm_read_dword(&ENTRANCE_PHASE[key + 1]);
    int32_t position = (frame * 1000L + startPhase + (endPhase - startPhase) * (int32_t)(phaseTime % 1000) / 1000) % 200000L;
    entranceMirror = FRAME_COUNT != 201 && position > 100000L;
    if (entranceMirror) position = 200000L - position;
    frame = (position + 500) / 1000;
  }
#endif
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
      if (effectType == 1 || effectType >= 64 || effectType == 3) {
        uint8_t scale = 255;
        if (effectType == 1) {
          const uint16_t seed = x * 37 + y * 101 + 17;
          const uint8_t periods[4] = {51, 63, 81, 99};
          const uint8_t period = periods[seed % 4];
          const uint8_t phaseFrame = (seed * 7) % period;
          const uint8_t lutIndex = (((effectTick + phaseFrame) % period) * 256UL + period / 2) / period;
          scale = 180 + (int16_t)75 * sineAt(lutIndex) / 127;
        } else if (effectType == 3) {
          const uint8_t phase = ((effectTick + x * 5 + y * 3) % 210) * 256UL / 210;
          scale = 218 + (int16_t)37 * sineAt(phase) / 127;
        } else {
          const uint8_t phaseFrame = effectType - 64;
          const uint8_t lutIndex = (((effectTick + phaseFrame) % 144) * 256UL + 72) / 144;
          scale = 220 + (int16_t)35 * sineAt(lutIndex) / 127;
        }
        rgb[0] = ((uint16_t)rgb[0] * scale + 127) / 255;
        rgb[1] = ((uint16_t)rgb[1] * scale + 127) / 255;
        rgb[2] = ((uint16_t)rgb[2] * scale + 127) / 255;
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
  if (completed) ++status.frames;
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
      sceneTimelineAt = millis();
      effectOrigin = started - int64_t(effectTick) * 1000000 / EVENT_FPS;
    }
    if (parked) {
      // Keep the original ACK alive until core 0 releases the request. No PSRAM,
      // scene drawing, LED transfers, or asset reads while flash may be active.
      status.quiescent = true;
      status.maintenanceEpoch = acknowledgedEpoch;
    } else if (outputReady) {
      if (fresh) applyControlAgents();
      const uint32_t previousTick = effectTick;
      effectTick = uint64_t(started - effectOrigin) * EVENT_FPS / 1000000;
      effectFrameTicks = effectTick - previousTick;
      const bool maintenance = control.maintenance != Maintenance::None;
      const bool flying = meteorFlightInProgress();
      if (maintenance && !flying && !sceneEntranceActive()) {
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
          parked = true;
          status.quiescent = true;
          status.maintenanceEpoch = acknowledgedEpoch;
        }
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
            : requestedOn || (flying && sceneProgress == 4600UL));
        if (!requestedOn && (!sceneVisible || sceneProgress == 0)) {
          sceneVisible = false;
          strip.clear();
          showFrame();
        } else if (!flying && requestedOn && (!control.connected || !control.accountLinked || ipHold)) {
          sceneVisible = false;
          drawIpAddress();
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
          if (renderScene(animationFile, effectMaskFile, frame, flying)) showFrame();
          else {
            status.assetsReady = false;
            sceneVisible = false;
            sceneProgress = 0;
            strip.clear();
            showFrame();
          }
        }
      }
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
