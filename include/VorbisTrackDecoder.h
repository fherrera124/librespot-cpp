#pragma once

#include <cstdint>
#include <vector>

#include "Decoder.h"

#ifdef BELL_VORBIS_FLOAT
#include "vorbis/vorbisfile.h"
#else
#include "ivorbisfile.h"  // for OggVorbis_File, ov_callbacks
#endif

namespace cspot {
class VorbisTrackDecoder : public Decoder {
 public:
  VorbisTrackDecoder();

  bool open(CDNAudioFile* stream) override;
  void close() override;
  long readChunk(uint8_t** pcmOut) override;
  void seekMs(uint32_t positionMs) override;
  bool getPositionMs(uint32_t& outMs) override;

  // Vorbis codec callbacks - public so the free trampoline functions in
  // VorbisTrackDecoder.cpp (the only callers, ov_open_callbacks needs
  // plain function pointers) can reach them. Mirrors the shape
  // TrackPlayer's own _vorbisRead/_vorbisSeek/_vorbisTell/_vorbisClose
  // had before this class existed.
  size_t vorbisRead(void* ptr, size_t size, size_t nmemb);
  size_t vorbisClose();
  int vorbisSeek(int64_t offset, int whence);
  long vorbisTell();

 private:
  // Non-owning - see Decoder::open()'s own comment.
  CDNAudioFile* stream = nullptr;

  OggVorbis_File vorbisFile;
  ov_callbacks vorbisCallbacks;
  int currentSection = 0;
  bool opened = false;

  // readChunk()'s own decode buffer - see Decoder::readChunk()'s
  // "not owned by the caller" contract.
  std::vector<uint8_t> pcmBuffer = std::vector<uint8_t>(1024);
};
}  // namespace cspot
