#pragma once

// Standard includes
#include <memory>
#include <optional>

// Library includes
#include "bell/http/Client.h"
#include "bell/io/DataStream.h"
#include "mbedtls/build_info.h"  // for MBEDTLS_VERSION_NUMBER, checked below
// mbedTLS 4.0 moved both of these under mbedtls/private/ (still shipped,
// just relocated). ESP-IDF's own port has a bignum.h wrapper at the
// classic path meant to redirect via #include_next, but its search-path
// continuation doesn't reliably reach tf-psa-crypto/drivers/builtin/include
// in this build (real, reproduced failure) - including both real headers
// directly instead, same as aes.h (which never had a wrapper to begin with).
#if MBEDTLS_VERSION_NUMBER >= 0x04000000
#include "mbedtls/private/aes.h"
#include "mbedtls/private/bignum.h"
#else
#include "mbedtls/aes.h"
#include "mbedtls/bignum.h"
#endif

namespace cspot {

/**
 * @brief DataStream implementation that fetches encrypted audio data from a CDN URL
 *        and decrypts it on-the-fly using AES-128-CTR.
 *
 * This class uses HTTP range requests to fetch data in chunks, decrypts the data using
 * the provided AES key, and exposes a standard DataStream interface for reading the
 * decrypted audio data. It handles alignment, buffering, and tail trimming as needed.
 */
class CDNDataStream : public bell::io::DataStream {
 public:
  explicit CDNDataStream(std::shared_ptr<bell::HTTPClient> httpClient);

  ~CDNDataStream() override;

  bell::Result<> open(const std::string& cdnUrl,
                      const std::vector<std::byte>& decryptKey);

  bool isSeekable() const override;
  bool isInfinite() const override;

  // Returns the trimmed (multiple-of-16) size visible to the caller.
  std::optional<size_t> size() const override;
  size_t position() const override;
  bell::Result<> seek(size_t offset, SeekOrigin origin) override;
  bell::Result<size_t> read(std::byte* outputBuffer,
                            size_t outputBufferLen) override;

 private:
  const char* LOG_TAG = "CDNDataStream";

  // HTTP client handle
  std::shared_ptr<bell::HTTPClient> httpClient;

  // User-visible total size (trimmed to 16-byte boundary). Populated after first range response.
  std::optional<size_t> totalSize;

  // Raw original total size from server (may be non 16-aligned).
  size_t originalTotalSizeRaw = 0;

  // Number of bytes trimmed off the tail (originalTotalSizeRaw % 16).
  size_t tailRemainderBytes = 0;

  // MbedTLS contexts
  mbedtls_aes_context aesCtx{};
  mbedtls_mpi aesIV{};

  // AES decryption key
  std::vector<std::byte> decryptKey;

  // Buffer to store the aligned fetched & decrypted data
  std::vector<std::byte> lastReadChunk;
  size_t bytesInLastReadChunk = 0;

  // Offset inside lastReadChunk where the next readable user-visible byte resides
  size_t chunkStartPosition = 0;

  // Bytes at the tail of lastReadChunk that are decrypted but should not be exposed
  size_t pendingDiscardBack = 0;

  // Current user-visible position (0..*totalSize)
  size_t currentPosition = 0;

  // Position used to update IV increments (aligned to AES block boundary)
  int32_t ivPosition = 0;

  int64_t totalRequestTimeMs = 0;

  // Keeps params of the HTTP request, reused for range-based files
  bell::HTTPRequest httpRequest;
  std::optional<bell::HTTPResponse> activeResponse;

  // Cached buffer coverage for reuse (aligned + visible)
  size_t bufferAlignedStart = 0;  // aligned start (requestStart)
  size_t bufferAlignedEnd =
      0;  // aligned end (exclusive) (requestStart + requestSize)
  size_t bufferVisibleStart = 0;  // user-visible start inside current buffer
  size_t bufferVisibleEnd = 0;    // user-visible end (exclusive)

  // Helper: alignment utilities
  static inline size_t alignDown16(size_t v) { return v & ~size_t(15); }
  static inline size_t alignUp16(size_t v) { return (v + 15) & ~size_t(15); }

  // Plan for a single aligned backend request derived from an arbitrary desired (offset,length).
  struct RangeRequestPlan {
    size_t desiredStart = 0;   // user-visible start
    size_t desiredLength = 0;  // user-visible requested length
    size_t requestStart = 0;   // aligned start sent to server
    size_t requestSize = 0;    // aligned size (multiple of 16) to request
    size_t skipPrefix = 0;     // bytes to discard (front) after decrypt
    size_t skipSuffix = 0;     // bytes to discard (back) after decrypt
  };

  // Compute plan for an aligned backend request (tail handling logic done in implementation)
  static RangeRequestPlan planRange(size_t desiredStart, size_t desiredLength);

  // Fast check if a (offset,length) lies wholly inside current user-visible buffer window.
  bool canReuse(size_t offset, size_t length) const;

  // Requests a new range of data from the server, filling the lastReadChunk buffer.
  // 'offset' is the logical user-visible offset (not necessarily 16-aligned).
  bell::Result<> requestRange(size_t offset, size_t length, SeekOrigin origin);

  // Decrypts 'size' bytes of data starting logically at 'position' (position must be aligned to
  // the actual decrypted block sequence; implementation adapts internal IV as needed).
  bell::Result<> decryptData(std::byte* data, size_t size, size_t position);
};
}  // namespace cspot
