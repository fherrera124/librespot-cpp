#pragma once

#include <atomic>              // for atomic
#include <condition_variable>  // for condition_variable
#include <cstdint>             // for uint8_t
#include <functional>          // for function
#include <memory>              // for shared_ptr
#include <mutex>               // for mutex
#include <string>              // for string
#include <vector>              // for vector

#include "BellTask.h"    // for Task
#include "ConnectStateModel.h"
#include "ContextResolver.h"
#include "PlaybackController.h"
#include "PlaybackEvent.h"
#include "PutStateClient.h"
#include "protobuf/connectstate.pb.h"

typedef struct cJSON cJSON;

namespace cspot {
struct Context;
class Login5Client;
class TrackPlayer;
struct TrackReference;

// Publishes this device's connect-state to spclient, and owns the playback
// engine (TrackQueue/TrackPlayer, position tracking, command execution).
//
// A bell::Task of its own: updatePlayerState() can be called from whatever
// task fired the triggering event, some of them too small for a blocking
// TLS PUT. It only records the latest pending state and returns
// immediately; this class's own task does the actual PUT.
class ConnectStateHandler : public bell::Task {
 public:
  ConnectStateHandler(std::shared_ptr<cspot::Context> ctx,
                      std::shared_ptr<cspot::Login5Client> login5);
  ~ConnectStateHandler();

  // Stops the background publisher task.
  void stop();

  // Sets the x-spotify-connection-id that authorizes PUTs (from the Dealer
  // hello message).
  void setConnectionId(const std::string& connectionId);

  // PUTs the current device state to spclient. Synchronous - only call from
  // DealerClient's own task, at registration time.
  // isActive: Device.is_active, drives cluster pushes; defaults false
  // (registration-only PUTs).
  // Returns true on HTTP 200.
  bool putState(connectstate_PutStateReason reason, bool isActive = false);

  // Tells spclient this device stopped being active (PUT
  // /connect-state/v1/devices/{id}/inactive). Synchronous - only call from
  // DealerClient's task (cluster-update path).
  bool putStateInactive();

  // Records a player-state snapshot and wakes the background task to PUT
  // it. is_active is always true. Non-blocking, safe to call from any
  // task; only the latest snapshot survives if called again before the
  // previous one is sent.
  // trackUri: "spotify:track:..."/"spotify:episode:...", empty if unknown.
  // reason defaults to PLAYER_STATE_CHANGED.
  // isBuffering: true for the early "new track, not yet decoding"
  // announcement.
  void updatePlayerState(
      bool isPlaying, const std::string& trackUri, uint32_t positionMs,
      uint32_t durationMs,
      connectstate_PutStateReason reason =
          connectstate_PutStateReason_PLAYER_STATE_CHANGED,
      bool isBuffering = false);

  // Decodes a ClusterUpdate push and stops playback if we thought we were
  // the active device and it now names a different one.
  void handleClusterUpdate(const std::vector<uint8_t>& payload);

  // Decodes a SetVolumeCommand push and applies it.
  void handleSetVolume(const std::vector<uint8_t>& payload);

  // Executes a hm://connect-state/v1/player/command request. `command` is
  // the parsed "command" object, borrowed for the duration of the call.
  // Returns whether it succeeded (drives the {"success":...} reply) -
  // false for any endpoint not yet implemented.
  bool handlePlayerCommand(const std::string& endpoint, cJSON* command);

  // Records which remote command was handled last (message_id/
  // sent_by_device_id) so every subsequent PUT can echo
  // last_command_message_id/last_command_sent_by_device_id - required for
  // the requesting client to see its own command acknowledged.
  void setLastCommand(uint32_t messageId, const std::string& sentByDeviceId);

  // --- Playback engine ---

  std::shared_ptr<TrackPlayer> getTrackPlayer() {
    return playbackController.getTrackPlayer();
  }

  // Registers the callback for engine events (PLAY_PAUSE/TRACK_INFO/etc,
  // PlaybackEvent.h).
  void setEventHandler(EventHandler handler);

  // Loads a fresh track list starting at `startIndex`/`positionMs`,
  // replacing whatever was queued before. Playing state becomes "paused"
  // until the engine's own trackLoadedCallback fires with the real one.
  void loadTracks(const std::vector<TrackReference>& tracks, int startIndex,
                  uint32_t requestedPositionMs, bool startPaused);

  void setPause(bool pause);
  bool nextSong();
  bool previousSong();
  void seekMs(uint32_t position);
  void setRepeatContext(bool repeat);
  uint32_t getPositionMs();

  // Called from cspot_connect.cpp on EventType::DEPLETED - resets to a
  // clean stopped state and reports it.
  void notifyAudioEnded();

  void disconnect();

 protected:
  void runTask() override;

 private:
  // Decodes a "transfer" command's base64 TransferState (protobuf),
  // resolves the context it names, and loads it via loadTracks(). Returns
  // whether the transfer could be carried out at all (decode/resolve
  // failure only - "transferred to an empty context" isn't one).
  bool handleTransfer(cJSON* command);
  // Decodes a "play" command - context as plain JSON on the command
  // object (command.context.uri), not base64 protobuf. Fired when
  // starting new playback, as opposed to "transfer" resuming a session
  // already playing elsewhere.
  bool handlePlay(cJSON* command);
  // Synchronous PUT (is_buffering=true, is_playing=true), sent from
  // handlePlay()/handleTransfer() before loadTracks() kicks off the async
  // key/CDN fetch and before the command's WS reply goes out. trackUri
  // empty is tolerated.
  bool putBufferingState(const std::string& trackUri, uint32_t positionMs,
                         bool paused);
  void sendEngineEvent(EventType type);
  void sendEngineEvent(EventType type, EventData data);

  std::shared_ptr<cspot::Context> ctx;
  std::shared_ptr<cspot::Login5Client> login5;

  cspot::ContextResolver contextResolver;
  EventHandler engineEventHandler = nullptr;

  // Owns TrackQueue/TrackPlayer, position tracking, and the load/skip/
  // seek/pause control surface - see PlaybackController.h. Has its own
  // internal lock.
  PlaybackController playbackController;

  // Owns PlayerState/session_id/playback_id/context_uri/restrictions/
  // context_metadata - see ConnectStateModel.h. Has its own internal lock.
  ConnectStateModel stateModel;

  // Set whenever a PUT with is_active=true is sent - read by
  // handleClusterUpdate() to tell "someone else just took over" apart
  // from "we were never active to begin with".
  std::atomic<bool> isActiveDevice{false};
  // Epoch ms of when this device last became active - goes out as
  // PutStateRequest.started_playing_at on every PUT while active. 0 = not
  // active.
  std::atomic<uint64_t> activeSinceMs{0};

  // See setLastCommand(). Guarded by lastCommandMutex: written from
  // DealerClient's task, read from both PUT paths.
  std::mutex lastCommandMutex;
  uint32_t lastCommandMessageId = 0;
  std::string lastCommandSentByDeviceId;

  // Shared PUT-sending machinery (connection-id/token checks, DeviceInfo,
  // encode, HTTP PUT, status handling).
  bool sendPutStateRequest(connectstate_PutStateRequest& request);

  void buildDeviceInfo(connectstate_DeviceInfo& info);

  std::mutex connectionIdMutex;
  std::string connectionId;

  uint32_t messageId = 0;

  // Owns spclient host resolution/connection reuse/retry/rate-limit
  // tracking for put()/putInactive() - see PutStateClient.h. Serializes
  // its own sends end-to-end: putState() runs synchronously on
  // DealerClient's task while updatePlayerState()'s actual send runs on
  // this class's own task - two tasks that can call in concurrently.
  PutStateClient putStateClient;

  // Latest-wins pending player-state update, written by updatePlayerState()
  // (any task), consumed by runTask() (this class's own task).
  std::mutex pendingMutex;
  std::condition_variable pendingCv;
  bool hasPending = false;
  bool pendingIsPlaying = false;
  std::string pendingTrackUri;
  uint32_t pendingPositionMs = 0;
  uint32_t pendingDurationMs = 0;
  connectstate_PutStateReason pendingReason =
      connectstate_PutStateReason_PLAYER_STATE_CHANGED;
  bool pendingIsBuffering = false;

  // Held by runTask() for its whole lifetime; the destructor takes it
  // after stop(), so the object can't be freed under a still-running
  // task.
  std::mutex taskLifetimeMutex;

  std::atomic<bool> running{true};
};
}  // namespace cspot
