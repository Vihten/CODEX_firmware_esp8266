#pragma once
#include <cstdint>
#include "daily.hpp"
#include "esp_http_server.h"
namespace daily_runtime {
void initialize(bool nvsReady);
bool validPacket(const char *packet);
// LFD1/LFD2 acknowledgements confirm RAM acceptance. Sender must replay.
bool accept(const char *packet, uint32_t today, uint32_t deviceId, char (&ack)[64]);
struct WeeklySnapshot {
  daily::WeekSummary codex, claude;
  bool codexReady = false, claudeReady = false;
};
// Bounded lock-only snapshot; never allocates or performs flash I/O.
void weeklySummary(uint32_t today, WeeklySnapshot &out);
bool dirty();
bool due(uint32_t now, bool flush);
bool persist();
esp_err_t get(httpd_req_t *request, uint32_t today);
}
