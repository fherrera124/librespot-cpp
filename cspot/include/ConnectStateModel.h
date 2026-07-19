#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "protobuf/connectstate.pb.h"

namespace cspot {

// Owns the connect-state session/player data: PlayerState, session/
// playback/context ids, restrictions, and context metadata. Knows nothing
// about HTTP or the playback engine - callers set what happened, and pull
// a snapshot to send.
class ConnectStateModel {
 public:
  // Generates the initial random session id and resets playerState to
  // go-librespot's State.reset() shape.
  ConnectStateModel();

  // Resets playerState (is_system_initiated/options/playback_speed set,
  // everything else zero). Does not touch sessionId/playbackId/contextUri.
  void reset();

  // Adopts the transferred session's id, or mints a fresh random one when
  // there is none.
  void adoptOrRegenerateSessionId(const char* transferredId);

  void setContextUri(const std::string& uri);
  std::string contextUri() const;

  void setRestrictions(std::string repeatContext, std::string repeatTrack,
                       std::string shuffle);
  void setContextMetadata(
      std::vector<std::pair<std::string, std::string>> metadata);

  void setPlaybackId(const std::string& id);

  // Sets the mechanical playback fields (is_playing/is_paused/
  // is_buffering/playback_speed/timestamp/position_as_of_timestamp/
  // duration/track). durationMs: nullopt leaves the existing duration
  // untouched (not yet known, e.g. an early "loading" announcement).
  void setPlaybackState(bool isPlaying, bool isPaused, bool isBuffering,
                        int64_t timestampMs, uint32_t positionMs,
                        std::optional<uint32_t> durationMs,
                        const std::string& trackUri);

  // Current track uri/duration.
  std::string trackUri() const;
  uint32_t duration() const;

  // Copies playerState + session_id/playback_id/context_uri/restrictions/
  // context_metadata into `request`. Caller must already have
  // request.device.has_player_state = true set.
  void fillIntoRequest(connectstate_PutStateRequest& request) const;

 private:
  mutable std::mutex mutex;
  connectstate_PlayerState playerState = connectstate_PlayerState_init_zero;
  std::string sessionId;
  std::string playbackId;
  std::string contextUriValue;
  std::string restrictionRepeatContext;
  std::string restrictionRepeatTrack;
  std::string restrictionShuffle;
  std::vector<std::pair<std::string, std::string>> contextMetadata;
};

}  // namespace cspot
