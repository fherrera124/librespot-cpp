#include "audio/ReadAheadPolicy.h"

using namespace cspot;

FixedDepthReadAheadPolicy::FixedDepthReadAheadPolicy(size_t depth)
    : depth(depth) {}

std::vector<size_t> FixedDepthReadAheadPolicy::chunksToPrefetch(
    size_t currentChunkIndex, std::optional<size_t> totalChunks) const {
  std::vector<size_t> result;
  result.reserve(depth);

  for (size_t i = 1; i <= depth; ++i) {
    size_t idx = currentChunkIndex + i;
    if (totalChunks && idx >= *totalChunks) {
      break;
    }
    result.push_back(idx);
  }

  return result;
}
