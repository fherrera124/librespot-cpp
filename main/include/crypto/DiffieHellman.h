#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include "mbedtls/build_info.h"  // for MBEDTLS_VERSION_NUMBER, checked below
// mbedTLS 4.0 moved this under mbedtls/private/ (still shipped, just
// relocated). ESP-IDF's own port has a wrapper at the classic path
// (components/mbedtls/port/include/mbedtls/bignum.h) that's meant to
// redirect here via #include_next, but its search-path continuation
// doesn't reliably reach tf-psa-crypto/drivers/builtin/include in this
// build (real, reproduced failure - "mbedtls/private/bignum.h: No such
// file", raised from inside the wrapper itself) - including the real
// header directly instead, same as CDNDataStream.h's aes.h.
#if MBEDTLS_VERSION_NUMBER >= 0x04000000
#include "mbedtls/private/bignum.h"
#else
#include "mbedtls/bignum.h"
#endif

namespace cspot {
// Diffie-Hellman key exchange, used for authenticating with Spotify.
// The key size used by Spotify is 96 bytes, hence the fixed size.
class DH {
 public:
  // Constructor, will generate a new key pair.
  DH();
  ~DH();

  /**
   * @brief Compute the shared key from the remote public key.
   *
   * @param remotePublicKey Pointer to the remote public key buffer.
   * @param keySize Amount of bytes in the remote public key.
   * @param sharedKey Pointer to the buffer where the shared key will be stored. Must be at least 96 bytes.
   */
  void computeSharedKey(const std::byte* remotePublicKey, size_t keySize,
                        std::byte* sharedKey);

  /**
   * @brief Returns the public key as a base64-encoded string.
   *
   * @return std::string string containing the public key.
   */
  std::string getPublicKeyBase64();

  std::vector<std::byte> getPublicKey() const {
    return {publicKey.begin(), publicKey.end()};
  }

 private:
  const char* LOG_TAG = "CspotDH";
  static const size_t dhKeySize = 96;

  // Holds the private and public key.
  std::array<std::byte, dhKeySize> privateKey;
  std::array<std::byte, dhKeySize> publicKey;

  // MbedTLS bignums
  mbedtls_mpi prime{};
  mbedtls_mpi generator{};
  mbedtls_mpi privateMpi{};

  // Fills the private key with random data.
  void generatePrivateKey();
};
}  // namespace cspot
