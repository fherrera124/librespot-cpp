#pragma once

#include <mutex>
#include <unordered_map>
#include <vector>

#include <bell/Result.h>
#include <bell/io/BinaryStream.h>
#include <bell/utils/DigestCrypto.h>
#include <tao/json.hpp>

#include "crypto/DiffieHellman.h"

namespace cspot {
class LoginBlob {
 public:
  LoginBlob();

  bell::Result<> authenticateZeroconfQuery(
      const std::unordered_map<std::string, std::string>& queryParams);

  bell::Result<> authenticateZeroconfString(std::string_view queryStr);

  std::string buildZeroconfJSONResponse();
  std::string getUsername();
  std::string getDeviceName();
  std::string getDeviceId();
  void setDeviceName(const std::string& deviceName);
  bool isAuthenticated();
  uint32_t getAuthType();

  std::vector<std::byte> getStoredAuthBlob();

  bell::Result<tao::json::value> getJSONForStorage();
  bell::Result<> restoreFromJSON(
      const tao::json::value& jsonData);  // Restore from JSON

  bell::Result<> decodeEncryptedAuthBlob(
      const std::string& username, const std::vector<std::byte>& authBlob);

 private:
  const char* LOG_TAG = "LoginBlob";

  // Device name, set on construction
  std::string deviceName;

  // Derieved from device name
  std::string deviceId;

  // Signed-in username, will be set after authentication
  std::string username;

  // Used for SHA1 computations
  bell::utils::DigestCrypto sha1Context{MBEDTLS_MD_SHA1, true};

  // Used for Diffie-Hellman key exchange
  DH dhPair;

  // Base64 encoded public key
  std::string dhPublicKey;

  std::recursive_mutex accessMutex;

  std::vector<std::byte> encryptedAuthBlob;

  std::vector<std::byte> authBlob;

  uint32_t authType = 0;  // Authentication type, set after authentication

  bell::Result<std::vector<std::byte>> decodeZeroconfBlob(
      const std::vector<std::byte>& blob,
      const std::vector<std::byte>& clientKey);

  static uint32_t readUvarint(bell::io::BinaryStream& stream);

  static bell::Result<> base64Decode(std::string_view encoded,
                                     std::vector<std::byte>& targetBuffer);
};
}  // namespace cspot
