#include <Authenticator.h>

#include <string_view>

#include <bell/Logger.h>
#include <bell/io/BinaryStream.h>
#include <bell/io/MemoryStream.h>
#include <mbedtls/base64.h>
#include <mbedtls/build_info.h>  // for MBEDTLS_VERSION_NUMBER, checked below
// mbedTLS 4.0 moved these under mbedtls/private/ (still shipped, just
// relocated - see MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS in
// targets/esp32/main/CMakeLists.txt for why they're still usable at all).
#if MBEDTLS_VERSION_NUMBER >= 0x04000000
#include <mbedtls/private/aes.h>
#include <mbedtls/private/pkcs5.h>
#else
#include <mbedtls/aes.h>
#include <mbedtls/pkcs5.h>
#endif
#include <tao/json.hpp>
#include "authentication.pb.h"
#include "bell/Result.h"
#include "bell/net/URIParser.h"
#include "proto/AuthenticationPb.h"
#include "tl/expected.hpp"

using namespace cspot;

namespace {

const std::string protocolVersion = "2.7.1";
const std::string swVersion = "2.0.0";
const std::string brandName = "cspot";
const std::string deviceType = "SPEAKER";
}  // namespace

Authenticator::Authenticator() {
  // Cache it, so we don't have to recalculate it
  dhPublicKey = dhPair.getPublicKeyBase64();
}

std::string Authenticator::buildZeroconfJSONResponse(
    std::string_view deviceName, std::string_view deviceId,
    std::string_view activeUser) {
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
  obj["activeUser"] = activeUser;

  return tao::json::to_string(obj);
}

bell::Result<cspot_proto::LoginCredentials>
Authenticator::authenticateZeroconfQuery(
    std::string_view deviceId,
    const std::unordered_map<std::string, std::string>& queryParams) {
  if (!queryParams.contains("blob") || queryParams.at("blob").empty()) {
    BELL_LOG(error, LOG_TAG, "Blob not found in query string");
    return bell::make_unexpected_errc<cspot_proto::LoginCredentials>(
        std::errc::bad_message);
  }

  if (!queryParams.contains("deviceId") || queryParams.at("deviceId").empty()) {
    BELL_LOG(error, LOG_TAG, "Device ID not found in query string");
    return bell::make_unexpected_errc<cspot_proto::LoginCredentials>(
        std::errc::bad_message);
  }

  if (!queryParams.contains("clientKey") ||
      queryParams.at("clientKey").empty()) {
    BELL_LOG(error, LOG_TAG, "Client key not found in query string");
    return bell::make_unexpected_errc<cspot_proto::LoginCredentials>(
        std::errc::bad_message);
  }
  // Holds base64 decoded blob and client key
  std::vector<std::byte> decodedBlob;
  std::vector<std::byte> decodedClientKey;

  auto res = base64Decode(queryParams.at("blob"), decodedBlob);
  if (!res) {
    BELL_LOG(error, LOG_TAG, "Failed to base64 decode blob");
    return tl::make_unexpected(res.error());
  }

  res = base64Decode(queryParams.at("clientKey"), decodedClientKey);
  if (!res) {
    BELL_LOG(error, LOG_TAG, "Failed to base64 decode client key");
    return tl::make_unexpected(res.error());
  }

  std::string username = queryParams.at("userName");
  auto encryptedAuthBlobRes = decodeZeroconfBlob(decodedBlob, decodedClientKey);
  if (!encryptedAuthBlobRes) {
    BELL_LOG(error, LOG_TAG, "Failed to decode zeroconf blob");
    return tl::make_unexpected(encryptedAuthBlobRes.error());
  }

  // Decode the auth blob
  return decodeEncryptedAuthBlob(deviceId, username, *encryptedAuthBlobRes);
}

bell::Result<cspot_proto::LoginCredentials>
Authenticator::authenticateZeroconfString(std::string_view deviceId,
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
      return bell::make_unexpected_errc<cspot_proto::LoginCredentials>(
          std::errc::invalid_argument);
    }

    // Perform URL decoding
    std::string key =
        bell::net::decodeURLEncoded(queryString.substr(pos, eqPos - pos));
    std::string value = bell::net::decodeURLEncoded(
        queryString.substr(eqPos + 1, nextPos - eqPos - 1));

    queryParams[key] = value;
    pos = nextPos + 1;
  }

  return authenticateZeroconfQuery(deviceId, queryParams);
}

bell::Result<std::vector<std::byte>> Authenticator::decodeZeroconfBlob(
    const std::vector<std::byte>& blob,
    const std::vector<std::byte>& clientKey) {
  std::scoped_lock lock(accessMutex);
  // 0:16 - iv; 17:-20 - blob; -20:0 - checksum
  auto iv = std::span(blob.data(), 16);
  auto encryptedData = std::span(blob.data() + 16, blob.size() - 20 - 16);
  auto checksum = std::span(blob.data() + blob.size() - 20, 20);

  std::array<std::byte, 96> sharedKey{};
  // Calculate the shared key
  dhPair.computeSharedKey(clientKey.data(), clientKey.size(), sharedKey.data());

  // Base key = sha1(sharedKey) 0:16
  std::vector<std::byte> baseKey(20);
  sha1Context.getDigest(sharedKey.data(), sharedKey.size(), baseKey.data());
  // Only use the first 16 bytes
  baseKey.resize(16);

  std::string checksumMessage = "checksum";
  std::vector<std::byte> checksumKey(20);
  // Calculate the checksum hmac
  sha1Context.getHmac(
      baseKey.data(), baseKey.size(),
      reinterpret_cast<const std::byte*>(checksumMessage.data()),
      checksumMessage.size(), checksumKey.data());

  std::string encryptionMessage = "encryption";
  std::vector<std::byte> encryptionKey(20);
  // Calculate the encryption hmac
  sha1Context.getHmac(
      baseKey.data(), baseKey.size(),
      reinterpret_cast<const std::byte*>(encryptionMessage.data()),
      encryptionMessage.size(), encryptionKey.data());

  std::vector<std::byte> mac(20);
  // Calculate the mac
  sha1Context.getHmac(checksumKey.data(), checksumKey.size(),
                      encryptedData.data(), encryptedData.size(), mac.data());

  if (!std::equal(mac.begin(), mac.end(), checksum.begin())) {
    BELL_LOG(error, LOG_TAG, "Encryption and checksum keys do not match");
    return bell::make_unexpected_errc<std::vector<std::byte>>(
        std::errc::bad_message);
  }

  // Initialize the aes context
  mbedtls_aes_context aesCtx;
  mbedtls_aes_init(&aesCtx);

  // needed for AES internal cache
  size_t off = 0;
  std::array<uint8_t, 16> streamBlock{};

  // set IV
  if (mbedtls_aes_setkey_enc(
          &aesCtx, reinterpret_cast<const uint8_t*>(encryptionKey.data()),
          128) != 0) {
    BELL_LOG(error, LOG_TAG, "Failed to set AES key");
    mbedtls_aes_free(&aesCtx);
    return bell::make_unexpected_errc<std::vector<std::byte>>(
        std::errc::bad_message);
  }

  std::array<std::byte, 16> nounceCounter{};
  std::copy(iv.begin(), iv.end(), nounceCounter.begin());

  std::vector<std::byte> authBlob;
  authBlob.resize(encryptedData.size());

  // Perform decrypt
  if (mbedtls_aes_crypt_ctr(
          &aesCtx, encryptedData.size(), &off,
          reinterpret_cast<uint8_t*>(nounceCounter.data()),
          reinterpret_cast<uint8_t*>(streamBlock.data()),
          reinterpret_cast<const uint8_t*>(encryptedData.data()),
          reinterpret_cast<uint8_t*>(authBlob.data())) != 0) {
    BELL_LOG(error, LOG_TAG, "Failed to aes decrypt auth data");
    mbedtls_aes_free(&aesCtx);
    return bell::make_unexpected_errc<std::vector<std::byte>>(
        std::errc::bad_message);
  }

  mbedtls_aes_free(&aesCtx);

  return authBlob;
}

bell::Result<cspot_proto::LoginCredentials>
Authenticator::decodeEncryptedAuthBlob(
    std::string_view deviceId, std::string_view username,
    const std::vector<std::byte>& encryptedAuthBlob) {
  std::scoped_lock lock(accessMutex);

  std::vector<std::byte> base64DecodedAuthData;
  auto decodeRes = base64Decode(
      std::string_view(reinterpret_cast<const char*>(encryptedAuthBlob.data()),
                       encryptedAuthBlob.size()),
      base64DecodedAuthData);

  if (!decodeRes) {
    BELL_LOG(error, LOG_TAG, "Failed to base64 decode auth blob");
    return tl::make_unexpected(decodeRes.error());
  }

  // Calculate the pbkdf2 hmac
  std::array<std::byte, 20> deviceIdDigest{};
  std::array<std::byte, 20> pbkdf2Hmac{};
  sha1Context.reset();
  sha1Context.getDigest(reinterpret_cast<const std::byte*>(deviceId.data()),
                        deviceId.size(), deviceIdDigest.data());

  int res = mbedtls_pkcs5_pbkdf2_hmac_ext(
      MBEDTLS_MD_SHA1, reinterpret_cast<const uint8_t*>(deviceIdDigest.data()),
      deviceIdDigest.size(), reinterpret_cast<const uint8_t*>(username.data()),
      username.size(), 256, 20, reinterpret_cast<uint8_t*>(pbkdf2Hmac.data()));
  if (res != 0) {
    BELL_LOG(error, LOG_TAG, "Failed to calculate pbkdf2, mbedtls error: {}",
             res);
    return bell::make_unexpected_errc<cspot_proto::LoginCredentials>(
        std::errc::bad_message);
  }

  // Calculate the sha1 hmac
  std::array<std::byte, 20 + 4> baseKeyHashed{};
  // First 4 bytes are the length of the base key, which is 20 bytes
  // The rest is the sha1 hash of the base key
  baseKeyHashed[23] = std::byte{0x14};

  sha1Context.reset();
  sha1Context.getDigest(pbkdf2Hmac.data(), pbkdf2Hmac.size(),
                        baseKeyHashed.data());

  // Initialize the aes context
  mbedtls_aes_context aesCtx;
  mbedtls_aes_init(&aesCtx);

  // Set the key
  if (mbedtls_aes_setkey_dec(
          &aesCtx, reinterpret_cast<const uint8_t*>(baseKeyHashed.data()),
          192) != 0) {
    BELL_LOG(error, LOG_TAG, "Failed to set AES key");
    mbedtls_aes_free(&aesCtx);
    return bell::make_unexpected_errc<cspot_proto::LoginCredentials>(
        std::errc::bad_message);
  }

  for (uint64_t x = 0; x < base64DecodedAuthData.size() / 16; x++) {
    // Temporary buffer for decrypted 16 bytes
    std::array<std::byte, 16> decryptedChunk{};

    if (mbedtls_aes_crypt_ecb(
            &aesCtx, MBEDTLS_AES_DECRYPT,
            reinterpret_cast<const uint8_t*>(&base64DecodedAuthData[x * 16]),
            reinterpret_cast<uint8_t*>(decryptedChunk.data())) != 0) {
      BELL_LOG(error, LOG_TAG, "Failed to aes decrypt auth data");
      mbedtls_aes_free(&aesCtx);
      return bell::make_unexpected_errc<cspot_proto::LoginCredentials>(
          std::errc::bad_message);
    }

    std::copy(decryptedChunk.begin(), decryptedChunk.end(),
              &base64DecodedAuthData[x * 16]);
  }

  mbedtls_aes_free(&aesCtx);

  auto l = base64DecodedAuthData.size();
  for (size_t i = 0; i < l - 16; i++) {
    base64DecodedAuthData[l - i - 1] ^= base64DecodedAuthData[l - i - 17];
  }

  bell::io::IMemoryStream blobMemoryStream(base64DecodedAuthData.data(),
                                           base64DecodedAuthData.size());

  // Construct the binary stream reader
  bell::io::BinaryStream blobBinaryStream(&blobMemoryStream);

  blobBinaryStream.skip(1);  // Skip the first byte
  // Skip the next uvarint of bytes
  blobBinaryStream.skip(readUvarint(blobBinaryStream) + 1);

  uint32_t authType = readUvarint(blobBinaryStream);
  blobBinaryStream.skip(1);  // Skip the next byte

  uint32_t authDataSize = readUvarint(blobBinaryStream);
  cspot_proto::LoginCredentials credentials;

  // Construct login credentials
  credentials.authData.resize(authDataSize);
  credentials.username = std::string(username);
  credentials.type = static_cast<AuthenticationType>(authType);
  blobMemoryStream.read(reinterpret_cast<char*>(credentials.authData.data()),
                        authDataSize);

  BELL_LOG(info, LOG_TAG, "Authenticated user {}", credentials.username);
  return credentials;
}

uint32_t Authenticator::readUvarint(bell::io::BinaryStream& stream) {
  uint8_t lo;
  stream >> lo;
  if ((int)(lo & 0x80) == 0) {
    return lo;
  }

  uint8_t hi;
  stream >> hi;

  return (uint32_t)((lo & 0x7f) | (hi << 7));
}

bell::Result<> Authenticator::base64Decode(
    std::string_view encoded, std::vector<std::byte>& targetBuffer) {
  size_t outputSize = 0;
  int res = mbedtls_base64_decode(
      nullptr, 0, &outputSize, reinterpret_cast<const uint8_t*>(encoded.data()),
      encoded.size());
  if (outputSize == 0) {
    return bell::make_unexpected_errc<>(std::errc::bad_message);
  }

  targetBuffer.resize(outputSize);
  res = mbedtls_base64_decode(reinterpret_cast<uint8_t*>(targetBuffer.data()),
                              targetBuffer.size(), &outputSize,
                              reinterpret_cast<const uint8_t*>(encoded.data()),
                              encoded.size());
  if (res != 0) {
    return bell::make_unexpected_errc<>(std::errc::bad_message);
  }

  return {};
}
