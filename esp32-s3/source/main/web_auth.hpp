#pragma once

#include <cstdint>
#include "esp_http_server.h"

namespace web_auth {
// Boot only, after NVS initialization. Storage errors lock access, never reset it.
void initialize();
bool registerRoutes(httpd_handle_t server);
// Sends a redirect/401/403/503 when false. Does not set portal CSP headers.
bool authorize(httpd_req_t *request);
bool pendingSave();
// Network task only, inside the renderer's quiescent flash maintenance guard.
bool persist();
bool busy();
// Close admission before OTA/restart, then wait for !busy() and !pendingSave().
void setMaintenance(bool enabled);
// Atomically seal admission only when no password operation or save is active.
bool tryMaintenance();
// Last password-check diagnostics; no credential or session data.
uint32_t lastCheckMs();
uint32_t httpStackFree();
uint32_t sessionRevision();
// Optional async-stream checks: capture once, then check without request headers.
bool captureAuthenticatedSession(httpd_req_t *request, char token[65]);
bool sessionActive(const char *token);
}  // namespace web_auth
