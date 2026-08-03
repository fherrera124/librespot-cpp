#pragma once

// Standard includes
#include <cstddef>
#include <vector>

// Library includes
#include "bell/Result.h"
#include "mbedtls/build_info.h"  // for MBEDTLS_VERSION_NUMBER, checked below
// mbedTLS 4.0 moved both of these under mbedtls/private/ (still shipped,
// just relocated). ESP-IDF's own port has a bignum.h wrapper at the
// classic path meant to redirect via #include_next, but its search-path
// continuation doesn't reliably reach tf-psa-crypto/drivers/builtin/include
// in this build (real, reproduced failure) - including both real headers
// directly instead, same as aes.h (which never had a wrapper to begin with).
#if MBEDTLS_VERSION_NUMBER >= 0x04000000
#include "mbedtls/private/aes.h"
#else
#include "mbedtls/aes.h"
#endif

namespace cspot {

/**
 * @brief Decrypts Spotify CDN audio bytes (AES-128-CTR, fixed base IV).
 *
 * Stateless across calls beyond the expanded key schedule: decrypt() always
 * re-derives the CTR counter from the absolute wire byte position passed in,
 * so concurrent callers each need their own instance (mbedtls_aes_context's
 * thread-safety isn't relied upon here) but a single instance is safe to
 * reuse across any number of sequential calls, in any order.
 */
class AesCtrCipher {
 public:
  // decryptKey: raw AES-128 key bytes.
  explicit AesCtrCipher(const std::vector<std::byte>& decryptKey);
  ~AesCtrCipher();

  AesCtrCipher(const AesCtrCipher&) = delete;
  AesCtrCipher& operator=(const AesCtrCipher&) = delete;

  // Decrypts 'size' bytes in-place. 'position' is the absolute wire-byte
  // offset (from the start of the encrypted stream, before any header
  // trimming) that data[0] corresponds to - must be 16-byte aligned, same
  // requirement the caller already has to satisfy for the HTTP range
  // request itself to land on an AES block boundary.
  //
  // Fails (without ever touching 'data') if the key passed to the
  // constructor was rejected by mbedtls_aes_setkey_enc (wrong length) -
  // checked here rather than in the constructor since construction itself
  // can't report bell::Result in this codebase's style.
  bell::Result<> decrypt(std::byte* data, size_t size, size_t position);

  // True if the constructor's mbedtls_aes_setkey_enc call succeeded - lets
  // a caller that wants to fail fast (rather than waiting for the first
  // decrypt() call) check right after construction.
  bool hasValidKey() const { return keySetupResult == 0; }

 private:
  const char* LOG_TAG = "AesCtrCipher";
  mbedtls_aes_context aesCtx{};
  int keySetupResult = 0;
};

}  // namespace cspot
