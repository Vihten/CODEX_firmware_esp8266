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
constexpr unsigned kLightStreams = 4;
constexpr unsigned kInfallStars = 12;
static_assert(kInfallStars % kLightStreams == 0, "Streams need equal particle counts");
constexpr unsigned kTrailSamples = 4;
constexpr uint32_t kInfallPeriodMs = 32768;
constexpr uint32_t kInfallPhaseStepMs = kInfallPeriodMs / (kInfallStars / kLightStreams);
constexpr uint32_t kTrailSpacingMs = 400;
constexpr float kCaptureProgress = 0.91555f;
struct LightSample { float x, y, energy, nx, ny, stretch; };
struct Impact { float x, y, energy; };
struct Frame {
  float orbit_x, orbit_y;
  float warp_x, warp_y, feeding;
  Impact impact[kInfallStars];
  LightSample light[kInfallStars * kTrailSamples];
};

inline Frame frame(uint32_t now) {
  // Both periods divide uint32_t rollover, including the particle respawn.
  constexpr float tau = 6.28318530718f;
  const float phase = float(now & 65535u) * (tau / 65536.0f);
  Frame result{};
  result.orbit_x = std::cos(phase);
  result.orbit_y = std::sin(phase);
  const float warp = float(now & 8191u) * (tau / 8192.0f);
  result.warp_x = std::cos(warp);
  result.warp_y = std::sin(warp);
  for (unsigned i = 0; i < kInfallStars; ++i) {
    for (unsigned j = 0; j < kTrailSamples; ++j) {
      const unsigned stream = i % kLightStreams;
      const unsigned particle = i / kLightStreams;
      const uint32_t tick = now + particle * kInfallPhaseStepMs - j * kTrailSpacingMs;
      const float progress = float(tick & (kInfallPeriodMs - 1)) / float(kInfallPeriodMs);
      // Accelerate toward the horizon; the tail follows the same curved path.
      // All four entry directions fit inside the physical height of the matrix.
      const float radius = 0.46f - 0.34f * progress * progress;
      const float angle = stream * (tau / kLightStreams) + progress * 3.8f +
                          progress * progress * 2.2f;
      const float enter = progress < 0.08f ? progress / 0.08f : 1.0f;
      const float exit = radius < 0.18f ? (radius - 0.14f) / 0.04f : 1.0f;
      const float fade_in = enter * enter * (3 - 2 * enter);
      const float fade_out = exit > 0 ? exit * exit * (3 - 2 * exit) : 0;
      const float visibility = fade_in * fade_out;
      const float cx = std::cos(angle), cy = std::sin(angle);
      // Orient the elongated light along its actual curved velocity. A radial
      // footprint sweeps sideways across LED rows during an orbit.
      const float radial_speed = -0.68f * progress;
      const float angular_speed = 3.8f + 4.4f * progress;
      const float velocity_x = radial_speed * cx - radius * angular_speed * cy;
      const float velocity_y = radial_speed * cy + radius * angular_speed * cx;
      const float inverse_speed = 1.0f / std::sqrt(velocity_x * velocity_x + velocity_y * velocity_y);
      const float stretch = radius < 0.32f ? (0.32f - radius) / 0.20f : 0;
      result.light[i * kTrailSamples + j] = {
          radius * cx, radius * cy,
          visibility * (j == 0 ? 0.58f : 0.27f / float(j)),
          velocity_x * inverse_speed, velocity_y * inverse_speed, 1.0f + 1.8f * stretch};
      if (j == 0) {
        // A local, smooth pulse as this star reaches the luminous inner ring.
        const float event = 1.0f - std::fabs(progress - kCaptureProgress) / 0.04f;
        const float pulse = event > 0 ? event * event * (3 - 2 * event) : 0;
        result.impact[i] = {0.175f * cx, 0.175f * cy, pulse};
        result.feeding += pulse * 0.08f;
      }
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

  // Oblique accretion plane and the lensed rear disk above the black core.
  // Distances use physical LED pitch, not square screen pixels.
  const float tilt = 0.34f + 0.035f * f.warp_y;
  const float plane = py + px * tilt;
  const float disk_y = plane / (0.29f + 0.012f * f.warp_x);
  const float disk_r = std::sqrt(px * px + disk_y * disk_y);
  const float direction_x = px / (disk_r + 0.0001f);
  const float direction_y = disk_y / (disk_r + 0.0001f);
  const float ripple = (direction_x * direction_x - direction_y * direction_y) *
                       f.warp_x + 2 * direction_x * direction_y * f.warp_y;
  const float disk = detail::band(disk_r - (0.34f + 0.012f * ripple), 0.16f);
  const float filament = detail::band(disk_r - (0.32f + 0.014f * ripple), 0.045f);
  const float angular = direction_x * f.orbit_x + direction_y * f.orbit_y;
  const float front_mix = detail::clamp(0.5f + plane / 0.04f);
  const float foreground = 0.55f + 0.45f * front_mix * front_mix * (3 - 2 * front_mix);
  const float heat = (disk * 0.55f + filament * 0.40f) *
                     (0.80f + 0.17f * angular) * foreground;
  const float upper = detail::clamp(0.5f - plane / 0.07f);
  const float rear_arc = detail::band(radius - (0.203f + 0.005f * ripple), 0.047f) * upper;
  const float lens = detail::band(radius - (0.163f + f.feeding * 0.008f), 0.018f) * 0.65f;
  const float arc = rear_arc * (0.75f + 0.12f * angular);
  const float glow = detail::band(disk_r - 0.36f, 0.25f) * 0.07f;
  const uint32_t star_hash = detail::hash(uint32_t(x) +
                                         detail::hash(uint32_t(y) + 917u));
  const float stars = radius > 0.32f && star_hash % 113u == 0u
                         ? (0.05f + float((star_hash >> 16) & 7u) * 0.003f)
                         : 0.0f;
  float captured_light = 0;
  for (const auto& light : f.light) {
    if (light.energy <= 0) continue;
    const float dx = px - light.x, dy = py - light.y;
    // Reject pixels outside the entire rotated ellipse before the expensive
    // projected-distance calculation. The longest semiaxis bounds both axes.
    const float extent = 0.09f * light.stretch;
    if (std::fabs(dx) > extent || std::fabs(dy) > extent) continue;
    // A broad, soft footprint overlaps adjacent rows at the physical LED pitch.
    const float along = (dx * light.nx + dy * light.ny) / light.stretch;
    const float across = -dx * light.ny + dy * light.nx;
    const float weight = detail::clamp(1 - (along * along + across * across) / 0.0081f);
    captured_light += weight * weight * weight * light.energy;
  }
  float response = 0;
  for (const auto& impact : f.impact) {
    if (impact.energy <= 0) continue;
    const float dx = px - impact.x, dy = py - impact.y;
    const float spot = detail::clamp(1 - (dx * dx + dy * dy) / 0.007f);
    response += spot * spot * impact.energy * 0.48f;
  }
  const float edge = detail::clamp((radius - horizon) / 0.012f);
  const float red = heat + lens + arc + glow + stars + captured_light + response;
  const float green = heat * 0.70f + lens * 0.85f + arc * 0.83f + glow * 0.40f +
                      stars + captured_light * 0.87f + response * 0.72f;
  const float blue = heat * 0.32f + lens * 0.57f + arc * 0.55f + glow * 0.08f +
                     stars * 0.85f + captured_light * 0.58f + response * 0.25f;
  // Shared exposure preserves the warm hue where disk, arc and a star overlap.
  const float exposure = edge / (red > 1 ? red : 1);
  return {detail::channel(red * exposure), detail::channel(green * exposure),
          detail::channel(blue * exposure)};
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
