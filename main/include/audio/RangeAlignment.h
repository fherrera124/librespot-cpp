#pragma once

// Standard includes
#include <cstddef>

namespace cspot {

inline size_t alignDown16(size_t v) {
  return v & ~size_t(15);
}

inline size_t alignUp16(size_t v) {
  return (v + 15) & ~size_t(15);
}

// Plan for a single aligned CDN range request derived from an arbitrary
// desired (offset, length) - Spotify CDN audio is AES-128-CTR encrypted,
// so every request has to land on a 16-byte block boundary; the extra
// bytes this pulls in on either side get discarded (skipPrefix/skipSuffix)
// once decrypted, exposing exactly [desiredStart, desiredStart+desiredLength)
// to the caller.
//
// Shared between CDNDataStream (the synchronous/foreground fetch path)
// and PrefetchWorker (the background read-ahead path): both need to
// derive byte-for-byte identical request/decrypt parameters for the same
// (desiredStart, desiredLength) so a chunk fetched by one can be trusted
// and consumed by the other via ChunkCache.
struct RangeRequestPlan {
  size_t desiredStart = 0;   // user-visible start
  size_t desiredLength = 0;  // user-visible requested length
  size_t requestStart = 0;   // aligned start sent to server
  size_t requestSize = 0;    // aligned size (multiple of 16) to request
  size_t skipPrefix = 0;     // bytes to discard (front) after decrypt
  size_t skipSuffix = 0;     // bytes to discard (back) after decrypt
};

inline RangeRequestPlan planRange(size_t desiredStart, size_t desiredLength) {
  RangeRequestPlan plan;
  plan.desiredStart = desiredStart;
  plan.desiredLength = desiredLength;

  plan.requestStart = alignDown16(desiredStart);
  plan.skipPrefix = desiredStart - plan.requestStart;

  size_t totalVisible = plan.skipPrefix + desiredLength;
  plan.requestSize = alignUp16(totalVisible);
  plan.skipSuffix = plan.requestSize - totalVisible;

  return plan;
}

}  // namespace cspot
