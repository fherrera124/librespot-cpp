#ifndef BUFFEREDAUDIOSINK_H
#define BUFFEREDAUDIOSINK_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>
#include "AudioSink.h"
#include "CircularBuffer.h"
#include "driver/i2s_std.h"

// Shared base for the ESP32 I2S sinks in this directory. Owns the I2S
// peripheral plus a buffer + dedicated feeder task that decouples
// feedPCMFrames() callers from blocking I2S writes. Also does software
// volume scaling and an optional stereo->mono downmix, shared by every
// sink that inherits this.
class BufferedAudioSink : public AudioSink {
 public:
  struct Config {
    int port = I2S_NUM_0;
    gpio_num_t bclkPin = GPIO_NUM_15;
    gpio_num_t wsPin = GPIO_NUM_16;
    gpio_num_t doutPin = GPIO_NUM_17;
    gpio_num_t mclkPin = I2S_GPIO_UNUSED;

    // true for a single-speaker mono I2S amp - downmixes stereo PCM to
    // mono before writing. false for a real stereo DAC.
    bool monoOutput = false;
  };

  void feedPCMFrames(const uint8_t* buffer, size_t bytes) override;
  bool setParams(uint32_t sampleRate, uint8_t channelCount,
                 uint8_t bitDepth) override;
  void volumeChanged(uint16_t volume) override;
  // Just raises a flag - the actual buffer/I2S drain happens on
  // i2sFeedTask()'s own thread, the only reader of dataBuffer.
  void flush() override;

 protected:
  // Derived sinks call this from their constructor once they've decided
  // on a Config (and done any codec-specific init of their own, e.g. an
  // I2C DAC/amp control interface).
  void initI2sChannel(const Config& config);
  void startI2sFeed(size_t buf_size = 256 * 1024);
  void feedPCMFramesInternal(const void* pvItem, size_t xItemSize);

 private:
  static void i2sFeedTask(void* pvParameters);
  void applyStdConfig();
  bool shouldDownmixToMono() const;

  Config sinkConfig;
  i2s_chan_handle_t txChannel = nullptr;
  std::unique_ptr<bell::CircularBuffer> dataBuffer;
  std::atomic<bool> flushRequested{false};
  // TEMP DIAGNOSTIC (occasional audible click investigation, 2026-07-22):
  // remove once resolved. Logs once per underrun episode instead of every
  // 100ms poll - i2sFeedTask()'s own comment.
  bool starvedLogged = false;

  uint32_t sampleRate = 44100;
  uint8_t channelCount = 2;
  uint8_t bitDepth = 16;

  float volumeScale = 1.0f;
  std::vector<int16_t> scaleBuffer;
  std::vector<int16_t> downmixBuffer;
};

#endif
