#include "PlayerStateModel.h"

#include "Crypto.h"        // for Crypto::base64Encode
#include "NanoPBHelper.h"  // for pbPutString

using namespace cspot;

namespace {
std::string generateSessionId() {
  Crypto crypto;
  return Crypto::base64Encode(crypto.generateVectorWithRandomData(16));
}
}  // namespace

PlayerStateModel::PlayerStateModel() {
  sessionId = generateSessionId();
  reset();
}

void PlayerStateModel::reset() {
  std::lock_guard<std::mutex> lock(mutex);
  playerState = connectstate_PlayerState_init_zero;
  playerState.is_system_initiated = true;
  playerState.has_options = true;
  playerState.playback_speed = 1.0;
}

void PlayerStateModel::adoptOrRegenerateSessionId(const char* transferredId) {
  std::lock_guard<std::mutex> lock(mutex);
  if (transferredId != nullptr && transferredId[0] != '\0') {
    sessionId = transferredId;
  } else {
    sessionId = generateSessionId();
  }
}

void PlayerStateModel::setContextUri(const std::string& uri) {
  std::lock_guard<std::mutex> lock(mutex);
  contextUriValue = uri;
}

std::string PlayerStateModel::contextUri() const {
  std::lock_guard<std::mutex> lock(mutex);
  return contextUriValue;
}

void PlayerStateModel::setRestrictions(std::string repeatContext,
                                        std::string repeatTrack,
                                        std::string shuffle) {
  std::lock_guard<std::mutex> lock(mutex);
  restrictionRepeatContext = std::move(repeatContext);
  restrictionRepeatTrack = std::move(repeatTrack);
  restrictionShuffle = std::move(shuffle);
}

void PlayerStateModel::setContextMetadata(
    std::vector<std::pair<std::string, std::string>> metadata) {
  std::lock_guard<std::mutex> lock(mutex);
  contextMetadata = std::move(metadata);
}

void PlayerStateModel::setPlaybackId(const std::string& id) {
  std::lock_guard<std::mutex> lock(mutex);
  playbackId = id;
}

void PlayerStateModel::setPlaybackState(bool isPlaying, bool isPaused,
                                         bool isBuffering, int64_t timestampMs,
                                         uint32_t positionMs,
                                         std::optional<uint32_t> durationMs,
                                         const std::string& trackUri) {
  std::lock_guard<std::mutex> lock(mutex);
  playerState.is_playing = isPlaying;
  playerState.is_paused = isPaused;
  playerState.is_buffering = isBuffering;
  playerState.playback_speed = (isPlaying && !isBuffering) ? 1.0 : 0.0;
  playerState.timestamp = timestampMs;
  playerState.position_as_of_timestamp = (int64_t)positionMs;
  if (durationMs.has_value()) {
    playerState.duration = (int64_t)*durationMs;
  }
  // Unconditional either way - a live, reused struct leaks a stale track
  // forward if this only ever sets the true case.
  playerState.has_track = !trackUri.empty();
  if (playerState.has_track) {
    pbPutString(trackUri, playerState.track.uri);
  }
}

std::string PlayerStateModel::trackUri() const {
  std::lock_guard<std::mutex> lock(mutex);
  return std::string(playerState.track.uri);
}

uint32_t PlayerStateModel::duration() const {
  std::lock_guard<std::mutex> lock(mutex);
  return (uint32_t)playerState.duration;
}

void PlayerStateModel::fillIntoRequest(
    connectstate_PutStateRequest& request) const {
  std::lock_guard<std::mutex> lock(mutex);
  request.device.player_state = playerState;
  pbPutString(sessionId, request.device.player_state.session_id);
  if (!playbackId.empty()) {
    pbPutString(playbackId, request.device.player_state.playback_id);
  }
  if (!contextUriValue.empty()) {
    pbPutString(contextUriValue, request.device.player_state.context_uri);
  }

  if (!restrictionRepeatContext.empty() || !restrictionRepeatTrack.empty() ||
      !restrictionShuffle.empty()) {
    request.device.player_state.has_context_restrictions = true;
    auto& restrictions = request.device.player_state.context_restrictions;
    pbPutString(restrictionRepeatContext,
                restrictions.disallow_toggling_repeat_context_reasons);
    pbPutString(restrictionRepeatTrack,
                restrictions.disallow_toggling_repeat_track_reasons);
    pbPutString(restrictionShuffle,
                restrictions.disallow_toggling_shuffle_reasons);
  }
  request.device.player_state.context_metadata_count =
      (pb_size_t)contextMetadata.size();
  for (size_t i = 0; i < contextMetadata.size(); i++) {
    pbPutString(contextMetadata[i].first,
                request.device.player_state.context_metadata[i].key);
    pbPutString(contextMetadata[i].second,
                request.device.player_state.context_metadata[i].value);
  }
}
