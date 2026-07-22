#include "BufferedAudioSink.h"

#include <algorithm>
#include <cinttypes>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "BufferedAudioSink";

void BufferedAudioSink::initI2sChannel(const Config& config) {
  this->sinkConfig = config;

  i2s_chan_config_t chanConfig =
      I2S_CHANNEL_DEFAULT_CONFIG(config.port, I2S_ROLE_MASTER);
  // 10 descriptors * 1000 frames each: ~227ms of DMA headroom at 44.1kHz
  // mono (~113ms stereo). 1000 frames/descriptor stays under the DMA
  // descriptor's ~4092-byte-per-buffer limit even at 32-bit/stereo.
  chanConfig.dma_desc_num = 10;
  chanConfig.dma_frame_num = 1000;
  // Zeroes a DMA buffer once nothing new has been written to it, instead
  // of re-transmitting the last buffer's contents on a loop (audible
  // stutter) during an underrun.
  chanConfig.auto_clear = true;
  ESP_ERROR_CHECK(i2s_new_channel(&chanConfig, &txChannel, nullptr));
  applyStdConfig();
  ESP_ERROR_CHECK(i2s_channel_enable(txChannel));
}

// feedPCMFrames() only knows how to downmix 16-bit stereo - applyStdConfig()
// must pick the same slot mode or the I2S peripheral and the data it's fed
// disagree on shape.
bool BufferedAudioSink::shouldDownmixToMono() const {
  return bitDepth == 16 && channelCount == 2 && sinkConfig.monoOutput;
}

void BufferedAudioSink::applyStdConfig() {
  if (sinkConfig.monoOutput && channelCount == 2 && bitDepth != 16) {
    ESP_LOGW(TAG,
             "monoOutput requested but unsupported for %u-bit audio - "
             "falling back to stereo",
             bitDepth);
  }

  i2s_std_config_t stdConfig = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sampleRate),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
          static_cast<i2s_data_bit_width_t>(bitDepth),
          (channelCount == 1 || shouldDownmixToMono()) ? I2S_SLOT_MODE_MONO
                                                        : I2S_SLOT_MODE_STEREO),
      .gpio_cfg =
          {
              .mclk = sinkConfig.mclkPin,
              .bclk = sinkConfig.bclkPin,
              .ws = sinkConfig.wsPin,
              .dout = sinkConfig.doutPin,
              .din = I2S_GPIO_UNUSED,
              .invert_flags = {.mclk_inv = false,
                               .bclk_inv = false,
                               .ws_inv = false},
          },
  };
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(txChannel, &stdConfig));
}

bool BufferedAudioSink::setParams(uint32_t newSampleRate,
                                  uint8_t newChannelCount,
                                  uint8_t newBitDepth) {
  if (newSampleRate == sampleRate && newChannelCount == channelCount &&
      newBitDepth == bitDepth) {
    return true;
  }

  ESP_LOGI(TAG, "reconfiguring: %" PRIu32 "Hz, %u ch, %u bit", newSampleRate,
           newChannelCount, newBitDepth);

  sampleRate = newSampleRate;
  channelCount = newChannelCount;
  bitDepth = newBitDepth;

  esp_err_t err = i2s_channel_disable(txChannel);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "i2s_channel_disable failed: %s", esp_err_to_name(err));
    return false;
  }
  applyStdConfig();
  ESP_ERROR_CHECK(i2s_channel_enable(txChannel));
  return true;
}

void BufferedAudioSink::flush() {
  flushRequested = true;
}

void BufferedAudioSink::volumeChanged(uint16_t volume) {
  volumeScale = static_cast<float>(volume) / static_cast<float>(UINT16_MAX);
}

void BufferedAudioSink::i2sFeedTask(void* pvParameters) {
  auto* sink = static_cast<BufferedAudioSink*>(pvParameters);
  uint8_t chunk[512];
  while (true) {
    if (sink->flushRequested.exchange(false)) {
      // Discard whatever's queued but not yet written.
      sink->dataBuffer->emptyBuffer();
      // Disable/enable also drops whatever's already queued in the DMA
      // descriptors.
      esp_err_t err = i2s_channel_disable(sink->txChannel);
      if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "flush: i2s_channel_disable failed: %s",
                 esp_err_to_name(err));
      }
      ESP_ERROR_CHECK(i2s_channel_enable(sink->txChannel));
    }

    // 100ms bound: also caps how long a flush request during an idle
    // stream waits to be picked up.
    size_t itemSize =
        sink->dataBuffer->readBlocking(chunk, sizeof(chunk), 100);
    if (itemSize > 0) {
      // TEMP DIAGNOSTIC (occasional audible click investigation,
      // 2026-07-22): remove once resolved. Only logs the *first* empty
      // poll after a run of real data, not every 100ms an idle/paused
      // stream sits here - a real underrun mid-playback is what would
      // explain a click (auto_clear zeroing the DMA output mid-waveform,
      // not at a zero crossing).
      if (sink->starvedLogged) {
        ESP_LOGW(TAG, "buffer recovered after underrun");
        sink->starvedLogged = false;
      }

      size_t written = 0;
      while (written < itemSize) {
        size_t chunkWritten = 0;
        esp_err_t err =
            i2s_channel_write(sink->txChannel, chunk + written,
                              itemSize - written, &chunkWritten, portMAX_DELAY);
        if (err != ESP_OK) {
          ESP_LOGW(TAG, "i2s_channel_write failed: %s", esp_err_to_name(err));
          break;
        }
        written += chunkWritten;
      }
    } else if (!sink->starvedLogged) {
      ESP_LOGW(TAG, "buffer underrun: no PCM data for >100ms");
      sink->starvedLogged = true;
    }
  }
}

void BufferedAudioSink::startI2sFeed(size_t buf_size) {
  dataBuffer = std::make_unique<bell::CircularBuffer>(buf_size);
  xTaskCreatePinnedToCore(&BufferedAudioSink::i2sFeedTask, "i2sFeed", 4096,
                          this, 10, NULL, tskNO_AFFINITY);
}

void BufferedAudioSink::feedPCMFrames(const uint8_t* buffer, size_t bytes) {
  const int16_t* samples = reinterpret_cast<const int16_t*>(buffer);
  size_t sampleCount = bytes / sizeof(int16_t);

  bool scale = bitDepth == 16 && volumeScale < 0.999f;
  bool downmix = shouldDownmixToMono();

  const uint8_t* out = buffer;
  size_t outBytes = bytes;

  // scale+clamp each channel, then integer-average L/R pairs - single pass
  // when both apply, to avoid a redundant intermediate buffer write.
  if (scale && downmix) {
    size_t frameCount = sampleCount / 2;
    downmixBuffer.resize(frameCount);
    for (size_t i = 0; i < frameCount; i++) {
      int16_t l = static_cast<int16_t>(std::clamp(
          static_cast<float>(samples[2 * i]) * volumeScale, -32768.0f,
          32767.0f));
      int16_t r = static_cast<int16_t>(std::clamp(
          static_cast<float>(samples[2 * i + 1]) * volumeScale, -32768.0f,
          32767.0f));
      downmixBuffer[i] =
          static_cast<int16_t>((static_cast<int32_t>(l) + r) / 2);
    }
    out = reinterpret_cast<const uint8_t*>(downmixBuffer.data());
    outBytes = frameCount * sizeof(int16_t);
  } else if (scale) {
    scaleBuffer.resize(sampleCount);
    for (size_t i = 0; i < sampleCount; i++) {
      scaleBuffer[i] = static_cast<int16_t>(
          std::clamp(static_cast<float>(samples[i]) * volumeScale, -32768.0f,
                     32767.0f));
    }
    out = reinterpret_cast<const uint8_t*>(scaleBuffer.data());
  } else if (downmix) {
    size_t frameCount = sampleCount / 2;
    downmixBuffer.resize(frameCount);
    for (size_t i = 0; i < frameCount; i++) {
      downmixBuffer[i] = static_cast<int16_t>(
          (static_cast<int32_t>(samples[2 * i]) + samples[2 * i + 1]) / 2);
    }
    out = reinterpret_cast<const uint8_t*>(downmixBuffer.data());
    outBytes = frameCount * sizeof(int16_t);
  }

  feedPCMFramesInternal(out, outBytes);
}

void BufferedAudioSink::feedPCMFramesInternal(const void* pvItem,
                                              size_t xItemSize) {
  const uint8_t* data = static_cast<const uint8_t*>(pvItem);
  size_t remaining = xItemSize;
  while (remaining > 0) {
    size_t written = dataBuffer->writeBlocking(
        data + (xItemSize - remaining), remaining, /*timeoutMs=*/50);
    remaining -= written;
  }
}
