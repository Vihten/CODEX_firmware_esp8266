#pragma once

#include <cstdint>

namespace network_progress {

enum class Stage : uint8_t {
  NotStarted, Starting, Wifi, Commands, Weekly, Telemetry, Discovery,
  ManifestResults, AirAlert, Control, ExternalLight, Buzzer, Publish,
  Maintenance, Sleeping, RenderStatus
};

constexpr const char *name(Stage stage) {
  switch (stage) {
    case Stage::NotStarted: return "not_started";
    case Stage::Starting: return "starting";
    case Stage::Wifi: return "wifi";
    case Stage::Commands: return "commands";
    case Stage::Weekly: return "weekly";
    case Stage::Telemetry: return "telemetry";
    case Stage::Discovery: return "discovery";
    case Stage::ManifestResults: return "manifest_results";
    case Stage::AirAlert: return "air_alert";
    case Stage::Control: return "control";
    case Stage::ExternalLight: return "external_light";
    case Stage::Buzzer: return "buzzer";
    case Stage::Publish: return "publish";
    case Stage::Maintenance: return "maintenance";
    case Stage::Sleeping: return "sleeping";
    case Stage::RenderStatus: return "render_status";
  }
  return "unknown";
}

constexpr uint32_t increment(uint32_t value) {
  return value == UINT32_MAX ? value : value + 1;
}

struct Heartbeat {
  Stage stage = Stage::NotStarted;
  uint32_t enteredAtMs = 0;
  uint32_t heartbeat = 0;
  uint32_t completedLoops = 0;
  bool started = false;
};

// Single writer. Repeated entry into the same stage is still progress.
// Synchronize the entire copy for cross-task readers; volatile is insufficient.
class Progress {
 public:
  void enter(Stage stage, uint32_t nowMs) {
    state_.stage = stage;
    state_.enteredAtMs = nowMs;
    ++state_.heartbeat;  // Deliberately wraps: sequence, not lifetime count.
    state_.started = true;
  }

  void completeLoop(uint32_t nowMs) {
    state_.completedLoops = increment(state_.completedLoops);
    enter(Stage::Sleeping, nowMs);
  }

  Heartbeat snapshot() const { return state_; }

 private:
  Heartbeat state_;
};

struct Health {
  Heartbeat progress;
  uint32_t stageAgeMs = 0;
  uint32_t staleEpisodes = 0;
  uint32_t staleSamples = 0;
  bool stale = false;
};

// Single observer, independent of the network owner. Pass a synchronized copy.
// Samples count observations, not elapsed seconds. With on-demand health polling,
// episodes count observed stalls; stalls entirely between polls are not recorded.
// Time arithmetic assumes a monotonic uint32 millisecond clock, with observations
// less than one complete clock wrap apart. Zero threshold disables detection.
class Observer {
 public:
  Health sample(const Heartbeat &progress, uint32_t nowMs, uint32_t staleAfterMs) {
    Health result;
    result.progress = progress;
    if (progress.started) result.stageAgeMs = nowMs - progress.enteredAtMs;
    result.stale = progress.started && staleAfterMs != 0 && result.stageAgeMs >= staleAfterMs;
    const bool changed = progress.started != previous_.started ||
        progress.heartbeat != previous_.heartbeat || progress.stage != previous_.stage ||
        progress.enteredAtMs != previous_.enteredAtMs ||
        progress.completedLoops != previous_.completedLoops;
    if (result.stale) {
      staleSamples_ = increment(staleSamples_);
      if (!wasStale_ || changed) staleEpisodes_ = increment(staleEpisodes_);
    }
    previous_ = progress;
    wasStale_ = result.stale;
    result.staleEpisodes = staleEpisodes_;
    result.staleSamples = staleSamples_;
    return result;
  }

 private:
  Heartbeat previous_;
  uint32_t staleEpisodes_ = 0, staleSamples_ = 0;
  bool wasStale_ = false;
};

}  // namespace network_progress
