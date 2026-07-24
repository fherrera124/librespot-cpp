#pragma once

// Standard includes
#include <string_view>

// Local includes
#include <mbedtls/md.h>

namespace bell::utils {
class DigestCrypto {
 public:
  /**
   * @brief Constructs an DigestCrypto object.
   *
   * Initializes the digest context with the specified hashing algorithm.
   *
   * @param type The type of hash algorithm to use.
   */
  DigestCrypto(mbedtls_md_type_t type);
  ~DigestCrypto();

  /**
   * @brief Updates the digest with a chunk of data.
   *
   * Feeds the specified bytes into the ongoing hash computation.
   *
   * @param bytes Pointer to the byte array to update the hash with.
   * @param length The number of bytes to process.
   */
  void update(const std::byte* bytes, size_t length);

  /**
   * @brief Updates the digest with a string.
   *
   * Feeds the specified string into the ongoing hash computation.
   *
   * @param str The string to update the hash with.
   */
  void updateString(std::string_view str);

  /**
   * @brief Finalizes the digest computation.
   *
   * Completes the hash computation and stores the result in the output array.
   *
   * @remark Size of the output array must be at least getDigestSize() bytes.
   * @param output Pointer to the output array where the digest result will be stored.
   */
  void finish(std::byte* output);

  /**
   * @brief Gets the size of the digest output.
   *
   * Returns the size of the resulting digest in bytes for the selected algorithm.
   *
   * @return The digest size in bytes.
   */
  size_t getDigestSize();

  /**
   * @brief Resets the digest context.
   */
  void reset();

  /**
   * @brief Performs a one-shot hash computation, simplifying the process.
   *
   * @param bytes Pointer to the byte array to hash.
   * @param length The number of bytes to hash.
   * @param output Pointer to the output array where the digest result will be stored, must be at least getDigestSize() bytes.
   */
  void getDigest(const std::byte* bytes, size_t length, std::byte* output);

  /**
   * @brief Performs a one-shot HMAC computation, simplifying the process.
   *
   * Goes through the PSA MAC API, not the mbedtls_md context above -
   * mbedTLS 4.0 no longer supports HMAC via the classic MD API at all
   * ("HMAC operations are no longer supported via MD", md.h's own words).
   *
   * @param key Pointer to the key array.
   * @param keyLength Length of the key in bytes.
   * @param message Pointer to the message array.
   * @param messageLength Length of the message in bytes.
   * @param output Pointer to the output array where the HMAC result will be stored, must be at least getDigestSize() bytes.
   */
  void getHmac(const std::byte* key, size_t keyLength, const std::byte* message,
               size_t messageLength, std::byte* output);

 private:
  mbedtls_md_context_t ctx{};
  mbedtls_md_type_t digestType;
};
}  // namespace bell::utils

namespace bell {
using DigestCrypto = utils::DigestCrypto;
}
