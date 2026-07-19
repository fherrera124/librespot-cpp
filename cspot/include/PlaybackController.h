#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "TrackPlayer.h"
#include "TrackQueue.h"

namespace cspot {
struct Context;
struct TrackReference;

// Owns TrackQueue/TrackPlayer and position tracking - the actual audio
// control path. Never reports connect-state itself (no
// sendEngineEvent()/updatePlayerState() calls here): callers are notified
// via the constructor callbacks once this class's own state is already
// updated, and decide what (if anything) to report.
class PlaybackController {
 public:
  using TrackLoadedCallback =
      std::function<void(std::shared_ptr<QueuedTrack>, bool paused)>;
  using TrackReachedCallback =
      std::function<void(std::shared_ptr<QueuedTrack>)>;
  using DepletedCallback = std::function<void()>;

  PlaybackController(std::shared_ptr<cspot::Context> ctx,
                     TrackLoadedCallback onTrackLoaded,
                     TrackReachedCallback onTrackReached,
                     DepletedCallback onDepleted);

  std::shared_ptr<TrackPlayer> getTrackPlayer() { return trackPlayer; }
  std::shared_ptr<cspot::TrackQueue> getTrackQueue() { return trackQueue; }

  void loadTracks(const std::vector<TrackReference>& tracks, int startIndex,
                  uint32_t requestedPositionMs, bool startPaused);

  // Folds elapsed time into positionMs when going from playing to paused,
  // so getPositionMs() reflects "now" the instant it freezes.
  void setPlaybackPlaying(bool playing);
  uint32_t getPositionMs();
  bool isPlaying() const;

  bool skipTrack(TrackQueue::SkipDirection dir);
  bool nextSong();
  bool previousSong();
  void seekMs(uint32_t position);
  void setRepeatContext(bool repeat);

  // Engine-side reset for "nothing next" (isPlayingState/positionMs,
  // trackPlayer->resetState(true)) - caller still does its own PUT report.
  void reportEnded();

  // Another device took over (ClusterUpdate) - stop producing audio, but
  // keep TrackQueue's task alive since this device may become active
  // again later. See disconnect() for permanent teardown.
  void stop();

  // Permanent teardown (object destruction) - also stops TrackQueue's own
  // background task, unlike stop().
  void disconnect();

  // Feeds PutStateRequest.has_been_playing_for_ms (sendPutStateRequest()).
  // 0 means no track loaded yet this session.
  int64_t getCurrentTrackStartedAtMs() const;

 private:
  std::shared_ptr<cspot::Context> ctx;
  std::shared_ptr<cspot::TrackQueue> trackQueue;
  std::shared_ptr<TrackPlayer> trackPlayer;

  TrackReachedCallback onTrackReached;

  // Guards positionMs/positionMeasuredAt/isPlayingState/
  // currentTrackStartedAtMs only - not TrackQueue/TrackPlayer's own
  // internal state, which they protect themselves.
  mutable std::mutex positionMutex;
  uint32_t positionMs = 0;
  int64_t positionMeasuredAt = 0;
  bool isPlayingState = false;
  int64_t currentTrackStartedAtMs = 0;
};

}  // namespace cspot
