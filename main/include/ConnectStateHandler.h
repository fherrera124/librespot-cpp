#pragma once

// Protobufs
#include "api/ApClient.h"
#include "bell/Result.h"
#include "bell/utils/Task.h"
#include "connect.pb.h"

#include <chrono>
#include <functional>
#include <mutex>

#include "SessionContext.h"
#include "api/SpClient.h"
#include "tracks/TrackQueueHandler.h"

namespace cspot {

// Fires with the new 0..65535 connect-state volume whenever a
// SetVolumeCommand push is handled - the caller (Session, then whatever
// owns the actual audio sink) applies it to real output gain. Defaults
// to a no-op the same way Session's own AudioOutputCallback does, so
// callers that don't care about volume (host targets, tests) don't need
// to pass anything.
using VolumeChangedCallback = std::function<void(uint16_t)>;

// Owns its own background task (see taskLoop()) purely to flush a
// deferred PUT scheduled by putState()'s rate-limiting - see that
// method's comment.
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

 private:
  const char* LOG_TAG = "ConnectStateHandler";

  std::shared_ptr<EventLoop> eventLoop;
  std::shared_ptr<AuthInfo> authInfo;
  std::shared_ptr<SpClient> spClient;
  std::shared_ptr<TrackQueueHandler> trackQueueHandler;
  VolumeChangedCallback volumeChangedCallback;
  // std::shared_ptr<ApClient> apClient;

  // Holds the protobuf state
  cspot_proto::PutStateRequest putStateRequestProto;

  // putState() rate-limiting - matches go-librespot's statePutMinInterval/
  // updateState() and this repo's own master branch (same 200ms value,
  // "last reason wins" coalescing when a burst arrives inside the window).
  // Guards putStateRequestProto too, since flushStateNowLocked() runs
  // either inline on the caller's thread (EventLoop's dispatch task, for
  // every handler here) or from taskLoop() (this class's own background
  // task) for a deferred flush - never both at once.
  std::mutex putStateMutex;
  std::chrono::steady_clock::time_point lastPutStateTime =
      std::chrono::steady_clock::time_point::min();
  bool putStatePending = false;
  std::chrono::steady_clock::time_point putStateDueTime{};
  PutStateReason pendingPutStateReason = PutStateReason_PLAYER_STATE_CHANGED;

  // Timestamp of our own most recent transfer command's playback state -
  // guards handleClusterUpdate() against an out-of-order/stale
  // ClusterUpdate deactivating us right after a legitimate transfer to
  // this device (matches go-librespot's lastTransferTimestamp guard,
  // daemon/player.go).
  int64_t lastTransferTimestamp = 0;

  // Actually performs the PUT - caller must hold putStateMutex.
  bell::Result<> flushStateNowLocked(PutStateReason reason);

  void taskLoop() override;

  void initialize();

  bell::Result<> handleTransferCommand(std::string_view payloadDataStr,
                                       const tao::json::value& options);

  bell::Result<> handlePlayCommand(const tao::json::value& options);

  bell::Result<> handleSkipNextCommand();

  bell::Result<> handleSkipPrevCommand();

  bell::Result<> handlePauseCommand(bool pause);

  // Shared by handleSkipNextCommand() (remote skip_next) and the TRACK_ENDED
  // handler (StreamPlayer ran out of audio) - either way, "what's next" is
  // decided the same way: ask trackQueueHandler, refresh the windows, and
  // tell Spotify. Matches go-librespot's single advanceNext()
  // (daemon/controls.go), used from both its skip_next command and its
  // natural end-of-track event.
  bell::Result<> advanceToNextTrack();

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
