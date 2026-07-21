#include "VorbisTrackDecoder.h"

#include "CDNAudioFile.h"
#include "Logger.h"

#ifdef BELL_VORBIS_FLOAT
#define VORBIS_SEEK(file, position) \
  (ov_time_seek(file, (double)position / 1000))
#define VORBIS_READ(file, buffer, bufferSize, section) \
  (ov_read(file, buffer, bufferSize, 0, 2, 1, section))
// ov_time_tell() returns seconds (double) in the float API, milliseconds
// (int64) in tremor's - normalized to ms either way.
#define VORBIS_TIME_TELL_MS(file) ((uint32_t)(ov_time_tell(file) * 1000))
#else
#define VORBIS_SEEK(file, position) (ov_time_seek(file, position))
#define VORBIS_READ(file, buffer, bufferSize, section) \
  (ov_read(file, buffer, bufferSize, section))
#define VORBIS_TIME_TELL_MS(file) ((uint32_t)ov_time_tell(file))
#endif

using namespace cspot;

namespace {
size_t vorbisReadCb(void* ptr, size_t size, size_t nmemb,
                    VorbisTrackDecoder* self) {
  return self->vorbisRead(ptr, size, nmemb);
}

int vorbisCloseCb(VorbisTrackDecoder* self) {
  return (int)self->vorbisClose();
}

int vorbisSeekCb(VorbisTrackDecoder* self, int64_t offset, int whence) {
  return self->vorbisSeek(offset, whence);
}

long vorbisTellCb(VorbisTrackDecoder* self) {
  return self->vorbisTell();
}
}  // namespace

VorbisTrackDecoder::VorbisTrackDecoder() {
  vorbisFile = {};
  vorbisCallbacks = {
      (decltype(ov_callbacks::read_func))&vorbisReadCb,
      (decltype(ov_callbacks::seek_func))&vorbisSeekCb,
      (decltype(ov_callbacks::close_func))&vorbisCloseCb,
      (decltype(ov_callbacks::tell_func))&vorbisTellCb,
  };
}

bool VorbisTrackDecoder::open(CDNAudioFile* stream) {
  this->stream = stream;

  int32_t result =
      ov_open_callbacks(this, &vorbisFile, NULL, 0, vorbisCallbacks);
  if (result != 0) {
    CSPOT_LOG(error, "ov_open_callbacks failed: %d", result);
    return false;
  }
  opened = true;
  return true;
}

void VorbisTrackDecoder::close() {
  if (opened) {
    ov_clear(&vorbisFile);
    opened = false;
  }
}

long VorbisTrackDecoder::readChunk(uint8_t** pcmOut) {
  *pcmOut = pcmBuffer.data();
  return VORBIS_READ(&vorbisFile, (char*)pcmBuffer.data(), pcmBuffer.size(),
                     &currentSection);
}

void VorbisTrackDecoder::seekMs(uint32_t positionMs) {
  VORBIS_SEEK(&vorbisFile, positionMs);
}

bool VorbisTrackDecoder::getPositionMs(uint32_t& outMs) {
  outMs = VORBIS_TIME_TELL_MS(&vorbisFile);
  return true;
}

size_t VorbisTrackDecoder::vorbisRead(void* ptr, size_t size, size_t nmemb) {
  if (stream == nullptr) {
    return 0;
  }
  return stream->readBytes((uint8_t*)ptr, nmemb * size);
}

size_t VorbisTrackDecoder::vorbisClose() {
  return 0;
}

int VorbisTrackDecoder::vorbisSeek(int64_t offset, int whence) {
  if (stream == nullptr) {
    return 0;
  }
  switch (whence) {
    case 0:
      stream->seek(offset);  // Spotify header offset
      break;
    case 1:
      stream->seek(stream->getPosition() + offset);
      break;
    case 2:
      stream->seek(stream->getSize() + offset);
      break;
  }
  return 0;
}

long VorbisTrackDecoder::vorbisTell() {
  if (stream == nullptr) {
    return 0;
  }
  return stream->getPosition();
}
