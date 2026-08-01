#pragma once

#include <atomic>
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

  // volume is 0..65535 (connect-state's own range, see ConnectStateHandler).
  // Called from whatever thread handles the SetVolumeCommand push -
  // volumeScale is atomic since feedPCMFrames() (a different thread,
  // AudioDecoderImpl's decode loop) reads it on every call.
  void volumeChanged(uint16_t volume);

  // Discards anything queued (ring buffer) and whatever the I2S DMA
  // engine is still holding, so stale audio (e.g. from before a seek)
  // doesn't keep playing. Callable from any thread - only clears the ring
  // buffer and raises flushRequested; the actual channel reset happens on
  // taskLoop()'s own thread (only it may touch txChannel).
  void flush();

 private:
  const char* LOG_TAG = "AudioSinkI2S";

  Config config;
  i2s_chan_handle_t txChannel = nullptr;
  bell::io::CircularByteBuffer ringBuffer;
  std::vector<int16_t> downmixScratch;
  // Squared, not the raw 0..1 fraction - see volumeChanged()'s comment.
  std::atomic<float> volumeScale{1.0f};
  std::atomic<bool> flushRequested{false};

  void taskLoop() override;
};

}  // namespace cspot
