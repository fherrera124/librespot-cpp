#include "audio/VorbisAudioStream.h"

#include "bell/Logger.h"
#include "bell/Result.h"
#include "ivorbisfile.h"

using namespace cspot;

namespace {

// Vorbis callbacks, passed through to the VorbisAudioStream directly
ov_callbacks vorbisCallbacks = {
    [](void* ptr, size_t size, size_t nmemb, void* ctx) -> size_t {
      return static_cast<VorbisAudioStream*>(ctx)->vorbisReadFunc(size, nmemb,
                                                                  ptr);
    },
    [](void* ctx, ogg_int64_t offset, int whence) -> int {
      return static_cast<VorbisAudioStream*>(ctx)->vorbisSeekFunc(offset,
                                                                  whence);
    },
    [](void* ctx) -> int {
      return static_cast<VorbisAudioStream*>(ctx)->vorbisCloseFunc();
    },
    [](void* ctx) -> long {
      return static_cast<VorbisAudioStream*>(ctx)->vorbisTellFunc();
    }};
}  // namespace

VorbisAudioStream::VorbisAudioStream(std::string cdnUrl,
                                     const std::vector<uint8_t>& decryptKey)
    : EncryptedAudioStream(std::move(cdnUrl), decryptKey) {}

VorbisAudioStream::~VorbisAudioStream() {
  ov_clear(&vorbisFile);
}

bell::Result<> VorbisAudioStream::prefetchBuffers() {
  if (!isOpen()) {
    auto res = open();
    if (!res) {
      BELL_LOG(error, LOG_TAG, "Failed to open Vorbis stream: {}", res.error());
      return res;
    }
  }

  // Read the header bytes from the stream
  auto res = readBytes(headerBytes.data(), headerBytes.size());
  if (!res) {
    BELL_LOG(error, LOG_TAG, "Failed to read Vorbis header bytes: {}",
             res.error());
    return res;
  }

  BELL_LOG(debug, LOG_TAG, "File size: {} bytes", getFileSize());

  int32_t openResult =
      ov_open_callbacks(this, &vorbisFile, nullptr, 0, vorbisCallbacks);

  if (openResult < 0) {
    BELL_LOG(error, LOG_TAG, "Failed to open Vorbis file: {}", openResult);
    return bell::make_unexpected_errc(std::errc::io_error);
  }

  int r = ov_time_seek(&vorbisFile, 15000);
  BELL_LOG(debug, LOG_TAG, "ov_time_seek result: {}", r);

  return {};
}

size_t VorbisAudioStream::vorbisReadFunc(size_t bytes, size_t count,
                                         void* dst) {
  BELL_LOG(debug, LOG_TAG, "Reading {} bytes, count: {} vorbisFileOffset: {}",
           bytes, count, vorbisFilePosition);
  if (!isOpen()) {
    BELL_LOG(error, LOG_TAG, "Vorbis stream is not open");
    return 0;
  }

  // Read within the header bytes
  if (vorbisFilePosition < headerBytes.size()) {
    size_t toRead =
        std::min(bytes * count, headerBytes.size() - vorbisFilePosition);
    std::memcpy(dst, headerBytes.data() + vorbisFilePosition, toRead);
    vorbisFilePosition += toRead;
    firstReadAfterSeek = false;
    return static_cast<int>(toRead);
  }

  size_t footerStartOffset = getFileSize() - footerBytes.size();

  // Read within the footer bytes
  if (vorbisFilePosition >= footerStartOffset) {
    size_t toRead =
        std::min(bytes * count,
                 footerBytes.size() - (vorbisFilePosition - footerStartOffset));
    std::memcpy(dst,
                footerBytes.data() + (vorbisFilePosition - footerStartOffset),
                toRead);
    vorbisFilePosition += toRead;
    firstReadAfterSeek = false;
    return static_cast<int>(toRead);
  }

  // Check if data falls in between data buffer end and start positions
  if (vorbisFilePosition >= dataBufferStartPosition &&
      vorbisFilePosition < dataBufferEndPosition) {
    size_t toRead =
        std::min(bytes * count, dataBufferEndPosition - vorbisFilePosition);
    std::memcpy(
        dst, dataBuffer.data() + (vorbisFilePosition - dataBufferStartPosition),
        toRead);
    vorbisFilePosition += toRead;

    firstReadAfterSeek = false;  // Reset the flag after reading
    return static_cast<int>(toRead);
  }

  BELL_LOG(debug, LOG_TAG,
           "Reading data from CDN, vorbisFilePosition: {}, "
           "dataBufferStartPosition: {}, "
           "dataBufferEndPosition: {}",
           vorbisFilePosition, dataBufferStartPosition, dataBufferEndPosition);

  size_t targetStartPosition = vorbisFilePosition - (vorbisFilePosition % 16);

  if (firstReadAfterSeek) {
    // Leave space for lookahead and lookbehind that happens during seeking.
    targetStartPosition -= 1024 * 4;
    firstReadAfterSeek = false;
  }

  size_t toRead =
      std::min(dataBuffer.size(), getFileSize() - targetStartPosition);
  // start measuring start time
  auto startTime = std::chrono::high_resolution_clock::now();

  auto res = readBytes(dataBuffer.data(), toRead, targetStartPosition);
  if (!res) {
    BELL_LOG(error, LOG_TAG, "Failed to read Vorbis data bytes: {}",
             res.error());
    return 0;
  }

  BELL_LOG(debug, LOG_TAG, "Request took {} ms to read {} bytes",
           std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::high_resolution_clock::now() - startTime)
               .count(),
           toRead);

  dataBufferStartPosition = targetStartPosition;
  dataBufferEndPosition = targetStartPosition + toRead;

  return vorbisReadFunc(bytes, count, dst);
}

long VorbisAudioStream::vorbisTellFunc() {
  BELL_LOG(debug, LOG_TAG, "Current Vorbis file position: {}",
           vorbisFilePosition);
  return static_cast<long>(vorbisFilePosition);
}

int VorbisAudioStream::vorbisSeekFunc(int64_t offset, int whence) {
  size_t targetPosition = 0;
  switch (whence) {
    case 0:
      targetPosition = offset;
      break;
    case 1:
      targetPosition = vorbisFilePosition + offset;
      break;
    case 2:
      targetPosition = getFileSize() - offset;
      break;
    default:
      BELL_LOG(error, LOG_TAG, "Invalid whence value: {}", whence);
      return -1;
  }

  BELL_LOG(debug, LOG_TAG, "Seeking Vorbis file to position: {}, whence={}",
           targetPosition, whence);

  vorbisFilePosition = targetPosition;
  firstReadAfterSeek = true;

  return 0;
}

int VorbisAudioStream::vorbisCloseFunc() {
  BELL_LOG(debug, LOG_TAG, "Closing Vorbis stream");
  close();
  return 0;
}
