#pragma once
#include "diagnostic_log.hpp"
#include <cstddef>
#include <cstdint>

namespace diagnostic_archive {
constexpr std::size_t kCapacity = 256;
constexpr std::size_t kSegmentBytes = 65536;
constexpr std::size_t kSegmentCount = 4;
struct Record {
  uint64_t serial = 0, firmware_version = 0;
  uint32_t boot_id = 0, reset_cause = 0;
  diagnostic_log::Entry entry{};
};

// Single owner. Caller supplies exclusive storage and serializes every call.
// No heap allocation or persistent open handles. Directory parents must exist.
class Store {
 public:
  explicit Store(Record* cache, std::size_t capacity = kCapacity) noexcept;
  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;
  bool open(const char* directory) noexcept;
  // Ignores input serial. Success means the complete record was fsynced.
  // A failed append may already be durable. Retry upserts the same identity.
  bool append(Record record) noexcept;
  // Copies the first cached record with after < serial <= until.
  // until=0 is an empty range. out must not alias the cache.
  bool readAfter(uint64_t after, uint64_t until, Record& out) const noexcept;
  std::size_t size() const noexcept { return size_; }
  uint64_t newest() const noexcept { return newest_; }
  bool ready() const noexcept { return ready_; }
 private:
  struct Segment {
    uint64_t newest = 0;
    std::size_t bytes = 0;
    bool repair = false;
  };
  bool path(std::size_t index, char* out, std::size_t bytes) const noexcept;
  void remember(const Record& record) noexcept;
  Record* cache_;
  std::size_t capacity_, size_ = 0;
  uint64_t newest_ = 0;
  bool ready_ = false;
  char directory_[384]{};
  Segment segments_[kSegmentCount]{};
};
}  // namespace diagnostic_archive
