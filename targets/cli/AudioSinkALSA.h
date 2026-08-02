#pragma once

#include <alsa/asoundlib.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "AudioSink.h"
#include "bell/io/CircularByteBuffer.h"
#include "bell/utils/Task.h"

namespace cspot {

// ALSA PCM sink for the CLI target. feedPCMFrames() writes into a ring
// buffer; a dedicated bell::Task drains it into the ALSA device,
// decoupling AudioDecoderImpl's decode loop from ALSA write timing.
// Hardcoded 44100Hz/2ch/16-bit input, matching what AudioDecoderImpl/
// TremorVorbisCodec produce for Spotify's Vorbis streams - same shape as
// targets/esp32/main/AudioSinkI2S.h.
class AudioSinkALSA : public bell::Task, public AudioSink {
 public:
  AudioSinkALSA();
  ~AudioSinkALSA() override;

  // Blocks once the ring buffer is full - the deliberate backpressure
  // that paces AudioDecoderImpl's decode loop against ALSA consumption.
  void feedPCMFrames(const uint8_t* data, size_t bytes) override;

  // No ALSA mixer control on this target - applied in software, same Q15
  // gain as AudioSinkI2S (see PCMGain.h). volumeScale is atomic since
  // feedPCMFrames() (a different thread, AudioDecoderImpl's decode loop)
  // reads it on every call.
  void volumeChanged(uint16_t volume) override;

  // Callable from any thread - only clears the ring buffer and raises
  // flushRequested; the actual snd_pcm_drop()/prepare() calls happen on
  // taskLoop()'s own thread (only it may touch pcmHandle).
  void flush() override;

 private:
  const char* LOG_TAG = "AudioSinkALSA";

  snd_pcm_t* pcmHandle = nullptr;
  snd_pcm_uframes_t periodFrames = 0;
  bell::io::CircularByteBuffer ringBuffer;
  std::vector<int16_t> gainScratch;
  // Squared, not the raw 0..1 fraction - see volumeToGain()'s comment.
  std::atomic<float> volumeScale{1.0f};
  std::atomic<bool> flushRequested{false};

  void taskLoop() override;
};

}  // namespace cspot
