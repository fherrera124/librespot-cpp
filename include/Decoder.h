#pragma once

#include <cstdint>

namespace cspot {
class CDNAudioFile;

// Strategy interface for the codec-specific part of TrackPlayer's decode
// loop (TrackPlayer.cpp's runTask()) - one fresh instance per track,
// constructed by createDecoder() (TrackPlayer.cpp) based on the track's
// own selectedFormat. Replaces what used to be an isVorbis/else branch
// duplicated across the whole decode loop (open/seek/position/read/
// close, each with its own Vorbis and MP3 copy) - adding a codec meant
// editing that same function again; adding a Decoder implementation
// doesn't touch it at all.
class Decoder {
 public:
  virtual ~Decoder() = default;

  // Opens against `stream` (non-owning - TrackPlayer keeps
  // currentTrackStream alive for the track's whole lifetime, longer than
  // any single Decoder). Returns false on failure - close() must not be
  // called on this instance if this returns false.
  virtual bool open(CDNAudioFile* stream) = 0;

  virtual void close() = 0;

  // Decodes the next chunk. Returns >0 = decoded PCM byte count (*pcmOut
  // points into this decoder's own buffer, valid until the next call -
  // not owned by the caller), 0 = clean EOF, <0 = unrecoverable error.
  virtual long readChunk(uint8_t** pcmOut) = 0;

  // Seeks to positionMs if the codec supports it - a silent no-op
  // otherwise (MP3/episodes, no seek support yet - F60).
  virtual void seekMs(uint32_t positionMs) = 0;

  // Real decoder position in ms. Returns false (nothing written to
  // outMs) if the codec has no tell() equivalent (MP3 - F60).
  virtual bool getPositionMs(uint32_t& outMs) = 0;
};
}  // namespace cspot
