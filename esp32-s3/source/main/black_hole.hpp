#pragma once

#include <cmath>
#include <cstdint>

namespace black_hole {
constexpr uint32_t kPreviewDurationMs = 60000;
inline bool previewActive(bool requested, uint32_t startedAt, uint32_t now) {
  return requested && uint32_t(now - startedAt) < kPreviewDurationMs;
}
struct Pixel { uint8_t red, green, blue; };

// Fixed-size samples: no allocation or per-pixel trigonometry.
constexpr unsigned kInfallStars = 10;
constexpr unsigned kTrailSamples = 4;
struct LightSample { float x, y, energy; };
struct Frame {
  float orbit_x, orbit_y;
  LightSample light[kInfallStars * kTrailSamples];
};

inline Frame frame(uint32_t now) {
  // Both periods divide uint32_t rollover, including the particle respawn.
  constexpr float tau = 6.28318530718f;
  const float phase = float(now & 65535u) * (tau / 65536.0f);
  Frame result{};
  result.orbit_x = std::cos(phase);
  result.orbit_y = std::sin(phase);
  for (unsigned i = 0; i < kInfallStars; ++i) {
    for (unsigned j = 0; j < kTrailSamples; ++j) {
      const uint32_t tick = now + i * 1638u - j * 85u;
      const float progress = float(tick & 16383u) / 16384.0f;
      // Accelerate toward the horizon; the tail follows the same curved path.
      const float radius = 0.76f - 0.64f * progress * progress;
      const float angle = i * 2.39996323f + progress * 3.8f +
                          progress * progress * 2.2f;
      const float enter = progress < 0.08f ? progress / 0.08f : 1.0f;
      const float exit = radius < 0.18f ? (radius - 0.14f) / 0.04f : 1.0f;
      const float visibility = exit > 0 ? enter * exit : 0;
      result.light[i * kTrailSamples + j] = {
          radius * std::cos(angle), radius * std::sin(angle),
          visibility * (j == 0 ? 0.65f : 0.30f / float(j))};
    }
  }
  return result;
}

namespace detail {
inline float clamp(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
inline float band(float distance, float width) {
  const float v = clamp(1 - std::fabs(distance) / width);
  return v * v * (3 - 2 * v);
}
inline uint8_t channel(float v) {
  return static_cast<uint8_t>(clamp(v) * 255 + 0.5f);
}
inline uint32_t hash(uint32_t v) {
  v ^= v >> 16; v *= 0x7feb352du;
  v ^= v >> 15; v *= 0x846ca68bu;
  return v ^ (v >> 16);
}
}  // namespace detail

inline Pixel pixel(unsigned x, unsigned y, unsigned w, unsigned h,
                   const Frame& f) {
  if (!w || !h || x >= w || y >= h) return {0, 0, 0};
  // Normalize in physical space, with 7 mm horizontal and 12 mm vertical pitch.
  const float physical_h = float(h) * (12.0f / 7.0f);
  const float scale = float(w) < physical_h ? float(w) : physical_h;
  const float px = (float(x) + 0.5f - float(w) * 0.5f) / scale;
  const float py = (float(y) + 0.5f - float(h) * 0.5f) * (12.0f / 7.0f) / scale;
  const float radius = std::sqrt(px * px + py * py);
  constexpr float horizon = 0.14f;
  if (radius <= horizon) return {0, 0, 0};

  // A tilted accretion disk, occluded by the circular event horizon above.
  const float disk_y = (py + px * 0.13f) / 0.22f;
  const float disk_r = std::sqrt(px * px + disk_y * disk_y);
  const float disk = detail::band(disk_r - 0.39f, 0.09f);
  // Continuous angular modulation travels through the body of the disk.
  const float angular = (px * f.orbit_x + disk_y * f.orbit_y) /
                        (disk_r + 0.0001f);
  const float heat = disk * (0.68f + 0.27f * angular) *
                     (py > 0 ? 1.0f : 0.75f);
  const float lens = detail::band(radius - 0.175f, 0.028f) *
                     (0.85f + 0.12f * (px * f.orbit_x + py * f.orbit_y) /
                                                (radius + 0.0001f));
  const uint32_t star_hash = detail::hash(uint32_t(x) +
                                         detail::hash(uint32_t(y) + 917u));
  const float stars = radius > 0.32f && star_hash % 113u == 0u
                         ? (0.05f + float((star_hash >> 16) & 7u) * 0.003f)
                         : 0.0f;
  float captured_light = 0;
  for (const auto& light : f.light) {
    const float dx = px - light.x, dy = py - light.y;
    // Compact subpixel footprint prevents blinking between widely spaced LEDs.
    const float weight = detail::clamp(1 - (dx * dx + dy * dy) / 0.0025f);
    captured_light += weight * weight * light.energy;
  }
  const float edge = detail::clamp((radius - horizon) / 0.012f);
  return {detail::channel((heat + lens + stars + captured_light) * edge),
          detail::channel((heat * 0.57f + lens * 0.72f + stars +
                           captured_light * 0.87f) * edge),
          detail::channel((heat * 0.095f + lens * 0.18f + stars * 0.85f +
                           captured_light * 0.58f) * edge)};
}

class State {
 public:
  State(uint32_t recoveryHoldMs = 2000) : recovery_hold_ms_(recoveryHoldMs) {}
  void update(bool known, bool fresh, uint32_t now) {
    if (!initialized_) {
      initialized_ = true;
      last_ = now;
      target_missing_ = !known;
    }
    if (!known) {
      advance(now);
      target_missing_ = true;
      stale_ = recovering_ = false;
      return;
    }
    if (!fresh) {
      recovering_ = false;
      if (!stale_) { stale_ = true; stale_since_ = now; }
      if (!target_missing_ && uint32_t(now - stale_since_) >= 15000u) {
        advance(stale_since_ + 15000u);
        target_missing_ = true;
      }
    } else {
      stale_ = false;
      if (!recovering_) { recovering_ = true; fresh_since_ = now; }
      if (target_missing_ && uint32_t(now - fresh_since_) >= recovery_hold_ms_) {
        advance(fresh_since_ + recovery_hold_ms_);
        target_missing_ = false;
      }
    }
    advance(now);
  }

  // Call on each parked render tick so resumption starts at the latest tick.
  void pause(uint32_t now) {
    last_ = now;
    stale_ = recovering_ = false;
  }

  uint32_t animationTime() const { return animation_time_; }
  uint8_t amount() const { return static_cast<uint8_t>((level_ + 350u) / 700u); }
  bool active() const { return amount() > 0 || target_missing_; }
  bool missing() const { return target_missing_; }

 private:
  void advance(uint32_t now) {
    const uint32_t elapsed = now - last_;
    last_ = now;
    animation_time_ += elapsed;
    // Saturate before multiplication, even after very long update gaps.
    const uint32_t step = (elapsed > 700u ? 700u : elapsed) * 255u;
    if (target_missing_) level_ = step >= 178500u - level_ ? 178500u : level_ + step;
    else level_ = step >= level_ ? 0u : level_ - step;
  }

  uint32_t level_ = 0, last_ = 0, stale_since_ = 0, fresh_since_ = 0;
  uint32_t animation_time_ = 0;
  uint32_t recovery_hold_ms_ = 2000;
  bool initialized_ = false, target_missing_ = true;
  bool stale_ = false, recovering_ = false;
};
}  // namespace black_hole
