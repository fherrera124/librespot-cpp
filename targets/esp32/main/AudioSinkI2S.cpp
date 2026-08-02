#include "AudioSinkI2S.h"

#include <algorithm>

#include "audio/PCMGain.h"
#include "bell/Logger.h"
#include "freertos/FreeRTOS.h"

using namespace cspot;

namespace {
// ~3s of headroom at 44.1kHz mono S16 - decouples decode/HTTP jitter from
// the I2S feed.
constexpr size_t kRingBufferBytes = 256 * 1024;
}  // namespace

AudioSinkI2S::AudioSinkI2S(const Config& config)
    : bell::Task("i2s_feed", 4 * 1024, /*espPriority=*/10,
                bell::TaskCore::CoreAny, /*espStackOnPsram=*/false),
      config(config),
      ringBuffer(kRingBufferBytes) {
  i2s_chan_config_t chanConfig =
      I2S_CHANNEL_DEFAULT_CONFIG(config.port, I2S_ROLE_MASTER);
  // 10 descriptors * 1000 frames stays under the DMA
  // descriptor's ~4092-byte-per-buffer limit; auto_clear zeroes a buffer
  // instead of repeating the last one (audible stutter) on underrun.
  chanConfig.dma_desc_num = 10;
  chanConfig.dma_frame_num = 1000;
  chanConfig.auto_clear = true;
  ESP_ERROR_CHECK(i2s_new_channel(&chanConfig, &txChannel, nullptr));

  i2s_std_config_t stdConfig = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
          I2S_DATA_BIT_WIDTH_16BIT,
          config.monoOutput ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO),
      .gpio_cfg =
          {
              .mclk = config.mclkPin,
              .bclk = config.bclkPin,
              .ws = config.wsPin,
              .dout = config.doutPin,
              .din = I2S_GPIO_UNUSED,
              .invert_flags = {.mclk_inv = false,
                               .bclk_inv = false,
                               .ws_inv = false},
          },
  };
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(txChannel, &stdConfig));
  ESP_ERROR_CHECK(i2s_channel_enable(txChannel));

  startTask();
}

AudioSinkI2S::~AudioSinkI2S() {
  stopTask();
}

void AudioSinkI2S::feedPCMFrames(const uint8_t* data, size_t bytes) {
  const std::byte* src = reinterpret_cast<const std::byte*>(data);
  float scale = volumeScale.load(std::memory_order_relaxed);
  int32_t fixedScale = gainToQ15(scale);

  if (config.monoOutput) {
    const int16_t* samples = reinterpret_cast<const int16_t*>(data);
    size_t frameCount = (bytes / sizeof(int16_t)) / 2;
    downmixScratch.resize(frameCount);
    for (size_t i = 0; i < frameCount; i++) {
      int32_t mixed = (static_cast<int32_t>(samples[2 * i]) + samples[2 * i + 1]) >> 1;

      downmixScratch[i] = applyQ15Gain(mixed, fixedScale);
    }
    src = reinterpret_cast<const std::byte*>(downmixScratch.data());
    bytes = frameCount * sizeof(int16_t);
  } else if (scale < 0.999f) {
    const int16_t* samples = reinterpret_cast<const int16_t*>(data);
    size_t sampleCount = bytes / sizeof(int16_t);
    downmixScratch.resize(sampleCount);
    for (size_t i = 0; i < sampleCount; i++) {
      downmixScratch[i] = applyQ15Gain(samples[i], fixedScale);
    }
    src = reinterpret_cast<const std::byte*>(downmixScratch.data());
    bytes = sampleCount * sizeof(int16_t);
  }

  size_t written = 0;
  while (written < bytes) {
    written += ringBuffer.write(src + written, bytes - written);
  }
}

void AudioSinkI2S::volumeChanged(uint16_t volume) {
  volumeScale.store(volumeToGain(volume), std::memory_order_relaxed);
}

void AudioSinkI2S::flush() {
  ringBuffer.clear();
  flushRequested = true;
}

void AudioSinkI2S::taskLoop() {

  // Only this thread touches txChannel - flush() just clears the ring
  // buffer and raises this flag. Disable+re-enable resets the channel's
  // DMA queue, discarding whatever was already queued for playback -
  // i2s_channel_write() has no separate "drop" primitive.
  if (flushRequested.exchange(false)) {
    i2s_channel_disable(txChannel);
    i2s_channel_enable(txChannel);
    return;
  }

  std::byte chunk[512];
  size_t available = ringBuffer.read(chunk, sizeof(chunk));
  if (available == 0) {
    return;
  }

  size_t written = 0;
  while (written < available) {
    size_t chunkWritten = 0;
    esp_err_t err =
        i2s_channel_write(txChannel, chunk + written, available - written,
                          &chunkWritten, portMAX_DELAY);
    if (err != ESP_OK) {
      BELL_LOG(warn, LOG_TAG, "i2s_channel_write failed: {}",
               static_cast<int>(err));
      break;
    }
    written += chunkWritten;
  }
}
