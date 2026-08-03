#include "audio/ChunkFetcher.h"

#include "bell/Logger.h"
#include "bell/http/Common.h"

using namespace cspot;

namespace {
const char* LOG_TAG = "ChunkFetcher";

// RAII: cancels the claim on idx unless publish (which disarms it) is
// reached first - covers every exit path below, including an exception
// escaping mid-fetch, not just the explicit early returns.
struct ClaimGuard {
  ChunkCache* cache;
  size_t idx;
  bool disarmed = false;

  ~ClaimGuard() {
    if (!disarmed) {
      cache->cancel(idx);
    }
  }
};
}  // namespace

bell::Result<ChunkFetchResult> cspot::fetchDecryptAndPublishChunk(
    CDNRangeFetcher& rangeFetcher, AesCtrCipher& aesCipher,
    ChunkCache& chunkCache, const std::string& cdnUrl, size_t chunkIndex,
    const RangeRequestPlan& plan) {
  ClaimGuard guard{&chunkCache, chunkIndex};
  size_t rangeEnd = plan.requestStart + plan.requestSize - 1;

  auto startTime = std::chrono::steady_clock::now();
  auto fetchRes = rangeFetcher.fetch(
      cdnUrl, fmt::format("bytes={}-{}", plan.requestStart, rangeEnd));
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - startTime);

  if (!fetchRes) {
    BELL_LOG(debug, LOG_TAG,
             "Fetch of chunk {} (bytes={}-{}) failed after {} ms: {}",
             chunkIndex, plan.requestStart, rangeEnd, elapsed.count(),
             fetchRes.error());
    return tl::make_unexpected(fetchRes.error());
  }
  if (fetchRes->data.size() < plan.requestSize) {
    BELL_LOG(debug, LOG_TAG,
             "Fetch of chunk {} (bytes={}-{}) got a short response ({} of "
             "{} bytes) after {} ms",
             chunkIndex, plan.requestStart, rangeEnd, fetchRes->data.size(),
             plan.requestSize, elapsed.count());
    return bell::make_unexpected_errc<ChunkFetchResult>(std::errc::io_error);
  }

  auto decryptRes = aesCipher.decrypt(fetchRes->data.data(), plan.requestSize,
                                      plan.requestStart);
  if (!decryptRes) {
    BELL_LOG(debug, LOG_TAG, "Decrypt failed for chunk {}: {}", chunkIndex,
             decryptRes.error());
    return tl::make_unexpected(decryptRes.error());
  }

  BELL_LOG(debug, LOG_TAG,
           "Fetched and cached chunk {} (bytes={}-{}) in {} ms", chunkIndex,
           plan.requestStart, rangeEnd, elapsed.count());

  size_t totalWireSize = fetchRes->contentRangeTotal;
  chunkCache.publish(chunkIndex, std::move(fetchRes->data));
  guard.disarmed = true;

  auto published = chunkCache.tryGet(chunkIndex);
  if (!published) {
    // Vanishingly unlikely (would need another thread to advanceWindow()/
    // reset() this exact index in the instant between publish() and this
    // tryGet()) but not impossible - report it rather than returning a
    // null data pointer to the caller.
    return bell::make_unexpected_errc<ChunkFetchResult>(std::errc::io_error);
  }

  return ChunkFetchResult{published, elapsed, totalWireSize};
}
