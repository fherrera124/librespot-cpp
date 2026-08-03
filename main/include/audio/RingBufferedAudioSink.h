#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "AudioSink.h"
#include "bell/io/CircularByteBuffer.h"

namespace cspot {

// Shared ring-buffer + Q15 gain + volume/flush state machine for
// ring-buffer-backed PCM sinks. Concrete sinks (AudioSinkI2S, AudioSinkALSA)
// add their own bell::Task and taskLoop() to drain ringBuffer into their
// hardware - that part stays out of this class since the two backends
// genuinely differ (I2S accepts arbitrary partial writes; ALSA needs
// period-aligned reads), not just in API surface. feedPCMFrames()/
// volumeChanged()/flush() run on the decode thread (AudioDecoderImpl's
// loop); ringBuffer/flushRequested are read back from each subclass's own
// drain thread - see CircularByteBuffer/std::atomic for the sync each
// side relies on.
class RingBufferedAudioSink : public AudioSink {
 public:
  void feedPCMFrames(const uint8_t* data, size_t bytes) override;
  void volumeChanged(uint16_t volume) override;
  void flush() override;

 protected:
  // downmixToMono: stereo pairs are averaged to mono, with gain applied in
  // the same pass, before ever reaching ringBuffer - the ESP32 target's
  // need for boards wired with a single I2S channel. Always false
  // elsewhere (ALSA has no such case today).
  RingBufferedAudioSink(size_t ringBufferBytes, bool downmixToMono = false);

  bell::io::CircularByteBuffer ringBuffer;
  std::atomic<bool> flushRequested{false};

 private:
  std::vector<int16_t> gainScratch;
  // Squared, not the raw 0..1 fraction - see volumeToGain()'s comment.
  std::atomic<float> volumeScale{1.0f};
  bool downmixToMono;
};

}  // namespace cspot
