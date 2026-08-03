#pragma once

#include <alsa/asoundlib.h>

#include "audio/RingBufferedAudioSink.h"
#include "bell/utils/Task.h"

namespace cspot {

// ALSA PCM sink for the CLI target. Ring buffer/gain/volume/flush state
// comes from RingBufferedAudioSink; this class owns the bell::Task that
// drains it into the ALSA device, decoupling AudioDecoderImpl's decode
// loop from ALSA write timing. Hardcoded 44100Hz/2ch/16-bit input,
// matching what AudioDecoderImpl/TremorVorbisCodec produce for Spotify's
// Vorbis streams - same shape as targets/esp32/main/AudioSinkI2S.h.
class AudioSinkALSA : public bell::Task, public RingBufferedAudioSink {
 public:
  AudioSinkALSA();
  ~AudioSinkALSA() override;

 private:
  const char* LOG_TAG = "AudioSinkALSA";

  snd_pcm_t* pcmHandle = nullptr;
  snd_pcm_uframes_t periodFrames = 0;

  void taskLoop() override;
};

}  // namespace cspot
