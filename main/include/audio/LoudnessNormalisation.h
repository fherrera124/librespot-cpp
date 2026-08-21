#pragma once

#include <cstddef>
#include <optional>

#include "tcb/span.hpp"

namespace cspot {

// Spotify's own encoder already measured this track's loudness (ITU-R
// BS.1770) as two little-endian floats inside the same proprietary
// header CDNDataStream's kSpotifyHeaderSize already skips past - offsets
// 144 (track_gain_db) and 148 (track_peak, linear 0-1] not dB). We never
// measure loudness ourselves, just read what Spotify already computed.
struct NormalizationData {
  float trackGainDb;
  float trackPeak;
};

// rawHeaderBytes: the same raw, still-header-included bytes
// SpotifySeekTable::tryParse() takes (e.g. from
// CDNDataStream::readRawHeaderBytes()). Returns nullopt if too short to
// safely contain both floats (needs >= 152 bytes).
std::optional<NormalizationData> parseNormalizationData(
    tcb::span<const std::byte> rawHeaderBytes);

// Applies Spotify's own normalization formula (targets -14 LUFS): clamps
// so the track's own peak sample never clips.
float computeNormalizationGain(float trackGainDb, float trackPeak);

// Scales interleaved 16-bit PCM in place by a linear gain that can exceed
// 1.0 - saturates instead of wrapping past int16 range.
void applyLoudnessGain(tcb::span<std::byte> pcm, float gain);

}  // namespace cspot
