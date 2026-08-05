#pragma once

// Standard includes
#include <cstddef>
#include <optional>
#include <vector>

namespace cspot {

// Chunk indices that should be prefetched ahead of the one currently being
// consumed, nearest first. currentChunkIndex is the chunk the reader is
// consuming right now; depth is how many ahead of it to return; totalChunks
// (if known) bounds how far ahead makes sense near EOF.
std::vector<size_t> chunksToPrefetch(size_t currentChunkIndex, size_t depth,
                                     std::optional<size_t> totalChunks);

}  // namespace cspot
