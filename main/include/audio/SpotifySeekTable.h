#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include "tcb/span.hpp"

namespace cspot {

// Parses the proprietary Ogg page Spotify prefixes to every CDN Ogg file -
// the same bytes CDNDataStream's kSpotifyHeaderSize already skips past -
// and answers "what byte offset roughly corresponds to this sample
// position" in O(1), without touching the actual audio stream at all.
// Same technique go-librespot's own vorbis/metadata.go uses; format
// verified byte-for-byte against a real captured header: a self-contained
// single-page Ogg logical stream (standard 27-byte page header + a 1-byte
// segment table) wrapping one packet that starts with a 0x81 marker byte,
// followed by length-prefixed segments - type 0 is the seek table this
// class reads, type 1 is replay gain (unused here), type 2 is a
// terminator.
//
// Purely a lookup/interpolation - the caller still needs to resync to a
// real Vorbis Ogg page after landing on the returned offset (see
// OggContainer::seekToByteOffset()).
class SpotifySeekTable {
 public:
  // rawHeaderBytes: the raw, still-Ogg-page-wrapped header bytes (e.g.
  // from CDNDataStream::readRawHeaderBytes()). Returns nullopt if the
  // page/packet framing or the 0x81 marker doesn't match, or the seek
  // table segment is missing/truncated/out of range - callers fall back
  // to bisection search in that case.
  static std::optional<SpotifySeekTable> tryParse(
      tcb::span<const std::byte> rawHeaderBytes);

  // Byte offset (logical, header-excluded - the same coordinate space
  // CDNDataStream/OggContainer's public interfaces already use) closest
  // to the given sample position.
  size_t getBytePosition(uint64_t samplePos) const;

 private:
  SpotifySeekTable() = default;

  uint32_t seekSamples = 0;
  uint32_t seekSize = 0;
  std::array<int32_t, 100> table{};
};

}  // namespace cspot
