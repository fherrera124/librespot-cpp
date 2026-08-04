#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "AudioSink.h"
#include "bell/Result.h"
#include "proto/MetadataPb.h"
#include "proto/SpotifyId.h"

namespace cspot {
class AudioDecoder {
 public:
  virtual ~AudioDecoder() = default;

  virtual bell::Result<> openStream(
      const std::string& cdnUrl, const std::vector<std::byte>& decryptKey,
      const SpotifyId& trackId, AudioFormat format) = 0;

  virtual void processPacket() = 0;

  virtual bool isOpen() const = 0;

  virtual void resetStream() = 0;

  virtual bool isEOF() const = 0;

  // Seeks the currently open stream to an absolute position. No-op-safe
  // to call only while isOpen() - callers must check first.
  virtual bell::Result<> seekToMs(int64_t positionMs) = 0;
};

// prefetchDepth: how many chunks the background PrefetchWorker tries to
// keep fetched ahead of the read cursor. 0 disables read-ahead entirely
// (every fetch stays fully synchronous, the same as before read-ahead
// existed) - a real runtime value, not a build flag, so it can be
// tuned/A-B'd without recompiling.
// targetChunkDuration: how many seconds of audio a single "normal"
// (non-tail) CDN range fetch should cover - the actual byte size is
// derived per-track from this and the format openStream() resolves to
// (see CDNDataStream's own comment for what fetch size costs in
// round-trip overhead vs. RAM/prefetch-window duration).
std::unique_ptr<AudioDecoder> createAudioDecoder(
    std::shared_ptr<AudioSink> audioSink, size_t prefetchDepth = 2,
    std::chrono::milliseconds targetChunkDuration =
        std::chrono::milliseconds(6500));
}  // namespace cspot
