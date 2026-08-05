#pragma once

// Standard includes
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

// Library includes
#include "audio/AesCtrCipher.h"
#include "audio/CDNRangeFetcher.h"
#include "audio/ChunkCache.h"
#include "audio/ReadAheadPolicy.h"
#include "bell/http/Client.h"
#include "bell/utils/Task.h"

namespace cspot {

/**
 * @brief Background read-ahead fetcher for CDN audio chunks.
 *
 * One long-lived instance, owned by AudioDecoderImpl and reused across
 * tracks (like AudioSinkI2S's own bell::Task) - spinning up a FreeRTOS
 * task per track would be wasteful. Deliberately knows nothing about
 * CDNDataStream: each requestPrefetch() call is a self-contained
 * description of what to fetch and where to publish it (a shared
 * ChunkCache), so there's no lifetime coupling to worry about - if the
 * CDNDataStream that issued a request is gone by the time this worker
 * gets to it, the ChunkCache it publishes into (kept alive by the
 * shared_ptr in the Session below) simply has no reader left, and the
 * work is silently wasted rather than unsafe.
 *
 * Every fetch/decrypt here uses its own CDNRangeFetcher/AesCtrCipher,
 * entirely separate from whatever CDNDataStream's own synchronous
 * (foreground) path is using - see AesCtrCipher's own header comment for
 * why an mbedtls_aes_context is never shared across threads here.
 */
class PrefetchWorker : public bell::Task {
 public:
  // Describes one track's (or one seek-phase's) prefetchable range - see
  // CDNDataStream's own comment on "phase" for what establishes a new one.
  struct Session {
    std::shared_ptr<ChunkCache> chunkCache;
    // shared_ptr, not a plain string: this Session gets copied on every
    // requestPrefetch() call (roughly once per chunk confirmed) - a
    // shared_ptr copy is a refcount bump, not a reallocation of the URL.
    std::shared_ptr<const std::string> cdnUrl;
    std::shared_ptr<AesCtrCipher> aesCipher;
    size_t chunkSize = 0;
    // The desiredStart (logical, not wire-aligned) of chunk index 0 for
    // this phase - chunk n's desiredStart is phaseAnchor + n*chunkSize.
    size_t phaseAnchor = 0;
    // Raw/wire total size of the stream, as parsed from Content-Range -
    // bounds how many chunks actually exist.
    size_t totalWireSize = 0;
    // How many chunks ahead of currentChunkIndex to prefetch - see
    // CDNDataStream::prefetchDepth's own comment for how this is derived.
    size_t depth = 0;
  };

  explicit PrefetchWorker(std::shared_ptr<bell::HTTPClient> httpClient);
  ~PrefetchWorker() override;

  PrefetchWorker(const PrefetchWorker&) = delete;
  PrefetchWorker& operator=(const PrefetchWorker&) = delete;

  // Cheap, non-blocking: records the latest (session, currentChunkIndex)
  // and wakes the worker. Called from the foreground/player thread every
  // time it confirms a new chunk - if the worker is still busy with a
  // previous request when a newer one arrives, the newer one simply
  // replaces it (no point prefetching from a cursor position playback has
  // already moved past).
  void requestPrefetch(Session session, size_t currentChunkIndex);

 protected:
  void taskLoop() override;
  void wakeTask() override;

 private:
  const char* LOG_TAG = "PrefetchWorker";

  CDNRangeFetcher rangeFetcher;

  std::mutex mutex_;
  std::condition_variable cv_;
  std::optional<Session> pendingSession;
  size_t pendingChunkIndex = 0;
  // Set (and cv_ notified) from wakeTask(), which stopTask() calls only
  // after taskRunning is already false - see wakeTask()'s own comment.
  bool shuttingDown = false;
};

}  // namespace cspot
