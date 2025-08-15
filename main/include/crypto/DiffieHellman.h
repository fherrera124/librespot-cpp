#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include "mbedtls/bignum.h"

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
  void computeSharedKey(const uint8_t* remotePublicKey, size_t keySize,
                        uint8_t* sharedKey);

  /**
   * @brief Returns the public key as a base64-encoded string.
   * 
   * @return std::string string containing the public key.
   */
  std::string getPublicKeyBase64();

  std::vector<uint8_t> getPublicKey() const {
    return {publicKey.begin(), publicKey.end()};
  }

 private:
  const char* LOG_TAG = "CspotDH";
  static const size_t dhKeySize = 96;

  // Holds the private and public key.
  std::array<uint8_t, dhKeySize> privateKey;
  std::array<uint8_t, dhKeySize> publicKey;

  // MbedTLS bignums
  mbedtls_mpi prime{};
  mbedtls_mpi generator{};
  mbedtls_mpi privateMpi{};

  // Fills the private key with random data.
  void generatePrivateKey();
};
}  // namespace cspot