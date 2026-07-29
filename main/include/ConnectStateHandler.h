#pragma once

// Protobufs
#include "api/ApClient.h"
#include "bell/Result.h"
#include "bell/utils/Task.h"
#include "connect.pb.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

#include "SessionContext.h"
#include "api/SpClient.h"
#include "events/EventModels.h"
#include "tracks/TrackQueueHandler.h"

namespace cspot {

// Fires with the new 0..65535 connect-state volume whenever a
// SetVolumeCommand push is handled. Defaults to a no-op so callers that
// don't care about volume (host targets, tests) don't need to pass
// anything.
using VolumeChangedCallback = std::function<void(uint16_t)>;

// Owns a background task (runTask()) that sends every connect-state PUT.
// putState()/putStateLocked() only ever mutate putStateRequestProto and
// schedule a flush - never send inline - so the HTTPS round-trip never
// blocks a caller mutating state under putStateMutex.
class ConnectStateHandler : public bell::Task {
 public:
  ConnectStateHandler(
      std::shared_ptr<EventLoop> eventLoop, std::shared_ptr<AuthInfo> authInfo,
      std::shared_ptr<SpClient> spClient,
      VolumeChangedCallback volumeChangedCallback = [](uint16_t) {});
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
  // push, updates DeviceInfo.volume, fires volumeChangedCallback and
  // PUTs VOLUME_CHANGED so the app sees it promptly. payloadDataStr is
  // the base64 payload from the dealer message's payloads[0].
  bell::Result<> handleSetVolume(std::string_view payloadDataStr);

  // Wired directly into StreamPlayer's constructor as its
  // PlayerStateAnnounceCallback - called synchronously, not through
  // EventLoop, so the PUT is dispatched before StreamPlayer does anything
  // else (in particular, before opening the CDN stream).
  void onPlayerStateUpdate(const PlayerStateUpdate& playerStateUpdate);

  // Tells the backend this device is going away, before it actually
  // disconnects - matches master's own PlayerEngine::putStateInactive().
  bell::Result<> putInactive();

 private:
  const char* LOG_TAG = "ConnectStateHandler";

  std::shared_ptr<EventLoop> eventLoop;
  std::shared_ptr<AuthInfo> authInfo;
  std::shared_ptr<SpClient> spClient;
  std::shared_ptr<TrackQueueHandler> trackQueueHandler;
  VolumeChangedCallback volumeChangedCallback;

  cspot_proto::PutStateRequest putStateRequestProto;

  // Rate-limits putState() (200ms min interval, "last reason wins"
  // coalescing - matches go-librespot/master). Also guards
  // putStateRequestProto/trackQueueHandler: every command/state handler
  // mutates them under this lock (EventLoop's thread), and
  // prepareAndEncodeLocked() reads them under the same lock (runTask()'s
  // thread) - never both at once. Not held during the network send
  // itself.
  std::mutex putStateMutex;
  std::condition_variable putStateCv;
  std::chrono::steady_clock::time_point lastPutStateTime =
      std::chrono::steady_clock::time_point::min();
  // Short-circuits putState()'s rate-limit check on the very first call in
  // this object's lifetime: `now - lastPutStateTime` would be a signed
  // overflow otherwise (lastPutStateTime starts at time_point::min()),
  // which silently wraps to a huge negative duration in an unsanitized
  // build instead of the intended "elapsed forever" value.
  bool hasEverSentPutState = false;
  bool putStatePending = false;
  std::chrono::steady_clock::time_point putStateDueTime{};
  PutStateReason pendingPutStateReason = PutStateReason_PLAYER_STATE_CHANGED;

  // Timestamp of our own most recent transfer's playback state - guards
  // handleClusterUpdate() against a stale ClusterUpdate deactivating us
  // right after a legitimate transfer (matches go-librespot's
  // lastTransferTimestamp guard).
  int64_t lastTransferTimestamp = 0;

  // This device's own outgoing PutStateRequest.message_id sequence
  // number, incremented before every PUT (prepareAndEncodeLocked()).
  uint32_t nextMessageId = 0;

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

  void initialize();

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

  // hm://connect-state/v1/player/command's "update_context" endpoint -
  // context metadata/restrictions sync, not a new queue/context. Always
  // acks, even on a uri mismatch (just skips applying) - matches master;
  // never NAKing matters because an unacknowledged update_context can
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

  // Shared by handleSkipNextCommandLocked() (remote skip_next) and the
  // TRACK_ENDED handler (StreamPlayer ran out of audio) - both decide
  // "what's next" the same way: ask trackQueueHandler, refresh the
  // windows, and tell Spotify. Matches go-librespot's single
  // advanceNext().
  //
  // forceNext=true (skip_next) always advances, ignoring repeat-track.
  // forceNext=false (natural end of track) replays the current track
  // instead when repeat-track is on. Either way, running off the end of
  // the context wraps the cursor to its start; whether that counts as a
  // real next track (vs. pausing there) depends on repeat-context -
  // matches go-librespot's advanceNext(ctx, forceNext, drop).
  //
  // Assumes putStateMutex is ALREADY held by the caller - each of the two
  // callers above takes it independently (they're two separate dispatch
  // entry points, not nested calls of one another).
  bell::Result<> advanceToNextTrackLocked(bool forceNext);

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
