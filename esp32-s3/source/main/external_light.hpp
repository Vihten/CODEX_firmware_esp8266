#pragma once
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace external_light {
constexpr int kFirstPin = 4;
constexpr int kSecondPin = 5;

// Versioned, padding-free NVS blob. Seal after edits and validate after loading.
struct Settings {
  uint32_t magic = 0x584c5331;
  uint8_t first = 0;
  uint8_t second = 0;
  uint8_t followMatrix = 1;
  uint8_t reserved = 0;
  uint32_t checksum = 0;
};
static_assert(sizeof(Settings) == 12 && offsetof(Settings, checksum) == 8,
              "stable external light storage layout");
static_assert(std::is_trivially_copyable<Settings>::value,
              "external light settings must be a storage blob");

inline uint32_t checksum(const Settings &settings) {
  const auto *bytes = reinterpret_cast<const uint8_t *>(&settings);
  uint32_t crc = UINT32_MAX;
  for (size_t i = 0; i < offsetof(Settings, checksum); ++i) {
    crc ^= bytes[i];
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
  }
  return ~crc;
}
inline void seal(Settings &settings) { settings.checksum = checksum(settings); }
inline bool valid(const Settings &settings) {
  return settings.magic == 0x584c5331 && settings.first <= 100 &&
      settings.second <= 100 && settings.followMatrix <= 1 &&
      settings.reserved == 0 && settings.checksum == checksum(settings);
}

// Initialize once in app_main before task creation; renderTask owns apply thereafter.
// No startup activation. Any initialization/write failure latches OFF until reboot.
bool initialize();
bool ready();
bool apply(uint8_t first, uint8_t second, bool allowed);
bool applyScaled(uint8_t first, uint8_t second, uint16_t gain);

inline uint16_t scaledDuty(uint8_t percent, uint16_t gain) {
  const uint32_t duty = (uint32_t(percent > 100 ? 100 : percent) * 1023U + 50U) / 100U;
  return uint16_t((duty * (gain > 1023 ? 1023 : gain) + 511U) / 1023U);
}
inline uint8_t outputPercent(uint8_t percent, uint16_t gain) {
  return uint8_t((uint32_t(scaledDuty(percent, gain)) * 100U + 511U) / 1023U);
}

// Generic PWM only: an IRF520 is not guaranteed to switch fully at a 3.3 V gate.
}  // namespace external_light
