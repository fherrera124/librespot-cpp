#include "TrackPlayer.h"

#include <memory>  // for unique_ptr, make_unique
#include <mutex>   // for mutex, scoped_lock
#include <string>  // for string

#include "BellLogger.h"        // for AbstractLogger
#include "BellUtils.h"         // for BELL_SLEEP_MS
#include "Decoder.h"
#include "Logger.h"            // for CSPOT_LOG
#include "Mp3TrackDecoder.h"
#include "Packet.h"            // for cspot
#include "TrackQueue.h"        // for CDNTrackStream, CDNTrackStream::TrackInfo
#include "VorbisTrackDecoder.h"
#include "WrappedSemaphore.h"  // for WrappedSemaphore

namespace cspot {
struct Context;
struct TrackReference;
}  // namespace cspot

using namespace cspot;

namespace {
// "Not Vorbis" reliably means MP3 here - selectedFormat only ever
// resolves to an OGG_VORBIS_* or MP3_* format (F60).
std::unique_ptr<Decoder> createDecoder(AudioFormat format) {
  bool isVorbis = format == AudioFormat_OGG_VORBIS_96 ||
                 format == AudioFormat_OGG_VORBIS_160 ||
                 format == AudioFormat_OGG_VORBIS_320;
  if (isVorbis) {
    return std::make_unique<VorbisTrackDecoder>();
  }
  return std::make_unique<Mp3TrackDecoder>();
}
}  // namespace

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

    if (track->getState() != QueuedTrack::State::READY) {
      // Wide enough for TrackQueue's own bounded retry (metadata/audio-key/
      // CDN-url, up to 3 attempts with a 1s backoff each - see
      // TrackQueue.cpp) to actually finish before this gives up; the
      // semaphore is only given on that retry's final outcome (success or
      // attempts exhausted), never on an intermediate attempt.
      track->loadedSemaphore->twait(8000);

      if (track->getState() != QueuedTrack::State::READY) {
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
      // No lock here (playbackMutex, removed - it was only ever taken by
      // this one call site, so it never actually excluded anything):
      // currentTrackStream and the local decoder below are only ever
      // touched from this task - every other TrackPlayer method that can
      // be called cross-thread (resetState/seekMs/setPaused/etc.) only
      // reads/writes the std::atomic<> fields declared in TrackPlayer.h.
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

      // Fires once, right before the first chunk reaches audioSink - must
      // run even if this track loads straight into a paused state (see
      // the notify call sites below), or the app would never learn a
      // paused track exists at all. See F76/F79 (the old, cross-thread
      // version of this same requirement).
      bool notifiedThisTrack = false;

      std::unique_ptr<Decoder> decoder = createDecoder(track->selectedFormat);
      uint32_t posMs;

      if (!decoder->open(currentTrackStream.get())) {
        // Same fallthrough as a mid-stream decode error below: must
        // still reach the eofCallback()/isFinished() check, or a bad
        // track never advances or signals DEPLETED. open() failed, so
        // close() must not be called on this instance (Decoder.h).
        currentSongPlaying = false;
        eof = true;
      } else {
        if (pendingSeekPositionMs > 0) {
          track->requestedPosition = (uint32_t)pendingSeekPositionMs;
        }

        if (track->requestedPosition > 0) {
          decoder->seekMs(track->requestedPosition);
        }
        hasDecoderPosition = decoder->getPositionMs(posMs);
        if (hasDecoderPosition) decoderPositionMs = posMs;

        eof = false;

        CSPOT_LOG(info, "Playing");

        while (!eof && currentSongPlaying) {
          if (pendingSeekPositionMs > 0) {
            uint32_t seekPosition = (uint32_t)pendingSeekPositionMs;
            pendingSeekPositionMs = 0;
            decoder->seekMs(seekPosition);
            hasDecoderPosition = decoder->getPositionMs(posMs);
            if (hasDecoderPosition) decoderPositionMs = posMs;
          }

          uint8_t* pcm = nullptr;
          long ret = decoder->readChunk(&pcm);
          hasDecoderPosition = decoder->getPositionMs(posMs);
          if (hasDecoderPosition) decoderPositionMs = posMs;

          if (ret == 0) {
            CSPOT_LOG(info, "EOF");
            eof = true;
          } else if (ret < 0) {
            CSPOT_LOG(error, "An error has occured in the stream %ld", ret);
            // eof, not just currentSongPlaying: a mid-stream decode error
            // must still reach the eofCallback()/isFinished() check below,
            // same as a clean EOF - otherwise a decode error on the last
            // queued track never advances or signals DEPLETED.
            currentSongPlaying = false;
            eof = true;
          } else {
            feedChunk(pcm, (size_t)ret, track->identifier, notifiedThisTrack);
          }
        }
        decoder->close();
        hasDecoderPosition = false;
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
