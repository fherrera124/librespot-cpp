#include "TrackPlayer.h"

#include <cstring>      // for memmove
#include <mutex>        // for mutex, scoped_lock
#include <string>       // for string
#include <type_traits>  // for remove_extent_t
#include <vector>       // for vector, vector<>::value_type

#include "BellLogger.h"        // for AbstractLogger
#include "BellUtils.h"         // for BELL_SLEEP_MS
#include "Logger.h"            // for CSPOT_LOG
#include "Packet.h"            // for cspot
#include "TrackQueue.h"        // for CDNTrackStream, CDNTrackStream::TrackInfo
#include "WrappedSemaphore.h"  // for WrappedSemaphore

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

namespace cspot {
struct Context;
struct TrackReference;
}  // namespace cspot

using namespace cspot;

static size_t vorbisReadCb(void* ptr, size_t size, size_t nmemb,
                           TrackPlayer* self) {
  return self->_vorbisRead(ptr, size, nmemb);
}

static int vorbisCloseCb(TrackPlayer* self) {
  return self->_vorbisClose();
}

static int vorbisSeekCb(TrackPlayer* self, int64_t offset, int whence) {

  return self->_vorbisSeek(offset, whence);
}

static long vorbisTellCb(TrackPlayer* self) {
  return self->_vorbisTell();
}

TrackPlayer::TrackPlayer(std::shared_ptr<cspot::Context> ctx,
                         std::shared_ptr<cspot::TrackQueue> trackQueue,
                         EOFCallback eof, TrackLoadedCallback trackLoaded,
                         TrackReachedPlaybackCallback reachedPlaybackCallback)
    : bell::Task("cspot_player", 48 * 1024, 5, 1) {
  this->ctx = ctx;
  this->eofCallback = eof;
  this->trackLoaded = trackLoaded;
  this->reachedPlaybackCallback = reachedPlaybackCallback;
  this->trackQueue = trackQueue;
  this->playbackSemaphore = std::make_unique<bell::WrappedSemaphore>(5);

  // Initialize vorbis callbacks
  vorbisFile = {};
  vorbisCallbacks = {
      (decltype(ov_callbacks::read_func))&vorbisReadCb,
      (decltype(ov_callbacks::seek_func))&vorbisSeekCb,
      (decltype(ov_callbacks::close_func))&vorbisCloseCb,
      (decltype(ov_callbacks::tell_func))&vorbisTellCb,
  };
}

TrackPlayer::~TrackPlayer() {
  isRunning = false;
  resetState();
  std::scoped_lock lock(runningMutex);
}

void TrackPlayer::start() {
  if (!isRunning) {
    isRunning = true;
    startTask();
  }
}

void TrackPlayer::stop() {
  isRunning = false;
  resetState();
  std::scoped_lock lock(runningMutex);
}

void TrackPlayer::resetState(bool paused) {
  this->pendingReset = true;
  this->currentSongPlaying = false;
  this->startPaused = paused;

  std::scoped_lock lock(dataOutMutex);

  CSPOT_LOG(info, "Resetting state");
}

bool TrackPlayer::getDecoderPositionMs(uint32_t& outMs) const {
  if (!hasDecoderPosition) {
    return false;
  }
  outMs = decoderPositionMs;
  return true;
}

void TrackPlayer::setAudioSink(AudioSink* sink) {
  audioSink = sink;
}

void TrackPlayer::setPaused(bool paused) {
  this->paused = paused;
  // Heard immediately instead of after whatever's buffered downstream
  // drains - same behavior as before (F52/F77), decided here now.
  if (paused && audioSink) {
    audioSink->flush();
  }
}

void TrackPlayer::setVolume(uint16_t volume) {
  if (audioSink) {
    audioSink->volumeChanged(volume);
  }
}

bool TrackPlayer::setAudioParams(uint32_t sampleRate, uint8_t channelCount,
                                uint8_t bitDepth) {
  return audioSink ? audioSink->setParams(sampleRate, channelCount, bitDepth)
                   : false;
}

void TrackPlayer::feedChunk(const uint8_t* data, size_t bytes,
                            std::string_view trackId,
                            bool& notifiedThisTrack) {
  if (!notifiedThisTrack && reachedPlaybackCallback) {
    // TEMP DIAGNOSTIC (track-flicker investigation, 2026-07-18): remove
    // once resolved.
    CSPOT_LOG(info, "feedChunk: firing reachedPlaybackCallback trackId=%.*s",
             (int)trackId.size(), trackId.data());
    reachedPlaybackCallback(trackId);
    notifiedThisTrack = true;
  }

  // Retries this same already-decoded chunk while paused instead of
  // decoding ahead - never discards, mirrors the old dataCallback
  // 0-return contract (F52/F77).
  while (paused && currentSongPlaying && !pendingReset) {
    BELL_SLEEP_MS(50);
  }

  if (audioSink && currentSongPlaying && !pendingReset) {
    std::scoped_lock dataOutLock(dataOutMutex);
    if (currentSongPlaying && !pendingReset) {
      audioSink->feedPCMFrames(data, bytes);
    }
  }
}

void TrackPlayer::seekMs(size_t ms) {
  if (inFuture) {
    // Already playing the next track - reset to seek within it.
    resetState();
  }

  CSPOT_LOG(info, "Seeking...");
  this->pendingSeekPositionMs = ms;

  // Report the new position immediately instead of waiting for the decode
  // loop to actually apply it (VORBIS_SEEK/VORBIS_TIME_TELL_MS only run at
  // the top of the read loop) - while paused, that loop is blocked inside
  // feedChunk()'s wait and won't get back there until resumed, which left
  // getDecoderPositionMs() reporting the pre-seek position the whole time
  // paused. Guarded on hasDecoderPosition so this stays a no-op for MP3
  // (seeking unsupported there - F60) rather than fabricating one.
  if (hasDecoderPosition) {
    decoderPositionMs = (uint32_t)ms;
  }
}

void TrackPlayer::runTask() {
  std::scoped_lock lock(runningMutex);

  std::shared_ptr<QueuedTrack> track, newTrack = nullptr;

  int trackOffset = 0;
  bool eof = false;
  bool endOfQueueReached = false;

  while (isRunning) {
    if (!this->trackQueue->hasTracks() ||
        (!pendingReset && endOfQueueReached && trackQueue->isFinished())) {
      this->trackQueue->playableSemaphore->twait(300);
      continue;
    }

    if (pendingReset) {
      track = nullptr;
      pendingReset = false;
      inFuture = false;
    }

    endOfQueueReached = false;

    // Debounce rapid re-queueing - a reset requested during this wait
    // restarts the loop instead of acting on a stale track.
    BELL_SLEEP_MS(50);

    if (pendingReset) {
      continue;
    }

    newTrack = trackQueue->consumeTrack(track, trackOffset);

    if (newTrack == nullptr) {
      if (trackOffset == -1) {
        // Reset required
        track = nullptr;
      }

      BELL_SLEEP_MS(100);
      continue;
    }

    track = newTrack;

    inFuture = trackOffset > 0;

    if (track->state != QueuedTrack::State::READY) {
      // Wide enough for TrackQueue's own bounded retry (metadata/audio-key/
      // CDN-url, up to 3 attempts with a 1s backoff each - see
      // TrackQueue.cpp) to actually finish before this gives up; the
      // semaphore is only given on that retry's final outcome (success or
      // attempts exhausted), never on an intermediate attempt.
      track->loadedSemaphore->twait(8000);

      if (track->state != QueuedTrack::State::READY) {
        CSPOT_LOG(error, "Track failed to load, skipping it");
        // Advance TrackQueue's own head past this failed track too, or it
        // stays one position behind whatever plays next. See F89.
        // (0 for currentPositionMs: only read by skipTrack()'s PREV branch.)
        trackQueue->skipTrack(TrackQueue::SkipDirection::NEXT, 0, false);
        this->eofCallback();
        continue;
      }
    }

    // TEMP DIAGNOSTIC (track-flicker investigation, 2026-07-18): trackOffset
    // - remove once resolved.
    CSPOT_LOG(info, "Got track ID=%s trackOffset=%d inFuture=%d",
             track->identifier.c_str(), trackOffset, (int)inFuture);

    currentSongPlaying = true;

    {
      std::scoped_lock lock(playbackMutex);

      currentTrackStream = track->getAudioFile(cdnConnection);

      // openStream() can throw (CDN request failure, or a too-short
      // response - CDNAudioFile.cpp), and unlike readBytes() it isn't
      // protected internally. Uncaught, that would crash this task and
      // reboot the device on a single bad CDN response. See F75.
      try {
        currentTrackStream->openStream();
      } catch (const std::exception& e) {
        CSPOT_LOG(error, "Failed to open CDN stream, skipping track: %s",
                  e.what());
        currentTrackStream = nullptr;
        this->eofCallback();
        continue;
      }

      if (pendingReset || !currentSongPlaying) {
        continue;
      }

      if (trackOffset == 0 && pendingSeekPositionMs == 0) {
        this->trackLoaded(track, startPaused);
        startPaused = false;
      }

      // "Not Vorbis" reliably means MP3 here - selectedFormat only ever
      // resolves to an OGG_VORBIS_* or MP3_* format (F60).
      bool isVorbis = track->selectedFormat == AudioFormat_OGG_VORBIS_96 ||
                      track->selectedFormat == AudioFormat_OGG_VORBIS_160 ||
                      track->selectedFormat == AudioFormat_OGG_VORBIS_320;

      // Fires once, right before the first chunk reaches audioSink - must
      // run even if this track loads straight into a paused state (see
      // the notify call sites below), or the app would never learn a
      // paused track exists at all. See F76/F79 (the old, cross-thread
      // version of this same requirement).
      bool notifiedThisTrack = false;

      if (isVorbis) {
        int32_t r =
            ov_open_callbacks(this, &vorbisFile, NULL, 0, vorbisCallbacks);

        if (pendingSeekPositionMs > 0) {
          track->requestedPosition = pendingSeekPositionMs;
        }

        if (track->requestedPosition > 0) {
          VORBIS_SEEK(&vorbisFile, track->requestedPosition);
        }
        decoderPositionMs = VORBIS_TIME_TELL_MS(&vorbisFile);
        hasDecoderPosition = true;

        eof = false;

        CSPOT_LOG(info, "Playing");

        while (!eof && currentSongPlaying) {
          if (pendingSeekPositionMs > 0) {
            uint32_t seekPosition = pendingSeekPositionMs;
            pendingSeekPositionMs = 0;
            VORBIS_SEEK(&vorbisFile, seekPosition);
            decoderPositionMs = VORBIS_TIME_TELL_MS(&vorbisFile);
          }

          long ret = VORBIS_READ(&vorbisFile, (char*)&pcmBuffer[0],
                                 pcmBuffer.size(), &currentSection);
          decoderPositionMs = VORBIS_TIME_TELL_MS(&vorbisFile);

          if (ret == 0) {
            CSPOT_LOG(info, "EOF");
            eof = true;
          } else if (ret < 0) {
            CSPOT_LOG(error, "An error has occured in the stream %d", ret);
            // eof, not just currentSongPlaying: a mid-stream decode error
            // must still reach the eofCallback()/isFinished() check below,
            // same as a clean EOF - otherwise a decode error on the last
            // queued track never advances or signals DEPLETED.
            currentSongPlaying = false;
            eof = true;
          } else {
            feedChunk((const uint8_t*)&pcmBuffer[0], (size_t)ret,
                     track->identifier, notifiedThisTrack);
          }
        }
        ov_clear(&vorbisFile);
        hasDecoderPosition = false;
      } else {
        // MP3 (podcast episodes). No seek support yet: mid-track seeking
        // would need bitrate-based position estimation or frame
        // scanning, neither implemented here - a pending seek is logged
        // and dropped instead of silently mishandled. See finding F60.
        mp3BytesInBuffer = 0;

        if (pendingSeekPositionMs > 0) {
          pendingSeekPositionMs = 0;
        }
        if (track->requestedPosition > 0) {
          CSPOT_LOG(info,
                    "Seeking isn't supported for MP3 (episodes) yet - "
                    "starting from the beginning");
        }

        eof = false;

        CSPOT_LOG(info, "Playing (MP3)");

        while (!eof && currentSongPlaying) {
          if (pendingSeekPositionMs > 0) {
            CSPOT_LOG(info,
                      "Seeking isn't supported for MP3 (episodes) yet - "
                      "ignoring");
            pendingSeekPositionMs = 0;
          }

          uint8_t* pcm = nullptr;
          long ret = _mp3DecodeFrame(&pcm);

          if (ret == 0) {
            CSPOT_LOG(info, "EOF");
            eof = true;
          } else if (ret < 0) {
            CSPOT_LOG(error, "An error has occured in the MP3 stream %ld",
                      ret);
            // See the matching Vorbis branch above: must set eof too, or
            // eofCallback() never fires for a decode error.
            currentSongPlaying = false;
            eof = true;
          } else {
            feedChunk(pcm, (size_t)ret, track->identifier, notifiedThisTrack);
          }
        }
      }

      CSPOT_LOG(info, "Playing done");

      currentTrackStream = nullptr;
    }

    if (eof) {
      if (trackQueue->isFinished()) {
        endOfQueueReached = true;
      }

      this->eofCallback();
    }
  }
}

size_t TrackPlayer::_vorbisRead(void* ptr, size_t size, size_t nmemb) {
  if (this->currentTrackStream == nullptr) {
    return 0;
  }
  return this->currentTrackStream->readBytes((uint8_t*)ptr, nmemb * size);
}

size_t TrackPlayer::_vorbisClose() {
  return 0;
}

int TrackPlayer::_vorbisSeek(int64_t offset, int whence) {
  if (this->currentTrackStream == nullptr) {
    return 0;
  }
  switch (whence) {
    case 0:
      this->currentTrackStream->seek(offset);  // Spotify header offset
      break;
    case 1:
      this->currentTrackStream->seek(this->currentTrackStream->getPosition() +
                                     offset);
      break;
    case 2:
      this->currentTrackStream->seek(this->currentTrackStream->getSize() +
                                     offset);
      break;
  }

  return 0;
}

long TrackPlayer::_vorbisTell() {
  if (this->currentTrackStream == nullptr) {
    return 0;
  }
  return this->currentTrackStream->getPosition();
}

// Not bell::EncodedAudioStream::decodeFrameMp3(): its output buffer is
// undersized for a real MP3 frame and its resync subtracts a fixed byte
// count without checking it's available. Fixed here: a correctly-sized
// buffer, resync bounded by attempt count instead. See F60.
long TrackPlayer::_mp3DecodeFrame(uint8_t** pcmOut) {
  *pcmOut = nullptr;

  if (this->currentTrackStream == nullptr) {
    return 0;
  }

  const int MAX_RESYNC_ATTEMPTS = 8;
  for (int attempt = 0; attempt < MAX_RESYNC_ATTEMPTS; attempt++) {
    if (mp3BytesInBuffer < mp3InputBuffer.size()) {
      size_t readBytes = currentTrackStream->readBytes(
          mp3InputBuffer.data() + mp3BytesInBuffer,
          mp3InputBuffer.size() - mp3BytesInBuffer);
      mp3BytesInBuffer += readBytes;
    }

    if (mp3BytesInBuffer == 0) {
      return 0;  // Nothing buffered and nothing new to read - clean EOF.
    }

    int offset = MP3FindSyncWord(mp3InputBuffer.data(),
                                 static_cast<int>(mp3BytesInBuffer));
    if (offset < 0) {
      // No sync word anywhere in what's buffered - discard it and try
      // reading more on the next attempt.
      mp3BytesInBuffer = 0;
      continue;
    }

    if (offset > 0) {
      // Discard junk before the sync word.
      memmove(mp3InputBuffer.data(), mp3InputBuffer.data() + offset,
             mp3BytesInBuffer - offset);
      mp3BytesInBuffer -= offset;
    }

    // decode() takes inData by value, so bytesAvailable (by reference) is
    // the only way to know what's left unconsumed - libhelix advances its
    // read position even on a decode error, so consumed = before - after
    // regardless of success/failure.
    uint8_t* decodePtr = mp3InputBuffer.data();
    uint32_t bytesAvailable = static_cast<uint32_t>(mp3BytesInBuffer);
    uint32_t outLen = 0;
    uint8_t* pcm = mp3Decoder.decode(decodePtr, bytesAvailable, outLen);

    size_t consumed = mp3BytesInBuffer - bytesAvailable;
    if (consumed > 0 && bytesAvailable > 0) {
      memmove(mp3InputBuffer.data(), mp3InputBuffer.data() + consumed,
             bytesAvailable);
    }
    mp3BytesInBuffer = bytesAvailable;

    if (pcm != nullptr) {
      *pcmOut = pcm;
      return static_cast<long>(outLen);
    }
    // Decode error on this frame - loop and try the next sync word.
  }

  CSPOT_LOG(error, "MP3 resync failed after %d attempts", MAX_RESYNC_ATTEMPTS);
  return -1;
}
