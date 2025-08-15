#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "bell/audio/Common.h"
#include "ivorbisfile.h"

#include "audio/EncryptedAudioStream.h"

namespace cspot {
class VorbisAudioStream : public EncryptedAudioStream {
 public:
  VorbisAudioStream(std::string cdnUrl, const std::vector<uint8_t>& decryptKey);

  ~VorbisAudioStream();

  /**
   * Prefetch header and footer bytes of the Vorbis stream, speeding up the initial playback. Should be called as early as possible.
   *
   * @return A result indicating success or failure.
   */
  bell::Result<> prefetchBuffers();

  /**
   * Get the duration of the audio stream in milliseconds.
   */
  size_t getDurationMs() const;

  /**
   * Get the current position in the audio stream in milliseconds.
   */
  size_t getPositionMs() const;

  /**
   * Seek to a specific position in the audio stream in milliseconds.
   */
  bool seekMs(size_t ms);

  /**
   * Read audio samples from the stream.
   * @param dst Pointer to the destination buffer where audio samples will be stored.
   * @param size Number of bytes to read. Must be a multiple of the sample size
   * @return The number of bytes read, or an error if the read fails.
   */
  bell::Result<size_t> readSamples(uint8_t* dst, size_t size);

  /**
    * Check if the stream is at the end of the file, updated after each readSamples
   */
  bool isEOF() const;

  /**
   * Get the audio format of the stream.
   */
  bell::AudioFormat getAudioFormat() const;

  // Implementation of the read function for the Vorbis stream
  size_t vorbisReadFunc(size_t bytes, size_t count, void* dst);

  // Implementation of the seek function for the Vorbis stream
  int vorbisSeekFunc(int64_t offset, int whence);

  // Implementation of the close function for the Vorbis stream
  int vorbisCloseFunc();

  // Implementation of the tell function for the Vorbis stream
  long vorbisTellFunc();

 private:
  const char* LOG_TAG = "VorbisAudioStream";

  // Ogg Vorbis file handle
  OggVorbis_File vorbisFile;

  // Flag to indicate if the read call has been done right after a seek
  bool firstReadAfterSeek = false;

  std::array<uint8_t, 8L * 1024> headerBytes, footerBytes;
  std::array<uint8_t, 12L * 1024> dataBuffer;

  size_t dataBufferStartPosition = 0;  // Start position of the data buffer
  size_t dataBufferEndPosition = 0;    // End position of the data buffer
  size_t vorbisFilePosition = 167;     // Offset for the Spotify header

  bell::Result<size_t> readVorbisBytes(uint8_t* dst, size_t size);
};
}  // namespace cspot
