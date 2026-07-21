#include "PlaybackController.h"

#include "AccessKeyFetcher.h"
#include "CSpotContext.h"  // for Context
#include "Logger.h"        // for CSPOT_LOG
#include "TimeProvider.h"
#include "TrackReference.h"  // for TrackReference

using namespace cspot;

PlaybackController::PlaybackController(std::shared_ptr<cspot::Context> ctx,
                                       TrackLoadedCallback onTrackLoaded,
                                       TrackReachedCallback onTrackReached,
                                       DepletedCallback onDepleted)
    : ctx(ctx), onTrackReached(std::move(onTrackReached)) {
  trackQueue = std::make_shared<cspot::TrackQueue>(
      ctx, std::make_shared<cspot::AccessKeyFetcher>(ctx));

  auto eofCallback = [this, onDepleted = std::move(onDepleted)]() {
    if (trackQueue->isFinished()) {
      // Repeat-context (F92): loop back to the first track instead of
      // ending. See TrackQueue::restartFromBeginning().
      if (trackQueue->isRepeatingContext()) {
        trackQueue->restartFromBeginning();
      } else {
        onDepleted();
      }
    }
  };

  auto trackLoadedCallback = [this, onTrackLoaded = std::move(onTrackLoaded)](
                                 std::shared_ptr<QueuedTrack> track,
                                 bool paused = false) {
    {
      std::lock_guard<std::mutex> lock(positionMutex);
      isPlayingState = !paused;
      positionMs = track->requestedPosition;
      positionMeasuredAt = this->ctx->timeProvider->getSyncedTimestamp();
      currentTrackStartedAtMs = positionMeasuredAt;
    }
    onTrackLoaded(track, paused);
  };

  auto reachedPlaybackCallback = [this](std::string_view trackId) {
    // TEMP DIAGNOSTIC (track-flicker investigation, 2026-07-18): confirm
    // whether this fires twice in quick succession with different
    // trackIds - remove once resolved.
    CSPOT_LOG(info,
             "reachedPlaybackCallback: called trackId=%.*s notifyPending=%d",
             (int)trackId.size(), trackId.data(),
             (int)trackQueue->notifyPending);

    int offset = 0;

    // consumeTrack() returns nullptr when the queue is exhausted - see
    // F24, same null-deref hazard SpircHandler's own copy of this logic
    // guards against.
    auto currentTrack = trackQueue->consumeTrack(nullptr, offset);
    if (currentTrack == nullptr) {
      CSPOT_LOG(info, "reachedPlaybackCallback: queue empty, nothing to report");
      return;
    }

    if (trackQueue->notifyPending) {
      trackQueue->notifyPending = false;
      std::lock_guard<std::mutex> lock(positionMutex);
      positionMs = currentTrack->requestedPosition;
      positionMeasuredAt = this->ctx->timeProvider->getSyncedTimestamp();
      currentTrack->requestedPosition = 0;
    } else {
      // preloadedTracks' own head can be more than one skip behind
      // trackId: TrackPlayer.cpp's own failure path (F89) already calls
      // skipTrack() when a track fails to load, but that pops whatever
      // is CURRENTLY at the head - which, if this class hasn't caught up
      // from the PREVIOUS transition yet, is the already-finished track,
      // not the failed one. Left uncorrected, the failed track stays
      // stuck at the head and this function can end up never finding
      // trackId at all - silently never reporting whatever's actually
      // playing. Keep skipping forward (never backward, never guessing)
      // until the head really is trackId. One skip covers the normal
      // case; bounded generously beyond that only as a guard against an
      // actual bug elsewhere looping forever.
      constexpr int MAX_CATCHUP_SKIPS = 16;
      int skips = 0;
      while (currentTrack->identifier != trackId) {
        CSPOT_LOG(info,
                 "reachedPlaybackCallback: catch-up skip #%d, head='%s' "
                 "(uri=%s) != want='%.*s'",
                 skips + 1, currentTrack->identifier.c_str(),
                 currentTrack->ref.uri.c_str(), (int)trackId.size(),
                 trackId.data());
        if (++skips > MAX_CATCHUP_SKIPS ||
            !trackQueue->skipTrack(TrackQueue::SkipDirection::NEXT, 0, false)) {
          CSPOT_LOG(error,
                   "reachedPlaybackCallback: couldn't catch up to '%.*s', "
                   "giving up",
                   (int)trackId.size(), trackId.data());
          return;
        }
        currentTrack = trackQueue->consumeTrack(nullptr, offset);
        if (currentTrack == nullptr) {
          CSPOT_LOG(info, "reachedPlaybackCallback: queue exhausted mid-catch-up");
          return;
        }
      }
      std::lock_guard<std::mutex> lock(positionMutex);
      positionMs = 0;
      positionMeasuredAt = this->ctx->timeProvider->getSyncedTimestamp();
    }

    this->onTrackReached(currentTrack);
  };

  trackPlayer = std::make_shared<TrackPlayer>(ctx, trackQueue, eofCallback,
                                              trackLoadedCallback,
                                              reachedPlaybackCallback);
  // Unlike SpircHandler (which lazily started this on the first Load
  // frame), this engine is meant to just be ready - there's no separate
  // "session established" moment to defer to.
  trackPlayer->start();
}

void PlaybackController::loadTracks(const std::vector<TrackReference>& tracks,
                                    int startIndex,
                                    uint32_t requestedPositionMs,
                                    bool startPaused) {
  {
    std::lock_guard<std::mutex> lock(positionMutex);
    positionMs = requestedPositionMs;
    positionMeasuredAt = ctx->timeProvider->getSyncedTimestamp();
    // Real play/pause state is only known once trackLoadedCallback fires
    // - startPaused just seeds resetState() below.
    isPlayingState = false;
  }
  trackQueue->updateTracks(tracks, startIndex, requestedPositionMs, true);
  trackPlayer->resetState(startPaused);
}

void PlaybackController::setPlaybackPlaying(bool playing) {
  std::lock_guard<std::mutex> lock(positionMutex);
  if (isPlayingState && !playing) {
    positionMs += (uint32_t)(ctx->timeProvider->getSyncedTimestamp() -
                             positionMeasuredAt);
  }
  positionMeasuredAt = ctx->timeProvider->getSyncedTimestamp();
  isPlayingState = playing;
}

uint32_t PlaybackController::getPositionMs() {
  // Real decoder position (Vorbis only) beats the wall-clock estimate
  // below - freezes on its own while paused, see TrackPlayer.h's comment.
  uint32_t decoderPosition;
  if (trackPlayer->getDecoderPositionMs(decoderPosition)) {
    return decoderPosition;
  }

  std::lock_guard<std::mutex> lock(positionMutex);
  uint32_t position = positionMs;
  if (isPlayingState) {
    position += (uint32_t)(ctx->timeProvider->getSyncedTimestamp() -
                           positionMeasuredAt);
  }
  return position;
}

bool PlaybackController::isPlaying() const {
  std::lock_guard<std::mutex> lock(positionMutex);
  return isPlayingState;
}

bool PlaybackController::skipTrack(TrackQueue::SkipDirection dir,
                                   bool allowSeeking) {
  bool skipped =
      trackQueue->skipTrack(dir, getPositionMs(), true, allowSeeking);
  trackPlayer->resetState(!skipped);
  return skipped;
}

bool PlaybackController::nextSong() {
  return skipTrack(TrackQueue::SkipDirection::NEXT);
}

bool PlaybackController::previousSong(bool allowSeeking) {
  return skipTrack(TrackQueue::SkipDirection::PREV, allowSeeking);
}

void PlaybackController::seekMs(uint32_t position) {
  trackPlayer->seekMs(position);

  std::lock_guard<std::mutex> lock(positionMutex);
  positionMs = position;
  positionMeasuredAt = ctx->timeProvider->getSyncedTimestamp();
}

void PlaybackController::setRepeatContext(bool repeat) {
  trackQueue->setRepeatContext(repeat);
}

void PlaybackController::reportEnded() {
  {
    std::lock_guard<std::mutex> lock(positionMutex);
    isPlayingState = false;
    positionMs = 0;
    positionMeasuredAt = ctx->timeProvider->getSyncedTimestamp();
  }
  trackPlayer->resetState(true);
}

void PlaybackController::stop() {
  trackPlayer->stop();
}

void PlaybackController::disconnect() {
  trackQueue->stopTask();
  trackPlayer->stop();
}

int64_t PlaybackController::getCurrentTrackStartedAtMs() const {
  std::lock_guard<std::mutex> lock(positionMutex);
  return currentTrackStartedAtMs;
}
