#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "PlaybackEvent.h"
#include "protobuf/connectstate.pb.h"

typedef struct cJSON cJSON;

namespace cspot {
class PlaybackController;
class ConnectStateModel;
class ContextResolver;
struct TrackReference;

// Decodes a hm://connect-state/v1/player/command request and executes it -
// translates a dealer command into PlaybackController/ConnectStateModel
// calls. Never owns the PUT machinery itself (no sendPutStateRequest()/
// pending-queue access): the handful of operations still private to
// ConnectStateHandler (putBufferingState/updatePlayerState/sendEngineEvent)
// come in as constructor callbacks, same DI shape as PutStateClient/
// PlaybackController use elsewhere.
class PlayerCommandHandler {
 public:
  using PutBufferingStateCallback =
      std::function<bool(const std::string& trackUri, uint32_t positionMs,
                         bool paused)>;
  using UpdatePlayerStateCallback = std::function<void(
      bool isPlaying, const std::string& trackUri, uint32_t positionMs,
      uint32_t durationMs, connectstate_PutStateReason reason,
      bool isBuffering)>;
  using SendEngineEventCallback = std::function<void(EventType type)>;
  using SendEngineEventDataCallback =
      std::function<void(EventType type, EventData data)>;

  PlayerCommandHandler(PlaybackController& playbackController,
                       ConnectStateModel& stateModel,
                       cspot::ContextResolver& contextResolver,
                       PutBufferingStateCallback putBufferingState,
                       UpdatePlayerStateCallback updatePlayerState,
                       SendEngineEventCallback sendEngineEvent,
                       SendEngineEventDataCallback sendEngineEventData);

  // Returns whether it succeeded (drives the {"success":...} reply) -
  // false for any endpoint not yet implemented.
  bool handlePlayerCommand(const std::string& endpoint, cJSON* command);

  void setPause(bool pause);
  bool nextSong();
  bool previousSong();
  void seekMs(uint32_t position);
  void setRepeatContext(bool repeat);

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
  void loadTracks(const std::vector<TrackReference>& tracks, int startIndex,
                  uint32_t requestedPositionMs, bool startPaused);

  PlaybackController& playbackController;
  ConnectStateModel& stateModel;
  cspot::ContextResolver& contextResolver;
  PutBufferingStateCallback putBufferingState;
  UpdatePlayerStateCallback updatePlayerState;
  SendEngineEventCallback sendEngineEvent;
  SendEngineEventDataCallback sendEngineEventData;
};

}  // namespace cspot
