#pragma once

// Standard includes
#include <memory>
#include <optional>
#include <string>

// Library includes
#include "audio/AesCtrCipher.h"
#include "audio/CDNRangeFetcher.h"
#include "audio/ChunkCache.h"
#include "audio/ChunkFetcher.h"
#include "audio/PrefetchWorker.h"
#include "audio/RangeAlignment.h"
#include "bell/http/Client.h"
#include "bell/io/DataStream.h"

namespace cspot {

// Size of a "normal" (non-tail) CDN range fetch, in bytes - fixed rather
// than derived from quality/bitrate, so every fetch has the same request
// size/RAM cost regardless of what format gets resolved. The read-ahead
// buffer's duration is kept quality-independent instead via a dynamic
// prefetchDepth (see AudioDecoderImpl.cpp), not by varying this.
constexpr size_t kCDNChunkSize = 32 * 1024;

/**
 * @brief DataStream implementation that fetches encrypted audio data from a CDN URL
 *        and decrypts it on-the-fly using AES-128-CTR.
 *
 * This class uses HTTP range requests to fetch data in chunks, decrypts the data using
 * the provided AES key, and exposes a standard DataStream interface for reading the
 * decrypted audio data. It handles alignment, buffering, and tail trimming as needed.
 *
 * Owns the alignment/windowing/tail-handling policy (what range to ask for,
 * how much of it stays visible to the caller); the actual network fetch and
 * AES decryption are delegated to CDNRangeFetcher and AesCtrCipher.
 *
 * Read-ahead: every "normal" (non-tail) chunkSize-sized fetch is a
 * candidate for the shared PrefetchWorker to have already fetched it into
 * chunkCache - see requestRange()'s own comment on "phase" for how chunk
 * indices are derived from byte offsets without needing a second,
 * independent alignment scheme. A miss (cold start, or a seek that lands
 * outside the current window) always falls back to a plain synchronous
 * fetch - this class is never worse than that baseline, only sometimes
 * faster.
 */
class CDNDataStream : public bell::io::DataStream {
 public:
  CDNDataStream(std::shared_ptr<bell::HTTPClient> httpClient,
               std::shared_ptr<PrefetchWorker> prefetchWorker,
               size_t prefetchDepth);

  // Already non-copyable implicitly (aesCipher is std::optional<AesCtrCipher>,
  // and AesCtrCipher itself deletes its copy ops) - declared explicitly so
  // that's obvious from this class's own declaration, consistent with
  // AesCtrCipher/ChunkCache/PrefetchWorker all doing the same.
  CDNDataStream(const CDNDataStream&) = delete;
  CDNDataStream& operator=(const CDNDataStream&) = delete;

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

  // Fetches and decrypts the raw bytes [0, maxBytes) at the very start of
  // the wire stream - the fixed-size proprietary Spotify header every
  // other public method here (seek/size/position) deliberately hides
  // behind kSpotifyHeaderSize. Only useful to a caller that actually wants
  // to parse that header. Mutates this object's read position/buffer
  // state like any other range fetch - call before any real reading
  // begins.
  bell::Result<std::vector<std::byte>> readRawHeaderBytes(size_t maxBytes);

 private:
  const char* LOG_TAG = "CDNDataStream";

  // Executes the actual HTTP range GETs - no shared mutable state, safe to
  // call repeatedly (or, later, concurrently from another instance
  // pointed at the same cdnUrl/httpClient).
  CDNRangeFetcher rangeFetcher;

  // The CDN URL to fetch ranges against - set in open(), reused for every
  // subsequent range request. shared_ptr (not a plain string) so handing
  // a copy to PrefetchWorker::Session on every advancePrefetchWindow()
  // call is a refcount bump, not a string copy.
  std::shared_ptr<const std::string> cdnUrl;

  // Decrypts fetched bytes for the synchronous/foreground path. Deferred
  // construction (needs the decrypt key, only available in open()).
  std::optional<AesCtrCipher> aesCipher;

  // Shared with PrefetchWorker (see below) - a *separate* AesCtrCipher
  // instance (same key) so the worker's background thread never touches
  // the same mbedtls_aes_context as this one.
  std::shared_ptr<AesCtrCipher> prefetchAesCipher;

  // Long-lived, owned by AudioDecoderImpl and shared across every
  // CDNDataStream it opens - never null.
  std::shared_ptr<PrefetchWorker> prefetchWorker;

  // Read-ahead window for this stream's current "phase" - see
  // requestRange()'s comment for what a phase is. Fresh per open()/reset.
  std::shared_ptr<ChunkCache> chunkCache;

  // Size of a "normal" (non-tail) fetch, in bytes - see kCDNChunkSize.
  const size_t chunkSize = kCDNChunkSize;

  // How many chunks ahead of the read cursor PrefetchWorker should try to
  // keep fetched - derived per-track from the resolved format's bitrate
  // (see AudioDecoderImpl.cpp) so the read-ahead buffer's real-world
  // duration stays roughly constant across audio qualities despite
  // chunkSize being fixed.
  const size_t prefetchDepth;

  // The desiredStart (logical) that anchors chunk index 0 of the current
  // phase - unset until the first cacheable (chunkSize-sized, non-tail)
  // fetch of this phase happens. A phase ends (and this resets) whenever
  // a seek misses the current buffer window.
  std::optional<size_t> phaseAnchor;

  // User-visible total size (trimmed to 16-byte boundary). Populated after first range response.
  std::optional<size_t> totalSize;

  // Raw original total size from server (may be non 16-aligned).
  size_t originalTotalSizeRaw = 0;

  // Number of bytes trimmed off the tail (originalTotalSizeRaw % 16).
  size_t tailRemainderBytes = 0;

  // Buffer to store the aligned fetched & decrypted data
  std::vector<std::byte> lastReadChunk;
  size_t bytesInLastReadChunk = 0;

  // Offset inside lastReadChunk where the next readable user-visible byte resides
  size_t chunkStartPosition = 0;

  // Bytes at the tail of lastReadChunk that are decrypted but should not be exposed
  size_t pendingDiscardBack = 0;

  // Current user-visible position (0..*totalSize)
  size_t currentPosition = 0;

  int64_t totalRequestTimeMs = 0;

  // Cached buffer coverage for reuse (aligned + visible)
  size_t bufferAlignedStart = 0;  // aligned start (requestStart)
  size_t bufferAlignedEnd =
      0;  // aligned end (exclusive) (requestStart + requestSize)
  size_t bufferVisibleStart = 0;  // user-visible start inside current buffer
  size_t bufferVisibleEnd = 0;    // user-visible end (exclusive)

  // Resets phaseAnchor/chunkCache - called whenever a seek lands outside
  // the current buffer window (a new phase is about to begin) and from
  // open()/the destructor.
  void resetPrefetchPhase();

  // Chunk index of 'desiredStart' within the current phase, if it's
  // actually reachable from phaseAnchor via whole chunkSize steps (always
  // true for this class's own sequential access pattern - see
  // requestRange()'s comment).
  std::optional<size_t> chunkIndexInPhase(size_t desiredStart) const;

  // Adopts a chunk fetched by PrefetchWorker (or by a previous call to
  // this same class) as the current lastReadChunk, replaying the exact
  // bookkeeping a synchronous fetch would have done. Returns false (never
  // touching any state) if the cached data's size doesn't match what the
  // plan expects, so the caller can fall back to a normal fetch.
  bool adoptCachedChunk(const std::vector<std::byte>& data,
                       const RangeRequestPlan& plan);

  // After confirming chunk 'chunkIndex' (whether from cache or a fresh
  // fetch) evicts everything behind it and asks PrefetchWorker to look
  // further ahead.
  void advancePrefetchWindow(size_t chunkIndex);

  // Tries to serve a cacheable (non-tail, chunkSize-length) request out of
  // chunkCache - the "phase"/claim/wait/fetch logic. Returns true if fully
  // served (read state already updated, including telling PrefetchWorker
  // to look further ahead - caller should return {} immediately). A false
  // return never leaves a claim behind: a genuine miss (window full, an
  // in-flight wait that timed out, or this call's own fetch attempt
  // failing) always resolves its own claim first; the caller's fallback
  // synchronous fetch then runs unclaimed.
  bool tryServeFromCache(size_t desiredStart, size_t desiredLen);

  // Requests a new range of data from the server, filling the lastReadChunk buffer.
  // 'offset' is the logical user-visible offset (not necessarily 16-aligned).
  bell::Result<> requestRange(size_t offset, size_t length, SeekOrigin origin);
};
}  // namespace cspot
