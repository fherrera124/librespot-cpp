#pragma once

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>
#include "bell/Logger.h"
#include "fmt/core.h"
#include "mbedtls/base64.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

namespace cspot {

// No-op on the CLI/host build (ESP_PLATFORM only defined under ESP-IDF).
// Free internal DRAM is the scarce resource concurrent TLS connections
// (AP/Dealer WS/SpClient/CDNDataStream) compete over on this board - see
// targets/esp32/sdkconfig.defaults's own comment on hardware AES DMA
// descriptors failing to allocate under exactly that load. Free PSRAM is
// logged alongside it since mbedTLS's own buffers are routed there
// (CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC), to tell "we're out of DRAM" apart
// from "PSRAM itself is the bottleneck" (bus contention, not exhaustion).
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

inline std::string base64Encode(const std::byte* data, size_t dataSize) {
  std::string outputStr;
  size_t outputSize = 0;
  int res =
      mbedtls_base64_encode(nullptr, 0, &outputSize,
                            reinterpret_cast<const uint8_t*>(data), dataSize);
  if (outputSize == 0) {
    throw std::runtime_error(
        fmt::format("Failed to calculate base64 encoded size"));
  }

  outputStr.resize(outputSize);
  res = mbedtls_base64_encode(reinterpret_cast<uint8_t*>(outputStr.data()),
                              outputStr.size(), &outputSize,
                              reinterpret_cast<const uint8_t*>(data), dataSize);
  if (res != 0) {
    throw std::runtime_error("Failed to encode data to base64");
  }

  // Strip the trailing null character if it exists
  if (!outputStr.empty() && outputStr.back() == '\0') {
    outputStr.pop_back();
  }

  return outputStr;
}

inline void logDataBase64(const std::byte* data, size_t dataSize) {
  std::cout << base64Encode(data, dataSize) << std::endl;
}

inline std::vector<std::byte> base64Decode(std::string_view encoded) {
  std::vector<std::byte> decodedData;
  size_t outputSize = 0;
  int res = mbedtls_base64_decode(
      nullptr, 0, &outputSize, reinterpret_cast<const uint8_t*>(encoded.data()),
      encoded.size());
  if (outputSize == 0) {
    throw std::runtime_error("Failed to calculate base64 decoded size");
  }

  decodedData.resize(outputSize);
  res = mbedtls_base64_decode(reinterpret_cast<uint8_t*>(decodedData.data()),
                              decodedData.size(), &outputSize,
                              reinterpret_cast<const uint8_t*>(encoded.data()),
                              encoded.size());
  if (res != 0) {
    throw std::runtime_error("Failed to decode base64 data");
  }

  return decodedData;
}
};  // namespace cspot
