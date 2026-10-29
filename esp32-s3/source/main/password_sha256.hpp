#pragma once
#include <cstddef>
#include <cstdint>

// IDF 6 / Mbed TLS 4 builtin software SHA, deliberately local to password KDFs.
// Do not change global PSA acceleration or depend on private context fields.
#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#include "mbedtls/private/sha256.h"
#undef MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS

namespace password_sha256 {
class Hash {
 public:
  Hash() { mbedtls_sha256_init(&context_); }
  ~Hash() { mbedtls_sha256_free(&context_); }
  Hash(const Hash &) = delete;
  Hash &operator=(const Hash &) = delete;

  bool start() { return mbedtls_sha256_starts(&context_, 0) == 0; }
  bool update(const uint8_t *bytes, size_t size) {
    return mbedtls_sha256_update(&context_, bytes, size) == 0;
  }
  bool finish(uint8_t *out) { return mbedtls_sha256_finish(&context_, out) == 0; }
  void cloneFrom(const Hash &seed) {
    mbedtls_sha256_clone(&context_, &seed.context_);
  }
  static bool compute(const uint8_t *bytes, size_t size, uint8_t *out) {
    return mbedtls_sha256(bytes, size, out, 0) == 0;
  }

 private:
  mbedtls_sha256_context context_;
};
}  // namespace password_sha256
