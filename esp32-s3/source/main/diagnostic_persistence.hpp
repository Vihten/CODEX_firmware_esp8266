#pragma once

#include "diagnostic_archive.hpp"

namespace diagnostic_persistence {
struct Metadata {
  bool available = false;
  bool failed = false;
  bool pending = false;
  std::size_t count = 0;
  uint64_t newest = 0;
  uint32_t last_saved_ms = 0;
};

// initialize runs before the first renderer control frame. All later writes
// run only in the network owner's acknowledged flash-maintenance window.
bool initialize(const char* directory, uint64_t firmware, uint32_t boot,
                uint32_t reset_cause) noexcept;
void changed() noexcept;
// Trial-boot failure. Captures into RTC memory even before storage or IPC exists.
void retainOtaRejection(uint64_t firmware, const char* status, int32_t reason, int32_t detail) noexcept;
// Call only before first output or in an acknowledged flash-maintenance window.
bool archiveOtaRejection(int64_t now) noexcept;
bool due(int64_t now, bool urgent = false) noexcept;
bool flush(int64_t now) noexcept;
bool metadata(Metadata& out) noexcept;
bool readAfter(uint64_t after, uint64_t until,
               diagnostic_archive::Record& out) noexcept;
} // namespace diagnostic_persistence
