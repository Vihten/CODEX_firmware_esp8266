#pragma once

#include <cstddef>
#include <cstdint>
#include "project_config.hpp"

constexpr size_t kPreviewHeaderBytes = 24;
constexpr size_t kPreviewFrameBytes = kPreviewHeaderBytes + size_t(PROJECT_PIXELS) * 3;
static_assert(kPreviewFrameBytes <= 4096, "Preview exceeds the bounded HTTP task buffer");
using PreviewMapping = uint16_t (*)(uint8_t, uint8_t);

bool initializePreview();
// Called only by the renderer after a successful LED transfer. Never waits.
void capturePreview(const uint8_t *grb, uint8_t brightness, uint32_t sequence,
                    uint32_t now, PreviewMapping mapping);
// Copies under a short lock, then releases it before any network transmission.
bool copyPreview(uint8_t *output, size_t capacity, uint32_t now);
