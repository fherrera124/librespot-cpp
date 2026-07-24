#pragma once

#include <mutex>
#include <unordered_map>
#include <vector>

#include <bell/Result.h>
#include <bell/io/BinaryStream.h>
#include <tao/json.hpp>

// DiffieHellman.h must come before DigestCrypto.h: the former pulls in
// mbedtls/bignum.h via ESP-IDF's port wrapper (which #defines
// MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS then #include_nexts the real header),
// the latter pulls in <psa/crypto.h> - if psa/crypto.h's own chain reaches
// the same underlying header first, without that define set, its include
// guard locks it in undeclared for the rest of this file. See
// CDNDataStream.h's own comment for the full story.
#include "crypto/DiffieHellman.h"
#include <bell/utils/DigestCrypto.h>
#include "proto/AuthenticationPb.h"

namespace cspot {
class Authenticator {
 public:
  Authenticator();

  bell::Result<cspot_proto::LoginCredentials> authenticateZeroconfQuery(
      std::string_view deviceId,
      const std::unordered_map<std::string, std::string>& queryParams);

  bell::Result<cspot_proto::LoginCredentials> authenticateZeroconfString(
      std::string_view deviceId, std::string_view queryStr);

  std::string buildZeroconfJSONResponse(std::string_view deviceName,
                                        std::string_view deviceId,
                                        std::string_view activeUser);

 private:
  const char* LOG_TAG = "Authenticator";

  // Used for SHA1 computations
  bell::utils::DigestCrypto sha1Context{MBEDTLS_MD_SHA1};

  // Used for Diffie-Hellman key exchange
  DH dhPair;

  // Base64 encoded public key
  std::string dhPublicKey;

  std::recursive_mutex accessMutex;

  bell::Result<std::vector<std::byte>> decodeZeroconfBlob(
      const std::vector<std::byte>& blob,
      const std::vector<std::byte>& clientKey);

  bell::Result<cspot_proto::LoginCredentials> decodeEncryptedAuthBlob(
      std::string_view deviceId, std::string_view username,
      const std::vector<std::byte>& authBlob);

  static uint32_t readUvarint(bell::io::BinaryStream& stream);

  static bell::Result<> base64Decode(std::string_view encoded,
                                     std::vector<std::byte>& targetBuffer);
};
}  // namespace cspot
