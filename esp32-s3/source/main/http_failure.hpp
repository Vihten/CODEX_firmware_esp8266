#pragma once

#include <cstdint>
#include <cstring>

namespace http_failure {

enum class Cause : uint16_t {
  General = 100, BusyView, PreviewUnavailable, PreviewBusy, PreviewDraining,
  PreviewStart, PreviewWarmup, Memory, Serialization, Maintenance, AgentRelease,
  AgentUpdate, Authentication, TelemetryQueue, TelemetryRejected,
  TelemetryTimeout, CommandQueue, OutputUnavailable
};

enum class Route : uint8_t {
  Unknown = 0, Overview, Agents, Settings, Status, Health, Frame, Frames,
  AgentTelemetry, AgentUpdate, Command, Asset, Other
};

inline Cause classify(const char *text) noexcept {
  if (!text) return Cause::General;
  struct Entry { const char *text; Cause cause; };
  static const Entry entries[] = {
    {"Controller busy; retry.", Cause::BusyView},
    {"Controller busy.", Cause::BusyView},
    {"Preview unavailable during maintenance.", Cause::PreviewUnavailable},
    {"Preview stream unavailable.", Cause::PreviewUnavailable},
    {"Preview stream is already in use.", Cause::PreviewBusy},
    {"Preview worker busy.", Cause::PreviewBusy},
    {"Preview stream is draining for maintenance.", Cause::PreviewDraining},
    {"Cannot start preview stream.", Cause::PreviewStart},
    {"Waiting for an LED frame.", Cause::PreviewWarmup},
    {"Not enough memory.", Cause::Memory},
    {"Status unavailable; retry.", Cause::Serialization},
    {"Diagnostics too large.", Cause::Serialization},
    {"Controller maintenance; retry.", Cause::Maintenance},
    {"Controller maintenance; retry after restart or update.", Cause::Maintenance},
    {"No signed agent release.", Cause::AgentRelease},
    {"Agent updates unavailable.", Cause::AgentUpdate},
    {"Update queue unavailable.", Cause::AgentUpdate},
    {"Authentication unavailable.", Cause::Authentication},
    {"Telemetry queue full.", Cause::TelemetryQueue},
    {"Agent table full; retry.", Cause::TelemetryRejected},
    {"Telemetry owner busy; retry.", Cause::TelemetryTimeout},
    {"Command queue full; retry.", Cause::CommandQueue},
    {"Виходи підсвітки недоступні.", Cause::OutputUnavailable},
    {"Звуковий вихід недоступний. Спробуйте ще раз.", Cause::OutputUnavailable},
  };
  for (const auto &entry : entries) {
    if (std::strcmp(text, entry.text) == 0) return entry.cause;
  }
  return Cause::General;
}

namespace detail {
// Read at most the known path length plus its delimiter, ignoring query data.
inline bool pathMatches(const char *uri, const char *path) noexcept {
  if (!uri) return false;
  while (*path) {
    if (*uri != *path) return false;
    ++uri;
    ++path;
  }
  return *uri == '\0' || *uri == '?';
}
} // namespace detail

inline Route route(const char *uri) noexcept {
  struct Entry { const char *path; Route route; };
  static const Entry entries[] = {
    {"/", Route::Overview}, {"/agents", Route::Agents},
    {"/settings", Route::Settings}, {"/status", Route::Status},
    {"/health", Route::Health}, {"/frame", Route::Frame},
    {"/frames", Route::Frames}, {"/agent", Route::AgentTelemetry},
    {"/agent-update-enroll", Route::AgentUpdate},
    {"/agent-update-challenge", Route::AgentUpdate},
    {"/agent-update-poll", Route::AgentUpdate},
    {"/agent-upgrade", Route::AgentUpdate},
    {"/agent-rename", Route::AgentUpdate},
    {"/agent-uninstall", Route::AgentUpdate},
    {"/agent-update/manifest.json", Route::AgentUpdate},
    {"/agent-update/package.zip", Route::AgentUpdate},
    {"/save", Route::Command}, {"/power", Route::Command},
    {"/brightness", Route::Command}, {"/external-light", Route::Command},
    {"/buzzer-test", Route::Command}, {"/buzzer-volume", Route::Command},
    {"/schedule", Route::Command}, {"/sky-colors", Route::Command},
    {"/agent-color", Route::Command}, {"/restart", Route::Command},
    {"/update", Route::Command}, {"/memorial", Route::Command},
    {"/black-hole", Route::Command}, {"/air-alert", Route::Command},
    {"/device.png", Route::Asset}, {"/favicon.svg", Route::Asset},
    {"/assets/css/portal.css", Route::Asset},
    {"/assets/css/preview.css", Route::Asset},
    {"/assets/js/portal.js", Route::Asset},
    {"/assets/js/preview.js", Route::Asset},
    {"/assets/js/boot.js", Route::Asset},
    {"/assets/js/token-history.js", Route::Asset},
    {"/token-history", Route::Other}, {"/logs", Route::Other},
    {"/logs.txt", Route::Other},
  };
  for (const auto &entry : entries) {
    if (detail::pathMatches(uri, entry.path)) return entry.route;
  }
  return Route::Unknown;
}

inline const char *name(Cause value) noexcept {
  switch (value) {
    case Cause::General: return "general";
    case Cause::BusyView: return "busy_view";
    case Cause::PreviewUnavailable: return "preview_unavailable";
    case Cause::PreviewBusy: return "preview_busy";
    case Cause::PreviewDraining: return "preview_draining";
    case Cause::PreviewStart: return "preview_start";
    case Cause::PreviewWarmup: return "preview_warmup";
    case Cause::Memory: return "memory";
    case Cause::Serialization: return "serialization";
    case Cause::Maintenance: return "maintenance";
    case Cause::AgentRelease: return "agent_release";
    case Cause::AgentUpdate: return "agent_update";
    case Cause::Authentication: return "authentication";
    case Cause::TelemetryQueue: return "telemetry_queue";
    case Cause::TelemetryRejected: return "telemetry_rejected";
    case Cause::TelemetryTimeout: return "telemetry_timeout";
    case Cause::CommandQueue: return "command_queue";
    case Cause::OutputUnavailable: return "output_unavailable";
  }
  return "general";
}

inline const char *name(Route value) noexcept {
  switch (value) {
    case Route::Unknown: return "unknown";
    case Route::Overview: return "/";
    case Route::Agents: return "/agents";
    case Route::Settings: return "/settings";
    case Route::Status: return "/status";
    case Route::Health: return "/health";
    case Route::Frame: return "/frame";
    case Route::Frames: return "/frames";
    case Route::AgentTelemetry: return "/agent";
    case Route::AgentUpdate: return "agent_update";
    case Route::Command: return "command";
    case Route::Asset: return "asset";
    case Route::Other: return "other";
  }
  return "unknown";
}

} // namespace http_failure
