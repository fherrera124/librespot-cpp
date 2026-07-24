#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "bell/io/CircularByteBuffer.h"
#include "bell/utils/Task.h"
#include "driver/i2s_std.h"

namespace cspot {

// ESP-IDF i2s_std sink. feedPCMFrames() writes into a ring buffer;
// a dedicated bell::Task drains it into the I2S peripheral, decoupling
// AudioDecoderImpl's decode loop from I2S write timing. v1: hardcoded
// 44100Hz/2ch/16-bit input (matches what AudioDecoderImpl/TremorVorbisCodec
// actually produce for Spotify's Vorbis streams), always downmixed to
// mono for this board's single-speaker amp - see AudioSinkI2S.cpp.
class AudioSinkI2S : public bell::Task {
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

  // Blocks once the ring buffer is full - the deliberate backpressure
  // that paces AudioDecoderImpl's decode loop against I2S consumption.
  void feedPCMFrames(const uint8_t* data, size_t bytes);

 private:
  const char* LOG_TAG = "AudioSinkI2S";

  Config config;
  i2s_chan_handle_t txChannel = nullptr;
  bell::io::CircularByteBuffer ringBuffer;
  std::vector<int16_t> downmixScratch;

  void taskLoop() override;
};

}  // namespace cspot
