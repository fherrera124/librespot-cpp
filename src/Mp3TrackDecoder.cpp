#include "Mp3TrackDecoder.h"

#include <cstring>  // for memmove

#include "CDNAudioFile.h"
#include "Logger.h"

using namespace cspot;

bool Mp3TrackDecoder::open(CDNAudioFile* stream) {
  this->stream = stream;
  bytesInBuffer = 0;
  return true;
}

void Mp3TrackDecoder::close() {}

void Mp3TrackDecoder::seekMs(uint32_t positionMs) {
  if (positionMs > 0) {
    CSPOT_LOG(info,
              "Seeking isn't supported for MP3 (episodes) yet - ignoring");
  }
}

bool Mp3TrackDecoder::getPositionMs(uint32_t&) {
  return false;
}

// Not bell::EncodedAudioStream::decodeFrameMp3(): its output buffer is
// undersized for a real MP3 frame and its resync subtracts a fixed byte
// count without checking it's available. Fixed here: a correctly-sized
// buffer, resync bounded by attempt count instead. See F60.
long Mp3TrackDecoder::readChunk(uint8_t** pcmOut) {
  *pcmOut = nullptr;

  if (stream == nullptr) {
    return 0;
  }

  const int MAX_RESYNC_ATTEMPTS = 8;
  for (int attempt = 0; attempt < MAX_RESYNC_ATTEMPTS; attempt++) {
    if (bytesInBuffer < inputBuffer.size()) {
      size_t readBytes = stream->readBytes(
          inputBuffer.data() + bytesInBuffer,
          inputBuffer.size() - bytesInBuffer);
      bytesInBuffer += readBytes;
    }

    if (bytesInBuffer == 0) {
      return 0;  // Nothing buffered and nothing new to read - clean EOF.
    }

    int offset = MP3FindSyncWord(inputBuffer.data(),
                                 static_cast<int>(bytesInBuffer));
    if (offset < 0) {
      // No sync word anywhere in what's buffered - discard it and try
      // reading more on the next attempt.
      bytesInBuffer = 0;
      continue;
    }

    if (offset > 0) {
      // Discard junk before the sync word.
      memmove(inputBuffer.data(), inputBuffer.data() + offset,
             bytesInBuffer - offset);
      bytesInBuffer -= offset;
    }

    // decode() takes inData by value, so bytesAvailable (by reference) is
    // the only way to know what's left unconsumed - libhelix advances its
    // read position even on a decode error, so consumed = before - after
    // regardless of success/failure.
    uint8_t* decodePtr = inputBuffer.data();
    uint32_t bytesAvailable = static_cast<uint32_t>(bytesInBuffer);
    uint32_t outLen = 0;
    uint8_t* pcm = mp3Decoder.decode(decodePtr, bytesAvailable, outLen);

    size_t consumed = bytesInBuffer - bytesAvailable;
    if (consumed > 0 && bytesAvailable > 0) {
      memmove(inputBuffer.data(), inputBuffer.data() + consumed,
             bytesAvailable);
    }
    bytesInBuffer = bytesAvailable;

    if (pcm != nullptr) {
      *pcmOut = pcm;
      return static_cast<long>(outLen);
    }
    // Decode error on this frame - loop and try the next sync word.
  }

  CSPOT_LOG(error, "MP3 resync failed after %d attempts", MAX_RESYNC_ATTEMPTS);
  return -1;
}
