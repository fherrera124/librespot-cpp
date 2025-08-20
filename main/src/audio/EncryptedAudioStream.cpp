// #include "audio/EncryptedAudioStream.h"
// #include <iostream>
// #include <optional>
// #include "bell/Logger.h"
// #include "mbedtls/aes.h"
// #include "mbedtls/bignum.h"

// using namespace cspot;

// namespace {
// // Base IV for AES decryption, incremented per block
// const std::array<uint8_t, 16> aesIVBase = {
//     0x72, 0xe0, 0x67, 0xfb, 0xdd, 0xcb, 0xcf, 0x77,
//     0xeb, 0xe8, 0xbc, 0x64, 0x3f, 0x63, 0x0d, 0x93,
// };

// }  // namespace

// EncryptedAudioStream::EncryptedAudioStream(
//     std::string cdnUrl, const std::vector<std::byte>& decryptKey)
//     : cdnUrl(std::move(cdnUrl)) {
//   if (this->cdnUrl.empty() || decryptKey.empty()) {
//     throw std::invalid_argument("CDN URL and file ID must not be empty");
//   }

//   // Initialize the AES context and IV
//   mbedtls_aes_init(&aesCtx);
//   mbedtls_mpi_init(&aesIV);

//   // Set the AES key for decryption
//   if (mbedtls_aes_setkey_enc(&aesCtx, decryptKey.data(),
//                              decryptKey.size() * 8) != 0) {
//     throw std::runtime_error("Failed to set AES key");
//   }

//   // Set the IV to the base value
//   int mbedtlsRes =
//       mbedtls_mpi_read_binary(&aesIV, aesIVBase.data(), aesIVBase.size());

//   if (mbedtlsRes != 0) {
//     BELL_LOG(error, LOG_TAG, "Failed to initialize AES IV, mbedtls error: {}",
//              mbedtlsRes);
//     throw std::runtime_error("Failed to initialize AES IV");
//   }
// }

// EncryptedAudioStream::~EncryptedAudioStream() {
//   close();

//   // Free the AES context and IV
//   mbedtls_aes_free(&aesCtx);
//   mbedtls_mpi_free(&aesIV);
// }

// bell::Result<> EncryptedAudioStream::open() {
//   // httpConnection = std::make_unique<bell::http::Connection>();

//   // auto res = httpConnection->connect(cdnUrl, 5000);
//   // if (!res) {
//   //   return res;
//   // }

//   return {};
// }

// bell::Result<> EncryptedAudioStream::decryptData(uint8_t* data, size_t size,
//                                                  size_t position) {
//   if (size == 0) {
//     return {};
//   }

//   if (size % 16 != 0) {
//     BELL_LOG(error, LOG_TAG, "Data size must be a multiple of 16 bytes");
//     return bell::make_unexpected_errc(std::errc::bad_message);
//   }

//   int32_t positionOffset = static_cast<int32_t>(position) - ivPosition;

//   // Add the position offset to the base IV
//   int res = mbedtls_mpi_add_int(&aesIV, &aesIV, positionOffset / 16);
//   if (res != 0) {
//     BELL_LOG(error, LOG_TAG, "Failed to update AES IV, mbedtls error: {}", res);
//     return bell::make_unexpected_errc(std::errc::bad_message);
//   }

//   ivPosition = static_cast<int32_t>(position);

//   std::array<uint8_t, 16> ivData{};
//   res = mbedtls_mpi_write_binary(&aesIV, ivData.data(), ivData.size());
//   if (res != 0) {
//     BELL_LOG(error, LOG_TAG,
//              "Failed to write AES IV to binary, mbedtls error: {}", res);
//     return bell::make_unexpected_errc(std::errc::bad_message);
//   }

//   // Decrypt the data
//   size_t offset = 0;
//   std::array<uint8_t, 16> streamBlock;
//   if (mbedtls_aes_crypt_ctr(&aesCtx, size, &offset, ivData.data(),
//                             streamBlock.data(), data, data) != 0) {
//     BELL_LOG(error, LOG_TAG, "Failed to decrypt audio data");
//     return bell::make_unexpected_errc(std::errc::bad_message);
//   }

//   return {};
// }

// size_t EncryptedAudioStream::getFileSize() const {
//   return totalContentLength;
// }

// bool EncryptedAudioStream::isExpired() const {
//   // This method can be used to check if the stream is expired based on some
//   // criteria, e.g., time since last access or a specific expiration time.
//   // For now, we return false as we don't have an expiration mechanism.
//   return false;
// }

// bool EncryptedAudioStream::isOpen() const {
//   // return httpConnection != nullptr;
//   return true;
// }

// void EncryptedAudioStream::close() {
//   // if (httpConnection) {
//   //   httpConnection.reset();
//   // }
//   connectionBuffer.clear();
//   totalContentLength = 0;
//   ivPosition = 0;

//   // Set the IV to the base value
//   int mbedtlsRes =
//       mbedtls_mpi_read_binary(&aesIV, aesIVBase.data(), aesIVBase.size());

//   if (mbedtlsRes != 0) {
//     BELL_LOG(error, LOG_TAG, "Failed to initialize AES IV, mbedtls error: {}",
//              mbedtlsRes);
//     throw std::runtime_error("Failed to initialize AES IV");
//   }
// }

// bell::Result<> EncryptedAudioStream::readBytes(uint8_t* dst, size_t size,
//                                                size_t offset) {
//   if (size == 0) {
//     return {};  // Nothing to read
//   }

//   if (size % 16 != 0) {
//     BELL_LOG(error, LOG_TAG, "Read size must be a multiple of 16 bytes");
//     return bell::make_unexpected_errc(std::errc::bad_message);
//   }

//   if (totalContentLength > 0 && (offset + size > totalContentLength)) {
//     BELL_LOG(error, LOG_TAG,
//              "Read size exceeds the total content length of the stream");
//     return bell::make_unexpected_errc(std::errc::bad_message);
//   }

//   // // Request the range from the CDN
//   // auto res = requestRange(offset, offset + size - 1, dst, size);

//   // if (!res) {
//   //   BELL_LOG(error, LOG_TAG, "Failed to request range from CDN: {}",
//   //            res.error());
//   //   return tl::make_unexpected(res.error());
//   // }
//   // size_t bytesRead = *res;
//   // if (bytesRead != size) {
//   //   BELL_LOG(warn, LOG_TAG,
//   //            "Requested size was {}, but only {} bytes were read", size,
//   //            bytesRead);
//   // }

//   // // Decrypt the data
//   // auto decryptRes = decryptData(dst, bytesRead, offset);
//   // if (!decryptRes) {
//   //   BELL_LOG(error, LOG_TAG, "Failed to decrypt data: {}", decryptRes.error());
//   //   return decryptRes;
//   // }

//   return {};
// }
