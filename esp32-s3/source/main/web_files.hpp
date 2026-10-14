#pragma once

#include <cstdio>
#include "esp_http_server.h"

// Boot-only installation, before the first renderer control snapshot.
bool initializeWebFiles();
FILE *openWebFile(const char *relativePath);
esp_err_t serveWebFile(httpd_req_t *request, const char *relativePath, const char *mime);
const char *webFilesVersion();
