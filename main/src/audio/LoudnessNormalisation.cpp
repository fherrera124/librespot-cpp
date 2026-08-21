#include "audio/LoudnessNormalisation.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace {
uint32_t readU32LE(tcb::span<const std::byte> data, size_t offset) {
  uint32_t value = 0;
  for (size_t i = 0; i < 4; i++) {
    value |= static_cast<uint32_t>(std::to_integer<uint8_t>(data[offset + i]))
             << (8 * i);
  }
  return value;
}

constexpr size_t kTrackGainOffset = 144;
constexpr size_t kTrackPeakOffset = 148;
constexpr size_t kMinHeaderBytes = kTrackPeakOffset + 4;
}  // namespace

namespace cspot {

std::optional<NormalizationData> parseNormalizationData(
    tcb::span<const std::byte> rawHeaderBytes) {
  if (rawHeaderBytes.size() < kMinHeaderBytes) {
    return std::nullopt;
  }

  uint32_t gainBits = readU32LE(rawHeaderBytes, kTrackGainOffset);
  uint32_t peakBits = readU32LE(rawHeaderBytes, kTrackPeakOffset);

  NormalizationData data{};
  std::memcpy(&data.trackGainDb, &gainBits, sizeof(float));
  std::memcpy(&data.trackPeak, &peakBits, sizeof(float));
  return data;
}

float computeNormalizationGain(float trackGainDb, float trackPeak) {
  float factor = std::pow(10.0f, trackGainDb / 20.0f);
  if (trackPeak > 0.0f) {
    factor = std::min(factor, 1.0f / trackPeak);
  }
  return factor;
}

void applyLoudnessGain(tcb::span<std::byte> pcm, float gain) {
  auto* samples = reinterpret_cast<int16_t*>(pcm.data());
  size_t sampleCount = pcm.size() / sizeof(int16_t);
  for (size_t i = 0; i < sampleCount; i++) {
    float scaled = samples[i] * gain;
    samples[i] =
        static_cast<int16_t>(std::clamp(scaled, -32768.0f, 32767.0f));
  }
}

}  // namespace cspot
