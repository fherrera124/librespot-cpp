#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "Decoder.h"
#include "MP3Decoder.h"  // for bell::MP3Decoder

namespace cspot {
// Podcast episodes (F60): no seek support (mid-track seeking would need
// bitrate-based position estimation or frame scanning, neither
// implemented - seekMs() logs and drops the request) and no position
// tracking (libhelix has no tell() equivalent - getPositionMs() always
// returns false), matching the Decoder interface's own comments on both
// being optional per-codec.
class Mp3TrackDecoder : public Decoder {
 public:
  bool open(CDNAudioFile* stream) override;
  void close() override;
  long readChunk(uint8_t** pcmOut) override;
  void seekMs(uint32_t positionMs) override;
  bool getPositionMs(uint32_t& outMs) override;

 private:
  // Non-owning - see Decoder::open()'s own comment.
  CDNAudioFile* stream = nullptr;

  bell::MP3Decoder mp3Decoder;

  // Compressed bytes read from `stream` until a sync word is found -
  // mirrors the pattern in bell::EncodedAudioStream::decodeFrameMp3()
  // but with real bounds checking on the resync path (see readChunk()).
  static const size_t kInputBufferSize = 2 * 1024;
  std::vector<uint8_t> inputBuffer = std::vector<uint8_t>(kInputBufferSize);
  size_t bytesInBuffer = 0;
};
}  // namespace cspot
