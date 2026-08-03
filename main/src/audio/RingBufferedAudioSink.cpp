#include "audio/RingBufferedAudioSink.h"

#include "audio/PCMGain.h"

using namespace cspot;

RingBufferedAudioSink::RingBufferedAudioSink(size_t ringBufferBytes,
                                             bool downmixToMono)
    : ringBuffer(ringBufferBytes), downmixToMono(downmixToMono) {}

void RingBufferedAudioSink::feedPCMFrames(const uint8_t* data, size_t bytes) {
  const std::byte* src = reinterpret_cast<const std::byte*>(data);
  float scale = volumeScale.load(std::memory_order_relaxed);
  int32_t fixedScale = gainToQ15(scale);

  if (downmixToMono) {
    const int16_t* samples = reinterpret_cast<const int16_t*>(data);
    size_t frameCount = (bytes / sizeof(int16_t)) / 2;
    gainScratch.resize(frameCount);
    for (size_t i = 0; i < frameCount; i++) {
      int32_t mixed =
          (static_cast<int32_t>(samples[2 * i]) + samples[2 * i + 1]) >> 1;
      gainScratch[i] = applyQ15Gain(mixed, fixedScale);
    }
    src = reinterpret_cast<const std::byte*>(gainScratch.data());
    bytes = frameCount * sizeof(int16_t);
  } else if (scale < 0.999f) {
    const int16_t* samples = reinterpret_cast<const int16_t*>(data);
    size_t sampleCount = bytes / sizeof(int16_t);
    gainScratch.resize(sampleCount);
    for (size_t i = 0; i < sampleCount; i++) {
      gainScratch[i] = applyQ15Gain(samples[i], fixedScale);
    }
    src = reinterpret_cast<const std::byte*>(gainScratch.data());
    bytes = sampleCount * sizeof(int16_t);
  }

  size_t written = 0;
  while (written < bytes) {
    written += ringBuffer.write(src + written, bytes - written);
  }
}

void RingBufferedAudioSink::volumeChanged(uint16_t volume) {
  volumeScale.store(volumeToGain(volume), std::memory_order_relaxed);
}

void RingBufferedAudioSink::flush() {
  ringBuffer.clear();
  flushRequested = true;
}
