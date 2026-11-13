#pragma once

#include <cstdint>

namespace network_memory {

struct Snapshot {
  uint32_t sampledAt = 0;
  uint32_t active = 0;
  uint32_t timeWait = 0;
  uint32_t queuedBytes = 0;
  uint32_t queuedSegments = 0;
  uint32_t callbackFailures = 0;
  bool sampled = false;
};

// Task-context API. Call from ordinary flash-guarded IDF tasks, never an ISR
// or a cache-disabled flash callback. Concurrent requests coalesce while pending.
void request();
Snapshot snapshot();

}  // namespace network_memory
