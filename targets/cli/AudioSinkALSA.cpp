#include "AudioSinkALSA.h"

#include <vector>

#include "bell/Logger.h"

using namespace cspot;

namespace {
constexpr const char* kPcmDevice = "default";
constexpr unsigned int kChannels = 2;
constexpr size_t kFrameBytes = kChannels * sizeof(int16_t);
// ~740ms of headroom at 44.1kHz/16-bit stereo - same margin
// AudioSinkI2S gives itself against decode/HTTP jitter.
constexpr size_t kRingBufferBytes = 256 * 1024;
}  // namespace

AudioSinkALSA::AudioSinkALSA()
    : bell::Task("cli_alsa_sink", 0), RingBufferedAudioSink(kRingBufferBytes) {
  int err = snd_pcm_open(&pcmHandle, kPcmDevice, SND_PCM_STREAM_PLAYBACK, 0);
  if (err < 0) {
    BELL_LOG(error, LOG_TAG, "Can't open \"{}\" PCM device: {}", kPcmDevice,
             snd_strerror(err));
    return;
  }

  snd_pcm_hw_params_t* hwParams;
  snd_pcm_hw_params_alloca(&hwParams);
  snd_pcm_hw_params_any(pcmHandle, hwParams);

  snd_pcm_hw_params_set_access(pcmHandle, hwParams,
                               SND_PCM_ACCESS_RW_INTERLEAVED);
  snd_pcm_hw_params_set_format(pcmHandle, hwParams, SND_PCM_FORMAT_S16_LE);
  snd_pcm_hw_params_set_channels(pcmHandle, hwParams, kChannels);
  unsigned int rate = 44100;
  snd_pcm_hw_params_set_rate_near(pcmHandle, hwParams, &rate, nullptr);

  // Microseconds. 4 periods (~80ms buffer) - enough headroom for
  // scheduling/IPC jitter (PipeWire's ALSA compat shim in particular)
  // without letting the decode pipeline race far ahead of real-time
  // playback. Left to their own defaults, some backends pick absurdly
  // deep buffers (seen: ~1024 periods, ~20s) that silently break
  // real-time position tracking.
  unsigned int periodTimeUs = 20000;
  int dir = -1;
  snd_pcm_hw_params_set_period_time_near(pcmHandle, hwParams, &periodTimeUs,
                                         &dir);
  unsigned int periods = 4;
  snd_pcm_hw_params_set_periods_near(pcmHandle, hwParams, &periods, nullptr);

  err = snd_pcm_hw_params(pcmHandle, hwParams);
  if (err < 0) {
    BELL_LOG(error, LOG_TAG, "Can't set hardware parameters: {}",
             snd_strerror(err));
    return;
  }

  snd_pcm_hw_params_get_period_size(hwParams, &periodFrames, nullptr);
  BELL_LOG(info, LOG_TAG, "PCM device '{}' ready, period={} frames",
           snd_pcm_name(pcmHandle), periodFrames);

  startTask();
}

AudioSinkALSA::~AudioSinkALSA() {
  stopTask();
  if (pcmHandle) {
    // Draining a stream that was opened but never started (still
    // PREPARED) hangs forever under PipeWire's ALSA compat shim.
    if (snd_pcm_state(pcmHandle) == SND_PCM_STATE_RUNNING) {
      snd_pcm_drain(pcmHandle);
    }
    snd_pcm_close(pcmHandle);
  }
}

void AudioSinkALSA::taskLoop() {
  // Only this thread touches pcmHandle - flush() just clears the ring
  // buffer and raises this flag instead of calling snd_pcm_drop()/
  // prepare() itself.
  if (flushRequested.exchange(false)) {
    snd_pcm_drop(pcmHandle);
    snd_pcm_prepare(pcmHandle);
    return;
  }

  // Always reads a full period's worth of bytes (looping over read(),
  // which may return less than requested per call) so snd_pcm_writei()
  // below never gets handed a partial frame.
  size_t frameBytes = periodFrames * kFrameBytes;
  std::vector<std::byte> chunk(frameBytes);
  size_t filled = 0;
  while (filled < frameBytes) {
    filled += ringBuffer.read(chunk.data() + filled, frameBytes - filled);
  }

  auto written = snd_pcm_writei(pcmHandle, chunk.data(), periodFrames);
  if (written == -EPIPE) {
    snd_pcm_prepare(pcmHandle);
  } else if (written < 0) {
    BELL_LOG(error, LOG_TAG, "Can't write to PCM device: {}",
             snd_strerror(static_cast<int>(written)));
  }
}
