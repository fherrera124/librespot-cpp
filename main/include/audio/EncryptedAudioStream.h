#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "bell/Result.h"
#include "bell/http/Client.h"
#include "mbedtls/aes.h"
#include "mbedtls/bignum.h"

namespace cspot {
class EncryptedAudioStream {
 public:
  EncryptedAudioStream(std::string cdnUrl,
                       const std::vector<uint8_t>& decryptKey);

  ~EncryptedAudioStream();

  size_t getFileSize() const;

  bell::Result<> readBytes(uint8_t* dst, size_t size, size_t offset = 0);

  bool isExpired() const;

  bool isOpen() const;

  void close();

  // Opens a connection to the CDN URL
  bell::Result<> open();

 private:
  const char* LOG_TAG = "EncryptedAudioStream";

  // CDN URL
  std::string cdnUrl;

  // MbedTLS stuff
  mbedtls_aes_context aesCtx{};
  mbedtls_mpi aesIV{};

  // Decrypts the read data from CDN, using the provided audio key & IV
  bell::Result<> decryptData(uint8_t* data, size_t size, size_t position);

  std::unique_ptr<bell::http::Connection> httpConnection;

  // Buffer for http data
  std::vector<char> connectionBuffer;

  // Contains the full size of the file, read from content-range header
  size_t totalContentLength = 0;
  int32_t ivPosition = 0;
};
}  // namespace cspot
