#pragma once

// Standard includes
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

// Library includes
#include "audio/AesCtrCipher.h"
#include "audio/CDNRangeFetcher.h"
#include "audio/ChunkCache.h"
#include "audio/RangeAlignment.h"
#include "bell/Result.h"

namespace cspot {

struct ChunkFetchResult {
  std::shared_ptr<const std::vector<std::byte>> data;
  std::chrono::milliseconds elapsed{0};
  // Raw/wire total size of the whole stream, as parsed from this
  // response's Content-Range - every fetch reports this (it's static for
  // a given track), not just the first one. Callers that only learn the
  // total size from whichever fetch happens to run first (like
  // CDNDataStream, which otherwise depends on a header/tail probe having
  // already run) need this to stay correct even when the very first
  // fetch for a stream turns out to be a chunk-cache one.
  size_t totalWireSize = 0;
};

/**
 * @brief Fetches, decrypts, and publishes exactly one already-claimed
 *        chunk into chunkCache.
 *
 * Caller must have already called chunkCache.claim(chunkIndex) and gotten
 * MustFetch back. This always resolves that claim one way or another -
 * publish() on success, cancel() on any failure (including an exception
 * escaping partway through, guarded internally) - so the caller never
 * needs its own cleanup for this specific call.
 *
 * Shared by CDNDataStream's synchronous "I have to fetch this myself"
 * path and PrefetchWorker's background loop - both need this exact
 * sequence once they own a claim, just with their own independent
 * CDNRangeFetcher/AesCtrCipher (see AesCtrCipher's own header comment for
 * why those are never shared across threads).
 */
bell::Result<ChunkFetchResult> fetchDecryptAndPublishChunk(
    CDNRangeFetcher& rangeFetcher, AesCtrCipher& aesCipher,
    ChunkCache& chunkCache, const std::string& cdnUrl, size_t chunkIndex,
    const RangeRequestPlan& plan);

}  // namespace cspot
