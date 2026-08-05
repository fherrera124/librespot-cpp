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

// targetPrefetchDuration: how much audio the background PrefetchWorker
// tries to keep read ahead of the cursor, in total playback time.
// CDNDataStream's chunk size is fixed (see kCDNChunkSize), so this is
// realized as a per-track chunk count - derived from this duration and
// the format openStream() resolves to (see AudioDecoderImpl's
// bytesPerSecond()) - keeping the actual buffered duration roughly
// constant across audio qualities. 0 disables read-ahead entirely (every
// fetch stays fully synchronous, the same as before read-ahead existed).
std::unique_ptr<AudioDecoder> createAudioDecoder(
    std::shared_ptr<AudioSink> audioSink,
    std::chrono::milliseconds targetPrefetchDuration =
        std::chrono::milliseconds(6500));
}  // namespace cspot
