#pragma once

#include <alsa/asoundlib.h>
#include <cstddef>
#include <cstdint>

#include "bell/io/CircularByteBuffer.h"
#include "bell/utils/Task.h"

namespace cspot {

// ALSA PCM sink for the CLI target. feedPCMFrames() writes into a ring
// buffer; a dedicated bell::Task drains it into the ALSA device,
// decoupling AudioDecoderImpl's decode loop from ALSA write timing.
// Hardcoded 44100Hz/2ch/16-bit input, matching what AudioDecoderImpl/
// TremorVorbisCodec produce for Spotify's Vorbis streams - same shape as
// targets/esp32/main/AudioSinkI2S.h.
class AudioSinkALSA : public bell::Task {
 public:
  AudioSinkALSA();
  ~AudioSinkALSA() override;

  // Blocks once the ring buffer is full - the deliberate backpressure
  // that paces AudioDecoderImpl's decode loop against ALSA consumption.
  void feedPCMFrames(const uint8_t* data, size_t bytes);

 private:
  const char* LOG_TAG = "AudioSinkALSA";

  snd_pcm_t* pcmHandle = nullptr;
  snd_pcm_uframes_t periodFrames = 0;
  bell::io::CircularByteBuffer ringBuffer;

  void taskLoop() override;
};

}  // namespace cspot
