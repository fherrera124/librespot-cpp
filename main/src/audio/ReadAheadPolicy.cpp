#include "audio/ReadAheadPolicy.h"

using namespace cspot;

std::vector<size_t> cspot::chunksToPrefetch(size_t currentChunkIndex,
                                            size_t depth,
                                            std::optional<size_t> totalChunks) {
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
