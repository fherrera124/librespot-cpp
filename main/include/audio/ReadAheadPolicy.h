#pragma once

// Standard includes
#include <cstddef>
#include <optional>
#include <vector>

namespace cspot {

// Decides which chunk indices should be resident ahead of the one
// currently being consumed - the single tunable extension point for
// read-ahead behavior (depth, adaptive-by-throughput, boosted near EOF,
// etc.) so the fetch/cache machinery around it never needs to change to
// support a new policy.
class ReadAheadPolicy {
 public:
  virtual ~ReadAheadPolicy() = default;

  // Returns the chunk indices that should be prefetched, nearest first.
  // currentChunkIndex is the chunk the reader is consuming right now;
  // totalChunks (if known) bounds how far ahead makes sense near EOF.
  virtual std::vector<size_t> chunksToPrefetch(
      size_t currentChunkIndex, std::optional<size_t> totalChunks) const = 0;
};

// Default policy: a fixed number of chunks ahead of the current one.
class FixedDepthReadAheadPolicy : public ReadAheadPolicy {
 public:
  explicit FixedDepthReadAheadPolicy(size_t depth = 1);

  std::vector<size_t> chunksToPrefetch(
      size_t currentChunkIndex,
      std::optional<size_t> totalChunks) const override;

 private:
  size_t depth;
};

}  // namespace cspot
