#pragma once

#include <algorithm>
#include <cmath>

namespace cspot {

// Spotify's own encoder already measured this track's loudness (ITU-R
// BS.1770) and targets -14 LUFS via trackGainDb - we never measure loudness
// ourselves, just apply the gain. pregainDb is a user-configurable offset on
// top of that (Context::ConfigState::normalisationPregainDb). Clamped so the
// track's own peak sample never clips, same approach as librespot's
// NormalisationData::get_factor().
inline float computeNormalizationGain(float trackGainDb, float trackPeak,
                                      float pregainDb) {
  float factor = std::pow(10.0f, (trackGainDb + pregainDb) / 20.0f);
  if (trackPeak > 0.0f) {
    factor = std::min(factor, 1.0f / trackPeak);
  }
  return factor;
}

}  // namespace cspot
