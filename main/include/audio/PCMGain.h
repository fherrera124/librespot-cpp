#pragma once

#include <cstdint>

namespace cspot {

// 1.0 in Q15 fixed point.
constexpr int32_t kQ15One = 1 << 15;

// Converts a connect-state volume (0..65535) to a linear 0..1 gain, squared
// rather than linear to approximate how humans perceive loudness.
inline float volumeToGain(uint16_t volume) {
  float linear = static_cast<float>(volume) / static_cast<float>(UINT16_MAX);
  return linear * linear;
}

// Converts a 0..1 linear gain to a Q15 fixed-point multiplier - call once
// per buffer, not per sample, and feed the result to applyQ15Gain().
inline int32_t gainToQ15(float gain) {
  return static_cast<int32_t>(gain * static_cast<float>(kQ15One));
}

// Applies a Q15 fixed-point gain to a single sample. q15Gain is normally
// gainToQ15()'s output, kept as a separate argument so callers compute it
// once per buffer instead of once per sample.
inline int16_t applyQ15Gain(int32_t sample, int32_t q15Gain) {
  return static_cast<int16_t>((sample * q15Gain) >> 15);
}

}  // namespace cspot
