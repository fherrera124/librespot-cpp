#pragma once

#include "audio/RingBufferedAudioSink.h"
#include "bell/utils/Task.h"
#include "driver/i2s_std.h"

namespace cspot {

// ESP-IDF i2s_std sink. Ring buffer/gain/volume/flush state comes from
// RingBufferedAudioSink; this class owns the bell::Task that drains it
// into the I2S peripheral, decoupling AudioDecoderImpl's decode loop from
// I2S write timing. Input is fixed at 44100Hz/2ch/16-bit; downmixed to
// mono in software when Config::monoOutput is set (see the base class'
// downmixToMono).
class AudioSinkI2S : public bell::Task, public RingBufferedAudioSink {
 public:
  struct Config {
    int port = I2S_NUM_0;
    gpio_num_t bclkPin = GPIO_NUM_NC;
    gpio_num_t wsPin = GPIO_NUM_NC;
    gpio_num_t doutPin = GPIO_NUM_NC;
    gpio_num_t mclkPin = I2S_GPIO_UNUSED;
    bool monoOutput = true;
  };

  explicit AudioSinkI2S(const Config& config);
  ~AudioSinkI2S() override;

 private:
  const char* LOG_TAG = "AudioSinkI2S";

  Config config;
  i2s_chan_handle_t txChannel = nullptr;

  void taskLoop() override;
};

}  // namespace cspot
