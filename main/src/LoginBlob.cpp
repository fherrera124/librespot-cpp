#include <LoginBlob.h>

#include <string_view>

#include <bell/Logger.h>
#include <bell/io/BinaryStream.h>
#include <bell/io/MemoryStream.h>
#include <mbedtls/aes.h>
#include <mbedtls/base64.h>
#include <mbedtls/pkcs5.h>
#include <tao/json.hpp>
#include "Utils.h"
#include "bell/Result.h"
#include "bell/net/URIParser.h"

using namespace cspot;

namespace {
// Constants
const std::string deviceIdPrefix = "142137fd329622137a149016";
const std::string protocolVersion = "2.7.1";
const std::string swVersion = "2.0.0";
const std::string brandName = "cspot";
const std::string deviceType = "SPEAKER";
}  // namespace

LoginBlob::LoginBlob() {
  // Cache it, so we don't have to recalculate it
  dhPublicKey = dhPair.getPublicKeyBase64();
}

void LoginBlob::setDeviceName(const std::string& deviceName) {
  std::scoped_lock lock(accessMutex);
  this->deviceName = deviceName;
  // Recalculate device ID
  this->deviceId = fmt::format("{}{:016x}", deviceIdPrefix,
                               std::hash<std::string>{}(deviceName));
}

std::string LoginBlob::getUsername() {
  std::scoped_lock lock(accessMutex);
  return username;
}

std::string LoginBlob::getDeviceName() {
  std::scoped_lock lock(accessMutex);
  return deviceName;
}

std::string LoginBlob::getDeviceId() {
  std::scoped_lock lock(accessMutex);
  return deviceId;
}

bool LoginBlob::isAuthenticated() {
  std::scoped_lock lock(accessMutex);
  return !authBlob.empty();
}

std::string LoginBlob::buildZeroconfJSONResponse() {
  std::scoped_lock lock(accessMutex);

  tao::json::value obj;
  obj["status"] = 101;
  obj["statusString"] = "OK";
  obj["version"] = protocolVersion;
  obj["spotifyError"] = 0;
  obj["libraryVersion"] = swVersion;
  obj["accountReq"] = "PREMIUM";
  obj["brandDisplayName"] = brandName;
  obj["modelDisplayName"] = brandName;
  obj["voiceSupport"] = "NO";
  obj["productID"] = 0;
  obj["tokenType"] = "default";
  obj["groupStatus"] = "NONE";
  obj["resolverVersion"] = "0";
  obj["scope"] = "streaming,client-authorization-universal";
  obj["deviceType"] = deviceType;
  obj["availability"] = "";

  obj["deviceID"] = deviceId;
  obj["remoteName"] = deviceName;
  obj["publicKey"] = dhPublicKey;
  obj["activeUser"] = username;

  return tao::json::to_string(obj);
}

bell::Result<tao::json::value> LoginBlob::getJSONForStorage() {
  std::scoped_lock lock(accessMutex);

  if (!isAuthenticated()) {
    BELL_LOG(error, LOG_TAG,
             "Cannot get JSON for storage, user is not authenticated");
    return std::errc::operation_not_permitted;
    return bell::make_unexpected_errc(std::errc::operation_not_permitted);
  }

  tao::json::value obj;
  obj["deviceName"] = deviceName;
  obj["deviceId"] = deviceId;
  obj["username"] = username;
  obj["authType"] = authType;
  obj["authBlob"] = base64Encode(authBlob.data(), authBlob.size());

  return obj;
}

bell::Result<> LoginBlob::restoreFromJSON(const tao::json::value& jsonData) {
  std::scoped_lock lock(accessMutex);

  if (!jsonData.is_object()) {
    BELL_LOG(error, LOG_TAG, "JSON data is not an object");
    return std::errc::bad_message;
  }

  if (!jsonData.at("deviceName").is_string()) {
    BELL_LOG(error, LOG_TAG, "Device name not found in JSON data");
    return std::errc::bad_message;
  }
  deviceName = jsonData.at("deviceName").get_string();

  if (!jsonData.at("deviceId").is_string()) {
    BELL_LOG(error, LOG_TAG, "Device ID not found in JSON data");
    return std::errc::bad_message;
  }
  deviceId = jsonData.at("deviceId").get_string();

  if (!jsonData.at("username").is_string()) {
    BELL_LOG(error, LOG_TAG, "Username not found in JSON data");
    return std::errc::bad_message;
  }
  username = jsonData.at("username").get_string();

  if (!jsonData.at("authType").is_number()) {
    BELL_LOG(error, LOG_TAG, "Auth type not found in JSON data");
    return std::errc::bad_message;
  }
  authType = jsonData.at("authType").get_unsigned();

  if (!jsonData.at("authBlob").is_string()) {
    BELL_LOG(error, LOG_TAG, "Auth blob not found in JSON data");
    return std::errc::bad_message;
  }

  auto authBlobBase64 = jsonData.at("authBlob").get_string();
  auto decodeRes = base64Decode(authBlobBase64, authBlob);
  if (!decodeRes) {
    BELL_LOG(error, LOG_TAG, "Failed to base64 decode auth blob");
    return decodeRes.getError();
  }

  return {};
}

bell::Result<> LoginBlob::authenticateZeroconfQuery(
    const std::unordered_map<std::string, std::string>& queryParams) {
  if (!queryParams.contains("blob") || queryParams.at("blob").empty()) {
    BELL_LOG(error, LOG_TAG, "Blob not found in query string");
    return std::errc::bad_message;
  }

  if (!queryParams.contains("deviceId") || queryParams.at("deviceId").empty()) {
    BELL_LOG(error, LOG_TAG, "Device ID not found in query string");
    return std::errc::bad_message;
  }

  if (!queryParams.contains("clientKey") ||
      queryParams.at("clientKey").empty()) {
    BELL_LOG(error, LOG_TAG, "Client key not found in query string");
    return std::errc::bad_message;
  }
  // Holds base64 decoded blob and client key
  std::vector<uint8_t> decodedBlob;
  std::vector<uint8_t> decodedClientKey;

  auto res = base64Decode(queryParams.at("blob"), decodedBlob);
  if (!res) {
    BELL_LOG(error, LOG_TAG, "Failed to base64 decode blob");
    return res.getError();
  }

  res = base64Decode(queryParams.at("clientKey"), decodedClientKey);
  if (!res) {
    BELL_LOG(error, LOG_TAG, "Failed to base64 decode client key");
    return res.getError();
  }

  username = queryParams.at("userName");
  auto encryptedAuthBlobRes = decodeZeroconfBlob(decodedBlob, decodedClientKey);
  if (!encryptedAuthBlobRes) {
    BELL_LOG(error, LOG_TAG, "Failed to decode zeroconf blob");
    return encryptedAuthBlobRes.getError();
  }

  encryptedAuthBlob = encryptedAuthBlobRes.takeValue();

  // Decode the auth blob
  return decodeEncryptedAuthBlob(username, encryptedAuthBlob);
}

bell::Result<> LoginBlob::authenticateZeroconfString(
    std::string_view queryString) {
  std::scoped_lock lock(accessMutex);

  // Parse the query string
  std::unordered_map<std::string, std::string> queryParams;
  size_t pos = 0;
  while (pos < queryString.size()) {
    size_t nextPos = queryString.find('&', pos);
    if (nextPos == std::string::npos) {
      nextPos = queryString.size();
    }

    size_t eqPos = queryString.find('=', pos);
    if (eqPos == std::string::npos || eqPos > nextPos) {
      return std::errc::invalid_argument;
    }

    // Perform URL decoding
    std::string key =
        bell::net::decodeURLEncoded(queryString.substr(pos, eqPos - pos));
    std::string value = bell::net::decodeURLEncoded(
        queryString.substr(eqPos + 1, nextPos - eqPos - 1));

    queryParams[key] = value;
    pos = nextPos + 1;
  }

  return authenticateZeroconfQuery(queryParams);
}

std::vector<uint8_t> LoginBlob::getStoredAuthBlob() {
  std::scoped_lock lock(accessMutex);
  return authBlob;
}

bell::Result<std::vector<uint8_t>> LoginBlob::decodeZeroconfBlob(
    const std::vector<uint8_t>& blob, const std::vector<uint8_t>& clientKey) {
  std::scoped_lock lock(accessMutex);
  // 0:16 - iv; 17:-20 - blob; -20:0 - checksum
  auto iv = std::span(blob.data(), 16);
  auto encryptedData = std::span(blob.data() + 16, blob.size() - 20 - 16);
  auto checksum = std::span(blob.data() + blob.size() - 20, 20);

  std::array<uint8_t, 96> sharedKey{};
  // Calculate the shared key
  dhPair.computeSharedKey(clientKey.data(), clientKey.size(), sharedKey.data());

  // Base key = sha1(sharedKey) 0:16
  std::vector<uint8_t> baseKey(20);
  sha1Context.getDigest(reinterpret_cast<const uint8_t*>(sharedKey.data()),
                        sharedKey.size(), baseKey.data());
  // Only use the first 16 bytes
  baseKey.resize(16);

  std::string checksumMessage = "checksum";
  std::vector<uint8_t> checksumKey(20);
  // Calculate the checksum hmac
  sha1Context.getHmac(baseKey.data(), baseKey.size(),
                      reinterpret_cast<const uint8_t*>(checksumMessage.data()),
                      checksumMessage.size(), checksumKey.data());

  std::string encryptionMessage = "encryption";
  std::vector<uint8_t> encryptionKey(20);
  // Calculate the encryption hmac
  sha1Context.getHmac(
      baseKey.data(), baseKey.size(),
      reinterpret_cast<const uint8_t*>(encryptionMessage.data()),
      encryptionMessage.size(), encryptionKey.data());

  std::vector<uint8_t> mac(20);
  // Calculate the mac
  sha1Context.getHmac(checksumKey.data(), checksumKey.size(),
                      reinterpret_cast<const uint8_t*>(encryptedData.data()),
                      encryptedData.size(), mac.data());

  if (!std::equal(mac.begin(), mac.end(), checksum.begin())) {
    BELL_LOG(error, LOG_TAG, "Encryption and checksum keys do not match");
    return std::errc::bad_message;
  }

  // Initialize the aes context
  mbedtls_aes_context aesCtx;
  mbedtls_aes_init(&aesCtx);

  // needed for AES internal cache
  size_t off = 0;
  std::array<uint8_t, 16> streamBlock;

  // set IV
  if (mbedtls_aes_setkey_enc(&aesCtx, encryptionKey.data(), 128) != 0) {
    BELL_LOG(error, LOG_TAG, "Failed to set AES key");
    mbedtls_aes_free(&aesCtx);
    return std::errc::bad_message;
  }

  std::array<uint8_t, 16> nounceCounter;
  std::copy(iv.begin(), iv.end(), nounceCounter.begin());

  std::vector<uint8_t> authBlob;
  authBlob.resize(encryptedData.size());

  // Perform decrypt
  if (mbedtls_aes_crypt_ctr(
          &aesCtx, encryptedData.size(), &off, nounceCounter.data(),
          streamBlock.data(),
          reinterpret_cast<const uint8_t*>(encryptedData.data()),
          authBlob.data()) != 0) {
    BELL_LOG(error, LOG_TAG, "Failed to aes decrypt auth data");
    mbedtls_aes_free(&aesCtx);
    return std::errc::bad_message;
  }

  mbedtls_aes_free(&aesCtx);

  BELL_LOG(info, LOG_TAG, "Decoded auth data: {}",
           std::string(authBlob.begin(), authBlob.end()));

  return authBlob;
}

bell::Result<> LoginBlob::decodeEncryptedAuthBlob(
    const std::string& username,
    const std::vector<uint8_t>& encryptedAuthBlob) {
  std::scoped_lock lock(accessMutex);

  std::vector<uint8_t> base64DecodedAuthData;
  auto decodeRes = base64Decode(
      std::string_view(reinterpret_cast<const char*>(encryptedAuthBlob.data()),
                       encryptedAuthBlob.size()),
      base64DecodedAuthData);

  if (!decodeRes) {
    BELL_LOG(error, LOG_TAG, "Failed to base64 decode auth blob");
    return decodeRes.getError();
  }

  // Calculate the pbkdf2 hmac
  std::array<uint8_t, 20> deviceIdDigest;
  std::array<uint8_t, 20> pbkdf2Hmac;
  sha1Context.reset();
  sha1Context.getDigest(reinterpret_cast<const uint8_t*>(deviceId.data()),
                        deviceId.size(), deviceIdDigest.data());

  int res = mbedtls_pkcs5_pbkdf2_hmac_ext(
      MBEDTLS_MD_SHA1, deviceIdDigest.data(), deviceIdDigest.size(),
      reinterpret_cast<const uint8_t*>(username.data()), username.size(), 256,
      20, pbkdf2Hmac.data());
  if (res != 0) {
    BELL_LOG(error, LOG_TAG, "Failed to calculate pbkdf2, mbedtls error: {}",
             res);
    return std::errc::bad_message;
  }

  // Calculate the sha1 hmac
  std::array<uint8_t, 20 + 4> baseKeyHashed{};
  // First 4 bytes are the length of the base key, which is 20 bytes
  // The rest is the sha1 hash of the base key
  baseKeyHashed[23] = 0x14;

  sha1Context.reset();
  sha1Context.getDigest(pbkdf2Hmac.data(), pbkdf2Hmac.size(),
                        baseKeyHashed.data());

  // Initialize the aes context
  mbedtls_aes_context aesCtx;
  mbedtls_aes_init(&aesCtx);

  // Set the key
  if (mbedtls_aes_setkey_dec(&aesCtx, baseKeyHashed.data(), 192) != 0) {
    BELL_LOG(error, LOG_TAG, "Failed to set AES key");
    mbedtls_aes_free(&aesCtx);
    return std::errc::bad_message;
  }

  for (uint64_t x = 0; x < base64DecodedAuthData.size() / 16; x++) {
    // Temporary buffer for decrypted 16 bytes
    std::array<uint8_t, 16> decryptedChunk{};

    if (mbedtls_aes_crypt_ecb(&aesCtx, MBEDTLS_AES_DECRYPT,
                              &base64DecodedAuthData[x * 16],
                              decryptedChunk.data()) != 0) {
      BELL_LOG(error, LOG_TAG, "Failed to aes decrypt auth data");
      mbedtls_aes_free(&aesCtx);
      return std::errc::bad_message;
    }

    std::copy(decryptedChunk.begin(), decryptedChunk.end(),
              &base64DecodedAuthData[x * 16]);
  }

  mbedtls_aes_free(&aesCtx);

  auto l = base64DecodedAuthData.size();
  for (size_t i = 0; i < l - 16; i++) {
    base64DecodedAuthData[l - i - 1] ^= base64DecodedAuthData[l - i - 17];
  }

  bell::io::IMemoryStream blobMemoryStream(
      reinterpret_cast<const std::byte*>(base64DecodedAuthData.data()),
      base64DecodedAuthData.size());

  // Construct the binary stream reader
  bell::io::BinaryStream blobBinaryStream(&blobMemoryStream);

  blobBinaryStream.skip(1);  // Skip the first byte
  // Skip the next uvarint of bytes
  blobBinaryStream.skip(readUvarint(blobBinaryStream) + 1);

  authType = readUvarint(blobBinaryStream);
  blobBinaryStream.skip(1);  // Skip the next byte

  uint32_t authDataSize = readUvarint(blobBinaryStream);
  authBlob.resize(authDataSize);
  blobMemoryStream.read(reinterpret_cast<char*>(authBlob.data()), authDataSize);

  BELL_LOG(info, LOG_TAG, "Auth type: {}", authType);
  BELL_LOG(info, LOG_TAG, "Auth blob len: {}", authBlob.size());

  return {};
}

uint32_t LoginBlob::getAuthType() {
  std::scoped_lock lock(accessMutex);
  return authType;
}

uint32_t LoginBlob::readUvarint(bell::io::BinaryStream& stream) {
  uint8_t lo;
  stream >> lo;
  if ((int)(lo & 0x80) == 0) {
    return lo;
  }

  uint8_t hi;
  stream >> hi;

  return (uint32_t)((lo & 0x7f) | (hi << 7));
}

bell::Result<> LoginBlob::base64Decode(std::string_view encoded,
                                       std::vector<uint8_t>& targetBuffer) {
  size_t outputSize = 0;
  int res = mbedtls_base64_decode(
      nullptr, 0, &outputSize, reinterpret_cast<const uint8_t*>(encoded.data()),
      encoded.size());
  if (outputSize == 0) {
    return std::errc::bad_message;
  }

  targetBuffer.resize(outputSize);
  res = mbedtls_base64_decode(reinterpret_cast<uint8_t*>(targetBuffer.data()),
                              targetBuffer.size(), &outputSize,
                              reinterpret_cast<const uint8_t*>(encoded.data()),
                              encoded.size());
  if (res != 0) {
    return std::errc::bad_message;
  }

  return {};
}
