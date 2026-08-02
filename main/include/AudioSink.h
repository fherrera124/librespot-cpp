#pragma once

#include <cstddef>
#include <cstdint>

namespace cspot {

// Platform-specific PCM output device (I2S, ALSA, ...) - the seam between
// this platform-agnostic core and the concrete sink each target owns.
// AudioDecoder, StreamPlayer and ConnectStateHandler only ever see this
// interface, never AudioSinkI2S/AudioSinkALSA directly.
class AudioSink {
 public:
  virtual ~AudioSink() = default;

  // May block to apply backpressure - see the concrete implementation's
  // own comment.
  virtual void feedPCMFrames(const uint8_t* data, size_t bytes) = 0;

  // volume is 0..65535 (connect-state's own range, see
  // ConnectStateHandler).
  virtual void volumeChanged(uint16_t volume) = 0;

  // Discards whatever the sink is currently holding/queued (ring buffer,
  // hardware DMA/FIFO) - see StreamPlayer::taskLoop()'s own comment for why
  // resetting the decoder alone isn't enough.
  virtual void flush() = 0;
};

// Default sink for callers that don't care about audio output (tests,
// metadata-only clients) - every method is a no-op.
class NullAudioSink final : public AudioSink {
 public:
  void feedPCMFrames(const uint8_t*, size_t) override {}
  void volumeChanged(uint16_t) override {}
  void flush() override {}
};

}  // namespace cspot
