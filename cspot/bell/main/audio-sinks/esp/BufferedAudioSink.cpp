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
  // I2S_CHANNEL_DEFAULT_CONFIG's dma_desc_num=6/dma_frame_num=240 gives
  // only ~1440 frames of buffering - ~33ms at 44.1kHz mono. Any
  // producer-side hiccup over that (network jitter, decode timing, Wi-Fi
  // driver latency) drains it and produces an audible glitch. 10 * 1000 =
  // 10000 frames (~227ms at 44.1kHz mono, ~113ms stereo) gives real
  // headroom, on top of the ring buffer feeding this task (see
  // startI2sFeed()) which adds another layer of cushion. 1000
  // frames/descriptor stays under the DMA descriptor's ~4092-byte-per-buffer
  // hardware limit even at 32-bit/stereo (4000 bytes). See
  // docs/spotify_component_analysis.md, findings F20/F51.
  chanConfig.dma_desc_num = 10;
  chanConfig.dma_frame_num = 1000;
  // Without this, an underrun (nothing new written for a while - e.g.
  // finding F52's pause handling, once the ring buffer/DMA buffers
  // already in flight drain) makes the I2S peripheral
  // keep re-transmitting the last DMA buffer's contents on a loop instead
  // of outputting silence - audible as a "scratched record" stutter
  // instead of clean silence. auto_clear makes the driver zero a DMA
  // buffer automatically once nothing new has been written for it. The
  // legacy driver/i2s.h API this replaced had the same knob
  // (`tx_desc_auto_clear`, see the pre-port PCM5102AudioSink.cpp) - this
  // was simply missed when picking dma_desc_num/dma_frame_num above. See
  // docs/spotify_component_analysis.md, finding F53.
  chanConfig.auto_clear = true;
  ESP_ERROR_CHECK(i2s_new_channel(&chanConfig, &txChannel, nullptr));
  applyStdConfig();
  ESP_ERROR_CHECK(i2s_channel_enable(txChannel));
}

// The only case feedPCMFrames() actually knows how to downmix is 16-bit
// stereo (see below) - applyStdConfig() must pick the same slot mode or
// the I2S peripheral and the data it's fed disagree on shape. See
// docs/spotify_component_analysis.md, finding F38 (same bug, ported here).
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
  // AudioSink advertises `softwareVolumeControl` but this used to be the
  // only place in the whole class hierarchy that didn't implement it (see
  // docs/spotify_component_analysis.md, finding F6) - now shared by every
  // sink that inherits BufferedAudioSink.
  volumeScale = static_cast<float>(volume) / static_cast<float>(UINT16_MAX);
}

void BufferedAudioSink::i2sFeedTask(void* pvParameters) {
  auto* sink = static_cast<BufferedAudioSink*>(pvParameters);
  while (true) {
    if (sink->flushRequested.exchange(false)) {
      // Drain and discard whatever's queued but not yet written - this is
      // the only task allowed to Receive from dataBuffer (see F77), so
      // flush() itself can't do this part directly.
      void* stale;
      size_t staleSize;
      while ((stale = xRingbufferReceiveUpTo(sink->dataBuffer, &staleSize, 0,
                                             SIZE_MAX)) != nullptr) {
        vRingbufferReturnItem(sink->dataBuffer, stale);
      }
      // Drops whatever's already queued in the DMA descriptors too - same
      // disable/enable already used by setParams() for reconfiguration.
      esp_err_t err = i2s_channel_disable(sink->txChannel);
      if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "flush: i2s_channel_disable failed: %s",
                 esp_err_to_name(err));
      }
      ESP_ERROR_CHECK(i2s_channel_enable(sink->txChannel));
    }

    size_t itemSize;
    // Bounded (was portMAX_DELAY) so a flush request during an idle
    // stream still gets picked up within this timeout instead of waiting
    // for the next real chunk to unblock the receive.
    char* item = (char*)xRingbufferReceiveUpTo(sink->dataBuffer, &itemSize,
                                               pdMS_TO_TICKS(100), 512);
    if (item != NULL) {
      size_t written = 0;
      while (written < itemSize) {
        size_t chunkWritten = 0;
        esp_err_t err =
            i2s_channel_write(sink->txChannel, item + written,
                              itemSize - written, &chunkWritten, portMAX_DELAY);
        if (err != ESP_OK) {
          ESP_LOGW(TAG, "i2s_channel_write failed: %s", esp_err_to_name(err));
          break;
        }
        written += chunkWritten;
      }
      vRingbufferReturnItem(sink->dataBuffer, (void*)item);
    }
  }
}

void BufferedAudioSink::startI2sFeed(size_t buf_size) {
  // PSRAM-backed explicitly (not the plain xRingbufferCreate() default) -
  // at 256KB this would eat a large chunk of scarce internal DRAM
  // otherwise. See finding F83 (CDN fetch latency spikes measured on real
  // hardware, up to ~3.5s, motivated growing this from 32KB).
  dataBuffer = xRingbufferCreateWithCaps(buf_size, RINGBUF_TYPE_BYTEBUF,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
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

  // The I2S peripheral is running in I2S_SLOT_MODE_MONO when
  // shouldDownmixToMono() is true (see applyStdConfig()), which expects
  // one flat sample per frame, not interleaved L/R pairs - downmix to
  // match. When both volume scaling and downmix apply, do them in a
  // single pass over the buffer instead of writing a full-size scaled
  // copy just to immediately read it back and average pairs of it -
  // same per-sample math (scale+clamp each channel to int16_t, then
  // integer-average the pair) as doing the two steps separately, just
  // without the redundant intermediate buffer write.
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
  xRingbufferSend(dataBuffer, pvItem, xItemSize, portMAX_DELAY);
}
