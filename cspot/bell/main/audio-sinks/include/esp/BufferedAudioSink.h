#ifndef BUFFEREDAUDIOSINK_H
#define BUFFEREDAUDIOSINK_H

#include <atomic>
#include <cstdint>
#include <vector>
#include "AudioSink.h"
#include "driver/i2s_std.h"
#include "freertos/ringbuf.h"

// Shared base for the ESP32 I2S sinks in this directory. Owns the I2S
// peripheral (driver/i2s_std.h - ported from the legacy driver/i2s.h this
// used before, removed in ESP-IDF v5.4+/v6; see
// docs/spotify_component_analysis.md, finding F51) plus a ring buffer +
// dedicated feeder task that decouples feedPCMFrames() callers from
// blocking I2S writes. Software volume scaling and an optional mono
// downmix (for single-speaker mono amps, e.g. the JC3248W535's NS4168)
// happen here too, shared by every sink that inherits this.
class BufferedAudioSink : public AudioSink {
 public:
  struct Config {
    // Plain int, not an enum: this ESP-IDF version's i2s_chan_config_t::id
    // takes a bare port number, unlike the legacy driver.
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
  // See finding F77. Just raises a flag - the actual ring buffer/I2S
  // drain happens on i2sFeedTask()'s own thread, the only one allowed to
  // Receive from dataBuffer (FreeRTOS ring buffers don't support
  // concurrent receivers).
  void flush() override;

 protected:
  // Derived sinks call this from their constructor once they've decided
  // on a Config (and done any codec-specific init of their own, e.g. an
  // I2C DAC/amp control interface - none of the currently-ported sinks
  // need that, but it's why this isn't folded into a single constructor).
  void initI2sChannel(const Config& config);
  // 256KB (~2.9s of mono 44.1kHz/16-bit PCM) - see finding F83.
  void startI2sFeed(size_t buf_size = 256 * 1024);
  void feedPCMFramesInternal(const void* pvItem, size_t xItemSize);

 private:
  static void i2sFeedTask(void* pvParameters);
  void applyStdConfig();
  bool shouldDownmixToMono() const;

  Config sinkConfig;
  i2s_chan_handle_t txChannel = nullptr;
  RingbufHandle_t dataBuffer = nullptr;
  std::atomic<bool> flushRequested{false};

  uint32_t sampleRate = 44100;
  uint8_t channelCount = 2;
  uint8_t bitDepth = 16;

  float volumeScale = 1.0f;
  std::vector<int16_t> scaleBuffer;
  std::vector<int16_t> downmixBuffer;
};

#endif
