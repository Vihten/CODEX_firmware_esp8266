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
bool due(uint32_t now, bool urgent = false) noexcept;
bool flush(uint32_t now) noexcept;
bool metadata(Metadata& out) noexcept;
bool readAfter(uint64_t after, uint64_t until,
               diagnostic_archive::Record& out) noexcept;
} // namespace diagnostic_persistence
