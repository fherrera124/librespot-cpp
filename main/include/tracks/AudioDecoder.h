#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "bell/Result.h"
#include "proto/SpotifyId.h"
#include "tcb/span.hpp"

namespace cspot {
using AudioOutputCallback = std::function<void(
    tcb::span<const std::byte> pcmBytes, const SpotifyId& id)>;
class AudioDecoder {
 public:
  virtual ~AudioDecoder() = default;

  virtual bell::Result<> openStream(
      const std::string& cdnUrl, const std::vector<std::byte>& decryptKey,
      const SpotifyId& trackId) = 0;

  virtual void processPacket() = 0;

  virtual bool isOpen() const = 0;

  virtual void resetStream() = 0;

  virtual bool isEOF() const = 0;

  // Seeks the currently open stream to an absolute position. No-op-safe
  // to call only while isOpen() - callers must check first.
  virtual bell::Result<> seekToMs(int64_t positionMs) = 0;
};

std::unique_ptr<AudioDecoder> createAudioDecoder(
    AudioOutputCallback outputCallback);
}  // namespace cspot
