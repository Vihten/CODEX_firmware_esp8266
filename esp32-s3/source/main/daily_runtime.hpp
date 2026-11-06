#pragma once
#include <cstdint>
#include "esp_http_server.h"
namespace daily_runtime {
void initialize(bool nvsReady);
// LFD1_RAM_ACK confirms RAM acceptance, never durable storage. Sender must replay.
bool accept(const char *packet, uint32_t today, uint32_t deviceId, char (&ack)[64]);
bool dirty();
bool due(uint32_t now, bool flush);
bool persist();
esp_err_t get(httpd_req_t *request, uint32_t today);
}
