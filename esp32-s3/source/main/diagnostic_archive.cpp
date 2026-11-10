#include "diagnostic_archive.hpp"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace diagnostic_archive {
namespace {
constexpr std::size_t kRecordBytes = 272;
constexpr std::size_t kCrcOffset = kRecordBytes - 4;
#ifdef _WIN32
int openFile(const char* p, int flags) { return _open(p, flags | _O_BINARY, _S_IREAD | _S_IWRITE); }
int closeFile(int fd) { return _close(fd); }
int syncFile(int fd) { return _commit(fd); }
int resizeFile(int fd, std::size_t n) { return static_cast<int>(_chsize_s(fd, n)); }
int seekFile(int fd, std::size_t n) { return _lseek(fd, static_cast<long>(n), SEEK_SET) < 0 ? -1 : 0; }
int readFile(int fd, void* p, std::size_t n) { return _read(fd, p, static_cast<unsigned>(n)); }
int writeFile(int fd, const void* p, std::size_t n) { return _write(fd, p, static_cast<unsigned>(n)); }
#else
int openFile(const char* p, int flags) { return ::open(p, flags, 0600); }
int closeFile(int fd) { return ::close(fd); }
int syncFile(int fd) { return ::fsync(fd); }
int resizeFile(int fd, std::size_t n) { return ::ftruncate(fd, static_cast<off_t>(n)); }
int seekFile(int fd, std::size_t n) { return ::lseek(fd, static_cast<off_t>(n), SEEK_SET) < 0 ? -1 : 0; }
ssize_t readFile(int fd, void* p, std::size_t n) { return ::read(fd, p, n); }
ssize_t writeFile(int fd, const void* p, std::size_t n) { return ::write(fd, p, n); }
#endif
void put(uint8_t*& p, uint64_t value, unsigned bytes) {
  for (unsigned i = 0; i < bytes; ++i) { *p++ = static_cast<uint8_t>(value); value >>= 8; }
}
uint64_t get(const uint8_t*& p, unsigned bytes) {
  uint64_t value = 0;
  for (unsigned i = 0; i < bytes; ++i) value |= uint64_t(*p++) << (i * 8);
  return value;
}
uint32_t crc(const uint8_t* data) {
  uint32_t value = 0xffffffffu;
  for (std::size_t i = 0; i < kCrcOffset; ++i) {
    value ^= data[i];
    for (unsigned bit = 0; bit < 8; ++bit)
      value = (value >> 1) ^ (0xedb88320u & (0u - (value & 1u)));
  }
  return ~value;
}
// Fixed v1 layout, all integers little endian. Reserved bytes are zero.
// Header magic "DARC", u16 version, u16 length. CRC32/IEEE covers bytes 0..267.
void encode(const Record& r, uint8_t (&bytes)[kRecordBytes]) {
  std::memset(bytes, 0, sizeof(bytes));
  uint8_t* p = bytes;
  put(p, 0x43524144u, 4); put(p, 1, 2); put(p, kRecordBytes, 2);
  put(p, r.serial, 8); put(p, r.firmware_version, 8);
  put(p, r.boot_id, 4); put(p, r.reset_cause, 4);
  const auto& e = r.entry;
  put(p, e.sequence, 8); put(p, e.uptime_ms, 4); put(p, e.unix_epoch_s, 4);
  put(p, e.last_uptime_ms, 4); put(p, e.last_unix_epoch_s, 4); put(p, e.repeat_count, 4);
  const auto& s = e.snapshot;
  put(p, static_cast<uint32_t>(s.reason), 4); put(p, s.attempts, 4); put(p, s.elapsed_ms, 4);
  put(p, s.heap_free, 4); put(p, s.heap_min, 4); put(p, s.largest_block, 4);
  put(p, s.network_stage, 4); put(p, s.network_age_ms, 4); put(p, s.render_frames, 4);
  put(p, static_cast<uint16_t>(s.wifi_rssi), 2); put(p, s.agents, 1); put(p, s.wifi_connected, 1);
  put(p, static_cast<uint32_t>(s.detail), 4); put(p, s.context_uptime_ms, 4); put(p, s.view_valid, 1);
  put(p, e.code, 2); put(p, static_cast<uint8_t>(e.module), 1); put(p, static_cast<uint8_t>(e.severity), 1);
  std::memcpy(p, e.text, sizeof(e.text)); p += sizeof(e.text);
  put(p, e.text_truncated, 1);
  p = bytes + kCrcOffset; put(p, crc(bytes), 4);
}
// Decode signed two's-complement values without implementation-defined casts.
int32_t signed32(uint64_t n) {
  return n <= INT32_MAX ? static_cast<int32_t>(n) : -1 - static_cast<int32_t>(UINT32_MAX - n);
}
bool decode(const uint8_t (&bytes)[kRecordBytes], Record& r) {
  const uint8_t* p = bytes + kCrcOffset;
  if (get(p, 4) != crc(bytes)) return false;
  p = bytes;
  if (get(p, 4) != 0x43524144u || get(p, 2) != 1 || get(p, 2) != kRecordBytes) return false;
  r = Record{};
  r.serial = get(p, 8); r.firmware_version = get(p, 8);
  r.boot_id = static_cast<uint32_t>(get(p, 4)); r.reset_cause = static_cast<uint32_t>(get(p, 4));
  auto& e = r.entry;
  e.sequence = get(p, 8);
  e.uptime_ms = static_cast<uint32_t>(get(p, 4)); e.unix_epoch_s = static_cast<uint32_t>(get(p, 4));
  e.last_uptime_ms = static_cast<uint32_t>(get(p, 4)); e.last_unix_epoch_s = static_cast<uint32_t>(get(p, 4));
  e.repeat_count = static_cast<uint32_t>(get(p, 4));
  auto& s = e.snapshot;
  s.reason = signed32(get(p, 4)); s.attempts = static_cast<uint32_t>(get(p, 4));
  s.elapsed_ms = static_cast<uint32_t>(get(p, 4)); s.heap_free = static_cast<uint32_t>(get(p, 4));
  s.heap_min = static_cast<uint32_t>(get(p, 4)); s.largest_block = static_cast<uint32_t>(get(p, 4));
  s.network_stage = static_cast<uint32_t>(get(p, 4)); s.network_age_ms = static_cast<uint32_t>(get(p, 4));
  s.render_frames = static_cast<uint32_t>(get(p, 4));
  const uint64_t rssi = get(p, 2);
  s.wifi_rssi = static_cast<int16_t>(rssi <= INT16_MAX ? static_cast<int32_t>(rssi) : -1 - static_cast<int32_t>(UINT16_MAX - rssi));
  s.agents = static_cast<uint8_t>(get(p, 1));
  const auto connected = get(p, 1);
  s.wifi_connected = connected != 0;
  s.detail = signed32(get(p, 4)); s.context_uptime_ms = static_cast<uint32_t>(get(p, 4));
  const auto valid = get(p, 1); s.view_valid = valid != 0;
  e.code = static_cast<uint16_t>(get(p, 2));
  e.module = static_cast<diagnostic_log::Module>(get(p, 1));
  e.severity = static_cast<diagnostic_log::Severity>(get(p, 1));
  std::memcpy(e.text, p, sizeof(e.text)); p += sizeof(e.text);
  const auto truncated = get(p, 1); e.text_truncated = truncated != 0;
  if (!r.serial || connected > 1 || valid > 1 || truncated > 1 ||
      e.module > diagnostic_log::Module::Output || e.severity > diagnostic_log::Severity::Error ||
      !std::memchr(e.text, 0, sizeof(e.text))) return false;
  while (p < bytes + kCrcOffset) if (*p++ != 0) return false;
  return true;
}
}  // namespace

Store::Store(Record* cache, std::size_t capacity) noexcept
    : cache_(cache), capacity_(cache && capacity <= kCapacity ? capacity : 0) {}
bool Store::path(std::size_t index, char* out, std::size_t bytes) const noexcept {
  const int n = std::snprintf(out, bytes, "%s/segment%u.bin", directory_, static_cast<unsigned>(index));
  return n >= 0 && static_cast<std::size_t>(n) < bytes;
}
void Store::remember(const Record& record) noexcept {
  for (std::size_t i = 0; i < size_; ++i) {
    if (cache_[i].boot_id != record.boot_id || cache_[i].entry.sequence != record.entry.sequence) continue;
    if (cache_[i].serial >= record.serial) return;
    for (std::size_t j = i + 1; j < size_; ++j) cache_[j - 1] = cache_[j];
    --size_; break;
  }
  if (size_ == capacity_) {
    if (record.serial <= cache_[0].serial) return;
    for (std::size_t i = 1; i < size_; ++i) cache_[i - 1] = cache_[i];
    --size_;
  }
  std::size_t index = size_;
  while (index && cache_[index - 1].serial > record.serial) {
    cache_[index] = cache_[index - 1]; --index;
  }
  cache_[index] = record; ++size_;
}
bool Store::open(const char* directory) noexcept {
  ready_ = false; size_ = 0; newest_ = 0;
  for (auto& segment : segments_) segment = Segment{};
  if (!capacity_ || !directory || !*directory || std::strlen(directory) >= sizeof(directory_)) return false;
  std::memcpy(directory_, directory, std::strlen(directory) + 1);
#ifdef _WIN32
  const int made = _mkdir(directory_);
#else
  const int made = ::mkdir(directory_, 0700);
#endif
  struct stat info{};
  if ((made != 0 && errno != EEXIST) || ::stat(directory_, &info) != 0 ||
      (info.st_mode & S_IFMT) != S_IFDIR) return false;
  // Scan segments in ascending serial order so revisions cannot resurrect an
  // evicted identity when physical segment numbers wrap around.
  for (std::size_t i = 0; i < kSegmentCount; ++i) {
    char filename[416]; if (!path(i, filename, sizeof(filename))) return false;
    const int fd = openFile(filename, O_RDONLY);
    if (fd < 0) { if (errno == ENOENT) continue; return false; }
    auto& segment = segments_[i];
    for (;;) {
      uint8_t bytes[kRecordBytes]; std::size_t count = 0;
      while (count < sizeof(bytes)) {
        const auto n = readFile(fd, bytes + count, sizeof(bytes) - count);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { closeFile(fd); return false; }
        if (!n) break;
        count += static_cast<std::size_t>(n);
      }
      if (!count) break;
      Record record;
      if (count != sizeof(bytes) || segment.bytes + count > kSegmentBytes ||
          !decode(bytes, record) || record.serial <= segment.newest) { segment.repair = true; break; }
      segment.bytes += count; segment.newest = record.serial;
      if (record.serial > newest_) newest_ = record.serial;
    }
    if (closeFile(fd) != 0) return false;
  }
  bool scanned[kSegmentCount]{};
  for (std::size_t pass = 0; pass < kSegmentCount; ++pass) {
    std::size_t index = kSegmentCount;
    for (std::size_t i = 0; i < kSegmentCount; ++i)
      if (!scanned[i] && (index == kSegmentCount || segments_[i].newest < segments_[index].newest)) index = i;
    scanned[index] = true;
    if (!segments_[index].bytes) continue;
    char filename[416]; if (!path(index, filename, sizeof(filename))) return false;
    const int fd = openFile(filename, O_RDONLY); if (fd < 0) return false;
    for (std::size_t offset = 0; offset < segments_[index].bytes; offset += kRecordBytes) {
      uint8_t bytes[kRecordBytes]; std::size_t count = 0;
      while (count < sizeof(bytes)) {
        const auto n = readFile(fd, bytes + count, sizeof(bytes) - count);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { closeFile(fd); return false; }
        count += static_cast<std::size_t>(n);
      }
      Record record;
      if (!decode(bytes, record)) { closeFile(fd); return false; }
      remember(record);
    }
    if (closeFile(fd) != 0) return false;
  }
  ready_ = true; return true;
}
bool Store::append(Record record) noexcept {
  if (!ready_ || newest_ == std::numeric_limits<uint64_t>::max()) return false;
  record.serial = newest_ + 1;
  uint8_t bytes[kRecordBytes]; encode(record, bytes);
  Record checked; if (!decode(bytes, checked)) return false;
  std::size_t index = 0;
  for (std::size_t i = 1; i < kSegmentCount; ++i)
    if (segments_[i].newest > segments_[index].newest) index = i;
  bool rotate = segments_[index].bytes + kRecordBytes > kSegmentBytes;
  if (rotate) {
    index = 0;
    for (std::size_t i = 1; i < kSegmentCount; ++i)
      if (segments_[i].newest < segments_[index].newest) index = i;
  }
  auto& segment = segments_[index];
  char filename[416]; if (!path(index, filename, sizeof(filename))) return false;
  const int fd = openFile(filename, O_RDWR | O_CREAT); if (fd < 0) return false;
  if (rotate || segment.repair) {
    const std::size_t prefix = rotate ? 0 : segment.bytes;
    if (resizeFile(fd, prefix) != 0) { segment.repair = true; closeFile(fd); return false; }
    // Reflect a successful truncation even if its subsequent sync fails.
    // Otherwise a retry could extend the file back to the discarded length.
    if (rotate) {
      // A storm of revisions can leave fewer than 256 distinct identities in
      // newer segments. Remove any cache entries whose only copy was retired.
      std::size_t retained = 0;
      for (std::size_t i = 0; i < size_; ++i)
        if (cache_[i].serial > segment.newest) cache_[retained++] = cache_[i];
      size_ = retained;
      segment = Segment{};
    }
    if (syncFile(fd) != 0) { segment.repair = true; closeFile(fd); return false; }
    segment.repair = false;
  }
  if (seekFile(fd, segment.bytes) != 0) { closeFile(fd); return false; }
  // Keep the known-good prefix until both write and sync succeed. A failed
  // close is treated conservatively too. The next append removes this tail.
  segment.repair = true;
  std::size_t count = 0;
  while (count < sizeof(bytes)) {
    const auto n = writeFile(fd, bytes + count, sizeof(bytes) - count);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) { closeFile(fd); return false; }
    count += static_cast<std::size_t>(n);
  }
  const bool synced = syncFile(fd) == 0;
  const bool closed = closeFile(fd) == 0;
  if (!synced || !closed) return false;
  segment.bytes += sizeof(bytes); segment.newest = record.serial; segment.repair = false;
  newest_ = record.serial; remember(record); return true;
}
bool Store::readAfter(uint64_t after, uint64_t until, Record& out) const noexcept {
  if (!ready_) return false;
  for (std::size_t i = 0; i < size_; ++i) {
    if (cache_[i].serial > until) break;
    if (cache_[i].serial > after) { out = cache_[i]; return true; }
  }
  return false;
}
}  // namespace diagnostic_archive
