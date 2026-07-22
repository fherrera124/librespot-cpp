#include "ALSAAudioSink.h"

#include "BellLogger.h"

namespace {
const char* TAG = "ALSA";
}

ALSAAudioSink::ALSAAudioSink() : Task("", 0, 0, 0) {
  /* Open the PCM device in playback mode */
  pcm = snd_pcm_open(&pcm_handle, PCM_DEVICE, SND_PCM_STREAM_PLAYBACK, 0);
  if (pcm < 0) {
    BELL_LOG(error, TAG, "Can't open \"%s\" PCM device. %s", PCM_DEVICE,
             snd_strerror(pcm));
  }

  /* Allocate parameters object and fill it with default values*/
  snd_pcm_hw_params_alloca(&params);

  snd_pcm_hw_params_any(pcm_handle, params);

  /* Set parameters */
  pcm = snd_pcm_hw_params_set_access(pcm_handle, params,
                                     SND_PCM_ACCESS_RW_INTERLEAVED);
  if (pcm < 0)
    BELL_LOG(error, TAG, "Can't set interleaved mode. %s", snd_strerror(pcm));

  pcm = snd_pcm_hw_params_set_format(pcm_handle, params,
                                     SND_PCM_FORMAT_S16_LE);
  if (pcm < 0)
    BELL_LOG(error, TAG, "Can't set format. %s", snd_strerror(pcm));

  pcm = snd_pcm_hw_params_set_channels(pcm_handle, params, 2);
  if (pcm < 0)
    BELL_LOG(error, TAG, "Can't set channels number. %s", snd_strerror(pcm));
  unsigned int rate = 44100;
  pcm = snd_pcm_hw_params_set_rate_near(pcm_handle, params, &rate, 0);
  if (pcm < 0)
    BELL_LOG(error, TAG, "Can't set rate. %s", snd_strerror(pcm));

  // Microseconds. 4 periods (~80ms buffer) - enough headroom for
  // scheduling/IPC jitter (PipeWire's ALSA compat shim in particular)
  // without letting the decode pipeline race far ahead of real-time
  // playback. Left to their own defaults, some backends pick absurdly
  // deep buffers (seen: ~1024 periods, ~20s) that silently break real-time
  // position tracking.
  unsigned int periodTime = 20000;
  int dir = -1;
  snd_pcm_hw_params_set_period_time_near(pcm_handle, params, &periodTime, &dir);
  unsigned int periods = 4;
  snd_pcm_hw_params_set_periods_near(pcm_handle, params, &periods, NULL);

  /* Write parameters */
  pcm = snd_pcm_hw_params(pcm_handle, params);
  if (pcm < 0)
    BELL_LOG(error, TAG, "Can't set hardware parameters. %s", snd_strerror(pcm));

  /* Resume information */
  BELL_LOG(info, TAG, "PCM name: '%s'", snd_pcm_name(pcm_handle));
  BELL_LOG(info, TAG, "PCM state: %s",
          snd_pcm_state_name(snd_pcm_state(pcm_handle)));

  unsigned int tmp;
  snd_pcm_hw_params_get_channels(params, &tmp);
  BELL_LOG(info, TAG, "channels: %i (%s)", tmp, tmp == 1 ? "mono" : "stereo");

  snd_pcm_hw_params_get_period_time(params, &tmp, NULL);
  BELL_LOG(info, TAG, "period_time = %d", tmp);
  snd_pcm_hw_params_get_period_size(params, &frames, 0);

  this->buff_size = frames * 2 * 2 /* 2 -> sample size */;
  BELL_LOG(info, TAG, "required buff_size: %d", buff_size);
  this->startTask();
}

ALSAAudioSink::~ALSAAudioSink() {
  stopAndWait();
  // Draining a stream that was opened but never started (still PREPARED)
  // hangs forever under PipeWire's ALSA compat shim.
  if (snd_pcm_state(pcm_handle) == SND_PCM_STATE_RUNNING) {
    snd_pcm_drain(pcm_handle);
  }
  snd_pcm_close(pcm_handle);
}

void ALSAAudioSink::runTask() {
  std::unique_ptr<std::vector<uint8_t>> dataPtr;
  while (!shouldStop()) {
    // Only this thread touches pcm_handle - flush() just raises this
    // flag instead of calling snd_pcm_drop()/prepare() itself.
    if (flushRequested.exchange(false)) {
      snd_pcm_drop(pcm_handle);
      snd_pcm_prepare(pcm_handle);
      continue;
    }
    if (!this->ringbuffer.pop(dataPtr)) {
      usleep(100);
      continue;
    }
    pcm = snd_pcm_writei(pcm_handle, dataPtr->data(), this->frames);
    if (pcm == -EPIPE) {
      snd_pcm_prepare(pcm_handle);
    } else if (pcm < 0) {
      BELL_LOG(error, TAG, "Can't write to PCM device. %s", snd_strerror(pcm));
    }
  }
}

void ALSAAudioSink::feedPCMFrames(const uint8_t* buffer, size_t bytes) {
  std::lock_guard<std::mutex> lock(buffMutex);

  buff.insert(buff.end(), buffer, buffer + bytes);
  while (buff.size() > this->buff_size) {
    auto ptr = std::make_unique<std::vector<uint8_t>>(
        this->buff.begin(), this->buff.begin() + this->buff_size);
    this->buff = std::vector<uint8_t>(this->buff.begin() + this->buff_size,
                                      this->buff.end());
    while (!this->ringbuffer.push(ptr)) {
      usleep(100);
    };
  }
}

void ALSAAudioSink::flush() {
  {
    std::lock_guard<std::mutex> lock(buffMutex);
    buff.clear();
  }
  ringbuffer.reset();
  flushRequested = true;
}
