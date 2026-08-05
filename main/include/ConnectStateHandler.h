#pragma once

// Protobufs
#include "api/ApClient.h"
#include "bell/Result.h"
#include "bell/utils/Task.h"
#include "connect.pb.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

#include "AudioSink.h"
#include "PlaybackNotifications.h"
#include "SessionContext.h"
#include "api/SpClient.h"
#include "events/EventModels.h"
#include "tracks/TrackQueueHandler.h"

namespace cspot {

// Owns a background task (runTask()) that sends every connect-state PUT.
// putState()/putStateLocked() only ever mutate putStateRequestProto and
// schedule a flush - never send inline - so the HTTPS round-trip never
// blocks a caller mutating state under putStateMutex.
class ConnectStateHandler : public bell::Task {
 public:
  ConnectStateHandler(
      std::shared_ptr<EventLoop> eventLoop, std::shared_ptr<AuthInfo> authInfo,
      std::shared_ptr<SpClient> spClient,
      std::shared_ptr<AudioSink> audioSink = std::make_shared<NullAudioSink>(),
      PlaybackNotificationCallback playbackNotificationCallback =
          [](const PlaybackNotificationEvent&) {});
  ~ConnectStateHandler() override;

  bell::Result<> handlePlayerCommand(tao::json::value& messageJson);

  bell::Result<> putState(
      PutStateReason reason = PutStateReason_PLAYER_STATE_CHANGED);

  // hm://connect-state/v1/cluster - detects another device becoming
  // active while we thought we were, stops local playback and PUTs
  // this device inactive. payloadDataStr is the base64 payload from the
  // dealer message's payloads[0].
  bell::Result<> handleClusterUpdate(std::string_view payloadDataStr);

  // hm://connect-state/v1/connect/volume - decodes a SetVolumeCommand
  // push, updates DeviceInfo.volume, calls audioSink->volumeChanged() and
  // PUTs VOLUME_CHANGED so the app sees it promptly. payloadDataStr is
  // the base64 payload from the dealer message's payloads[0].
  bell::Result<> handleSetVolume(std::string_view payloadDataStr);

  // Wired directly into StreamPlayer's constructor as its
  // PlayerStateAnnounceCallback - called synchronously, not through
  // EventLoop, so the PUT is dispatched before StreamPlayer does anything
  // else (in particular, before opening the CDN stream).
  void onPlayerStateUpdate(const PlayerStateUpdate& playerStateUpdate);

  // Tells the backend this device is going away, before it actually
  // disconnects.
  bell::Result<> putInactive();

  // --- Local control ---
  // Same commands a remote hm://connect-state/v1/player/command reaches
  // through handlePlayerCommand() - callable directly from any thread (a
  // GPIO button task, an app's own HTTP handler, ...), not just the
  // dealer's. Each one takes putStateMutex itself and calls the very same
  // ...Locked() method the matching remote endpoint does, so there's never
  // a second implementation of what "pause" or "skip" means. Safe to call
  // from any thread: none of these invoke any caller-supplied callback
  // while putStateMutex is held, so there's no reentrancy hazard to guard
  // against here (unlike the outward notification callback - see
  // PlaybackNotifications.h).
  bool requestPlayPause(bool play);
  bool requestNext();
  bool requestPrevious();
  bool requestSeek(uint32_t positionMs);
  bool requestSetRepeatContext(bool enabled);
  // 0 if nothing is loaded yet (no current track).
  uint32_t getPositionMs();

 private:
  const char* LOG_TAG = "ConnectStateHandler";

  std::shared_ptr<EventLoop> eventLoop;
  std::shared_ptr<AuthInfo> authInfo;
  std::shared_ptr<SpClient> spClient;
  std::shared_ptr<TrackQueueHandler> trackQueueHandler;
  std::shared_ptr<AudioSink> audioSink;
  PlaybackNotificationCallback playbackNotificationCallback;

  cspot_proto::PutStateRequest putStateRequestProto;

  // Rate-limits putState() (200ms min interval, "last reason wins"
  // coalescing). Also guards
  // putStateRequestProto/trackQueueHandler: every command/state handler
  // mutates them under this lock (EventLoop's thread), and
  // prepareAndEncodeLocked() reads them under the same lock (runTask()'s
  // thread) - never both at once. Not held during the network send
  // itself.
  std::mutex putStateMutex;
  std::condition_variable putStateCv;
  // nullopt until the first PUT is actually sent - putStateLocked() treats
  // that as "elapsed forever" (send immediately, skip the rate-limit wait).
  std::optional<std::chrono::steady_clock::time_point> lastPutStateTime;
  bool putStatePending = false;
  std::chrono::steady_clock::time_point putStateDueTime{};
  PutStateReason pendingPutStateReason = PutStateReason_PLAYER_STATE_CHANGED;

  // Timestamp of our own most recent transfer's playback state - guards
  // handleClusterUpdate() against a stale ClusterUpdate deactivating us
  // right after a legitimate transfer.
  int64_t lastTransferTimestamp = 0;

  // This device's own outgoing PutStateRequest.message_id sequence
  // number, incremented before every PUT (prepareAndEncodeLocked()).
  uint32_t nextMessageId = 0;

  // Counts consecutive TRACK_UNPLAYABLE signals with no successful load in
  // between
  int consecutiveUnplayableSkips = 0;

  // Assumes putStateMutex is ALREADY held by the caller - every handler
  // takes the lock for its own mutation and must call this instead of
  // the public, self-locking putState() (std::mutex isn't reentrant).
  //
  // Never sends the PUT itself - only schedules one for runTask() to
  // send.
  bell::Result<> putStateLocked(
      PutStateReason reason = PutStateReason_PLAYER_STATE_CHANGED);

  // Caller (runTask()) must hold putStateMutex. Finalizes this PUT's
  // per-send fields and encodes putStateRequestProto into outBody - the
  // only part of a flush that touches putStateRequestProto/
  // trackQueueHandler, so it must run under the lock. Does NOT do the
  // network send itself.
  bool prepareAndEncodeLocked(PutStateReason reason,
                              std::vector<std::byte>& outBody);

  void runTask() override;

  // Called by stopTask() after it has already set taskRunning = false -
  // wakes runTask()'s otherwise-unbounded wait() so shutdown doesn't
  // depend on a pending PUT's own due time or a next notify_one() from
  // putStateLocked().
  void wakeTask() override { putStateCv.notify_all(); }

  void initialize();

  // Extrapolates the current playback position from
  // playerState.positionAsOfTimestamp/timestamp/playbackSpeed as of `nowMs`
  // - the formula handlePauseCommandLocked()/handleSeekCommandLocked()/
  // getPositionMs() all need, factored out to one place. playbackSpeed is 0
  // while paused/buffering, so this is a no-op extrapolation in that case.
  // Assumes putStateMutex is ALREADY held by the caller.
  int64_t currentPositionMsLocked(int64_t nowMs) const;

  // Assumes putStateMutex is ALREADY held by the caller (handlePlayerCommand(),
  // its sole dispatcher) - matches putStateLocked()'s own naming/contract.
  bell::Result<> handleTransferCommandLocked(std::string_view payloadDataStr,
                                             const tao::json::value& options);

  // Assumes putStateMutex is ALREADY held by the caller (handlePlayerCommand()).
  bell::Result<> handlePlayCommandLocked(const tao::json::value& options);

  // Assumes putStateMutex is ALREADY held by the caller
  // (handlePlayerCommand() or the TRACK_ENDED handler - see
  // advanceToNextTrackLocked()'s own comment).
  bell::Result<> handleSkipNextCommandLocked();

  // Assumes putStateMutex is ALREADY held by the caller (handlePlayerCommand()).
  bell::Result<> handleSkipPrevCommandLocked();

  // Assumes putStateMutex is ALREADY held by the caller (handlePlayerCommand()).
  bell::Result<> handlePauseCommandLocked(bool pause);

  // Resolves seek_to's relative/beginning/absolute position from JSON,
  // then delegates to applySeekLocked() for the rest.
  // Assumes putStateMutex is ALREADY held by the caller (handlePlayerCommand()).
  bell::Result<> handleSeekCommandLocked(const tao::json::value& command);

  // The non-JSON half of handleSeekCommandLocked(): clamps targetPositionMs
  // to [0, duration], posts PLAYER_SEEK, updates
  // positionAsOfTimestamp/timestamp and PUTs the new position. Shared with
  // the local-control seek entry point, which already has a plain absolute
  // ms value and no JSON to parse.
  // Assumes putStateMutex is ALREADY held by the caller.
  bell::Result<> applySeekLocked(int64_t targetPositionMs);

  // hm://connect-state/v1/player/command's "update_context" endpoint -
  // context metadata/restrictions sync, not a new queue/context. Always
  // acks, even on a uri mismatch (just skips applying) - never NAKing
  // matters because an unacknowledged update_context can
  // trigger a Dealer WS reconnect. Restrictions/context_metadata aren't
  // captured - ConnectPb.h has no binding for those PlayerState fields
  // yet.
  //
  // Assumes putStateMutex is ALREADY held by the caller (handlePlayerCommand()).
  bell::Result<> handleUpdateContextCommandLocked(
      const tao::json::value& command);

  // Applies whichever of repeating_context/repeating_track/
  // shuffling_context are present (nullopt = not specified, leave
  // unchanged) - shared by player_options_override (play only -
  // handleTransferCommandLocked() doesn't read it) and the standalone
  // set_repeating_context/set_repeating_track/
  // set_shuffling_context/set_options commands, which all carry the same
  // three optional fields under different wire shapes. Does not PUT -
  // callers do that themselves. State-only for shuffle: syncs what the
  // client displays, doesn't reorder the queue - real shuffling isn't
  // implemented (TrackQueueHandler::enableShuffle() is still a stub).
  //
  // Assumes putStateMutex is ALREADY held by the caller (handlePlayerCommand()).
  void applyPlayerOptionsLocked(std::optional<bool> repeatingContext,
                                std::optional<bool> repeatingTrack,
                                std::optional<bool> shufflingContext);

  // set_repeating_context's applyPlayerOptionsLocked() + putStateLocked()
  // combo, factored out so the local-control entry point can reuse it
  // without going through JSON. repeatingContext: nullopt leaves the
  // existing value unchanged, matching applyPlayerOptionsLocked()'s own
  // semantics - see there for why endpoint's "value" being absent isn't
  // the same as "set to false".
  // Assumes putStateMutex is ALREADY held by the caller.
  bell::Result<> applyRepeatContextLocked(std::optional<bool> repeatingContext);

  // Shared by handleSkipNextCommandLocked() (remote skip_next) and
  // handleTrackAdvanceSignal() (TRACK_ENDED/TRACK_UNPLAYABLE) - all decide
  // "what's next" the same way: ask trackQueueHandler, refresh the
  // windows, and tell Spotify.
  //
  // forceNext=true (skip_next, or a track that could never load) always
  // advances, ignoring repeat-track. forceNext=false (natural end of
  // track) replays the current track instead when repeat-track is on.
  // Either way, running off the end of the context wraps the cursor to its
  // start; whether that counts as a real next track (vs. pausing there)
  // depends on repeat-context.
  //
  // Assumes putStateMutex is ALREADY held by the caller - each of its
  // callers takes it independently (they're separate dispatch entry
  // points, not nested calls of one another).
  bell::Result<> advanceToNextTrackLocked(bool forceNext);

  // Shared by the TRACK_ENDED and TRACK_UNPLAYABLE event handlers, neither
  // of which arrives with putStateMutex already held (unlike
  // handleSkipNextCommandLocked()'s handlePlayerCommand() dispatch path) -
  // takes the lock itself, then delegates to advanceToNextTrackLocked().
  // Also owns the consecutiveUnplayableSkips bookkeeping: within this
  // method forceNext==true always means "track was unplayable" (skip_next
  // never routes through here), so it's the one place that can tell the
  // two apart.
  void handleTrackAdvanceSignal(bool forceNext);

  bool encodeProtoTracks(pb_ostream_t* stream, const pb_field_t* field,
                         bool previous);

  static bool pbEncodeNextTracks(pb_ostream_t* stream, const pb_field_t* field,
                                 void* const* arg) {
    return static_cast<ConnectStateHandler*>(*arg)->encodeProtoTracks(
        stream, field, false);
  }

  static bool pbEncodePreviousTracks(pb_ostream_t* stream,
                                     const pb_field_t* field,
                                     void* const* arg) {
    return static_cast<ConnectStateHandler*>(*arg)->encodeProtoTracks(
        stream, field, true);
  }
};
}  // namespace cspot
