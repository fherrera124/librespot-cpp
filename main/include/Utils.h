#pragma once

#include <cstdlib>
#include <iostream>
#include <vector>
#include "bell/Logger.h"
#include "bell/Result.h"
#include "mbedtls/base64.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#endif

namespace cspot {

inline void logHeapStatus(const char* tag, const char* label) {
#ifdef ESP_PLATFORM
  size_t internalFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  size_t internalLargest =
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  size_t psramFree = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  BELL_LOG(info, tag,
           "Heap [{}]: internal free={} bytes (largest block={} bytes), "
           "psram free={} bytes",
           label, internalFree, internalLargest, psramFree);
#else
  (void)tag;
  (void)label;
#endif
}

inline bell::Result<std::string> base64Encode(const std::byte* data,
                                              size_t dataSize) {
  std::string outputStr;
  size_t outputSize = 0;
  int res =
      mbedtls_base64_encode(nullptr, 0, &outputSize,
                            reinterpret_cast<const uint8_t*>(data), dataSize);
  if (outputSize == 0) {
    return bell::make_unexpected_errc<std::string>(std::errc::bad_message);
  }

  outputStr.resize(outputSize);
  res = mbedtls_base64_encode(reinterpret_cast<uint8_t*>(outputStr.data()),
                              outputStr.size(), &outputSize,
                              reinterpret_cast<const uint8_t*>(data), dataSize);
  if (res != 0) {
    return bell::make_unexpected_errc<std::string>(std::errc::bad_message);
  }

  // Strip the trailing null character if it exists
  if (!outputStr.empty() && outputStr.back() == '\0') {
    outputStr.pop_back();
  }

  return outputStr;
}

inline void logDataBase64(const std::byte* data, size_t dataSize) {
  auto encoded = base64Encode(data, dataSize);
  if (encoded) {
    std::cout << *encoded << std::endl;
  }
}

inline bell::Result<std::vector<std::byte>> base64Decode(
    std::string_view encoded) {
  std::vector<std::byte> decodedData;
  size_t outputSize = 0;
  int res = mbedtls_base64_decode(
      nullptr, 0, &outputSize, reinterpret_cast<const uint8_t*>(encoded.data()),
      encoded.size());
  if (outputSize == 0) {
    return bell::make_unexpected_errc<std::vector<std::byte>>(
        std::errc::bad_message);
  }

  decodedData.resize(outputSize);
  res = mbedtls_base64_decode(reinterpret_cast<uint8_t*>(decodedData.data()),
                              decodedData.size(), &outputSize,
                              reinterpret_cast<const uint8_t*>(encoded.data()),
                              encoded.size());
  if (res != 0) {
    return bell::make_unexpected_errc<std::vector<std::byte>>(
        std::errc::bad_message);
  }

  return decodedData;
}
};  // namespace cspot
