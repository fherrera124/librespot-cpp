#include "ConnectStateHandler.h"

#include <algorithm>  // for min
#include <chrono>     // for milliseconds, steady_clock
#include <cstring>    // for strcpy
#include <exception>  // for exception
#include <utility>    // for pair
#include <vector>     // for vector

#include "ApResolve.h"
#include "BellLogger.h"  // for AbstractLogger
#include "BellUtils.h"   // for BELL_SLEEP_MS
#include "CSpotContext.h"  // for Context
#include "Crypto.h"         // for Crypto::base64Encode
#include "HTTPClient.h"    // for HTTPClient
#include "HttpRetry.h"     // for HttpRetry, PermanentHttpFailure
#include "Login5Client.h"
#include "Logger.h"        // for CSPOT_LOG
#include "NanoPBHelper.h"  // for pbEncode, pbDecode
#include "TimeProvider.h"
#include "TrackPlayer.h"    // for TrackPlayer
#include "TrackReference.h"  // for TrackReference (fillProvidedTracks)
#include "Utils.h"          // for bytesToHexString
#include "pb_decode.h"  // for pb_release

using namespace cspot;

namespace {
const char* CLIENT_ID_HEX = "65b708073fc0480ea92a077233ca87bd";
const char* SPIRC_VERSION = "3.2.6";
constexpr int PENDING_WAIT_MS = 500;
// §6.6: same coalescing interval go-librespot uses (statePutMinInterval) -
// caps how often updatePlayerState() actually reaches the network, however
// fast SpircHandler's events fire.
constexpr int PUT_MIN_INTERVAL_MS = 200;

// Spotify always sends Retry-After in delta-seconds, never an HTTP-date
// (developer.spotify.com/documentation/web-api/concepts/rate-limits) - only
// that form is parsed. Missing/malformed falls back to a default.
std::chrono::seconds parseRetryAfter(std::string_view value) {
  constexpr std::chrono::seconds DEFAULT_RETRY_AFTER{10};
  if (value.empty()) {
    return DEFAULT_RETRY_AFTER;
  }
  try {
    int secs = std::stoi(std::string(value));
    return secs > 0 ? std::chrono::seconds(secs) : DEFAULT_RETRY_AFTER;
  } catch (const std::exception&) {
    return DEFAULT_RETRY_AFTER;
  }
}
}  // namespace

ConnectStateHandler::ConnectStateHandler(
    std::shared_ptr<cspot::Context> ctx,
    std::shared_ptr<cspot::Login5Client> login5)
    : bell::Task("cspotConnectState", 32 * 1024, 1, 0), ctx(ctx),
      login5(login5), contextResolver(ctx, login5) {
  // See the member comment (ConnectStateHandler.h) - go-librespot always
  // sends a non-empty PlayerState.session_id; we never did.
  {
    Crypto crypto;
    sessionId = Crypto::base64Encode(crypto.generateVectorWithRandomData(16));
  }

  resetPlayerState();

  trackQueue = std::make_shared<cspot::TrackQueue>(ctx);

  auto eofCallback = [this]() {
    if (trackQueue->isFinished()) {
      // Repeat-context (F92): loop back to the first track instead of
      // ending. See TrackQueue::restartFromBeginning().
      if (trackQueue->isRepeatingContext()) {
        trackQueue->restartFromBeginning();
      } else {
        sendEngineEvent(EventType::DEPLETED);
      }
    }
  };

  auto trackLoadedCallback = [this](std::shared_ptr<QueuedTrack> track,
                                    bool paused = false) {
    {
      std::lock_guard<std::mutex> lock(engineMutex);
      isPlayingState = !paused;
      positionMs = track->requestedPosition;
      positionMeasuredAt = this->ctx->timeProvider->getSyncedTimestamp();
      currentTrackStartedAtMs = positionMeasuredAt;
    }
    sendEngineEvent(EventType::PLAYBACK_START, (int)track->requestedPosition);
    sendEngineEvent(EventType::PLAY_PAUSE, paused);
  };

  auto reachedPlaybackCallback = [this](std::string_view trackId) {
    notifyAudioReachedPlayback(std::string(trackId));
  };

  trackPlayer = std::make_shared<TrackPlayer>(ctx, trackQueue, eofCallback,
                                              trackLoadedCallback,
                                              reachedPlaybackCallback);
  // Unlike SpircHandler (which lazily started this on the first Load
  // frame), ConnectStateHandler's engine is meant to just be ready - there's
  // no separate "session established" moment to defer to.
  trackPlayer->start();

  startTask();
}

ConnectStateHandler::~ConnectStateHandler() {
  stop();
  // Blocks until runTask() releases it - never free this object under a
  // still-running task (F93 pattern, same as DealerClient/MercurySession).
  std::scoped_lock lock(taskLifetimeMutex);
}

void ConnectStateHandler::stop() {
  running = false;
  pendingCv.notify_all();
}

void ConnectStateHandler::setConnectionId(const std::string& id) {
  std::lock_guard<std::mutex> lock(connectionIdMutex);
  connectionId = id;
}

void ConnectStateHandler::buildDeviceInfo(connectstate_DeviceInfo& info) {
  info.can_play = true;
  info.volume = ctx->config.volume;  // cspot volume is 0..65535, same as
                                      // connect-state (player.MaxStateVolume)
  pbPutString(ctx->config.deviceName, info.name);
  info.device_type = connectstate_DeviceType_SPEAKER;
  pbPutCharArray("cspot 1.0", info.device_software_version);
  pbPutCharArray(SPIRC_VERSION, info.spirc_version);
  pbPutString(ctx->config.deviceId, info.device_id);
  pbPutCharArray(CLIENT_ID_HEX, info.client_id);

  info.has_capabilities = true;
  auto& caps = info.capabilities;
  caps.can_be_player = true;
  caps.gaia_eq_connect_id = true;
  // go-librespot ties this to whether Zeroconf pairing is enabled
  // (cfg.ZeroconfEnabled) - cspot's Zeroconf pairing (LoginBlob::
  // loadZeroconf) is unconditional, not behind a toggle, so this is too.
  caps.supports_logout = true;
  caps.is_observable = true;
  caps.volume_steps = 64;
  caps.command_acks = true;
  caps.is_controllable = true;
  caps.supports_transfer_command = true;
  caps.supports_command_request = true;
  caps.supports_playlist_v2 = true;
  // False until gzip decoding is supported (§6.3) - keeps cluster updates
  // uncompressed so DealerClient can read them.
  caps.supports_gzip_pushes = false;
  caps.supported_types_count = 2;
  strcpy(caps.supported_types[0], "audio/track");
  strcpy(caps.supported_types[1], "audio/episode");
}

bool ConnectStateHandler::sendPutStateRequest(
    connectstate_PutStateRequest& request) {
  // TEMP DIAGNOSTIC (playlist-switch flicker investigation, 2026-07-18):
  // dump exactly what this PUT's PlayerState carries, to rule out stale
  // track/position data leaving the device. Remove once resolved.
  CSPOT_LOG(info,
           "PUT DIAG: reason=%d is_active=%d is_playing=%d is_paused=%d "
           "is_buffering=%d has_track=%d track.uri=%s pos_as_of_ts=%lld",
           (int)request.put_state_reason, (int)request.is_active,
           (int)request.device.player_state.is_playing,
           (int)request.device.player_state.is_paused,
           (int)request.device.player_state.is_buffering,
           (int)request.device.player_state.has_track,
           request.device.player_state.has_track
               ? request.device.player_state.track.uri
               : "-",
           (long long)request.device.player_state.position_as_of_timestamp);

  std::string connId;
  {
    std::lock_guard<std::mutex> lock(connectionIdMutex);
    connId = connectionId;
  }
  if (connId.empty()) {
    CSPOT_LOG(error, "connect-state PUT skipped: no connection id yet");
    return false;
  }

  auto accessToken = login5->getToken();
  auto clientToken = login5->getClientToken();
  if (accessToken.empty() || clientToken.empty()) {
    CSPOT_LOG(error, "connect-state PUT skipped: missing token(s)");
    return false;
  }

  request.member_type = connectstate_MemberType_CONNECT_STATE;
  request.client_side_timestamp = ctx->timeProvider->getSyncedTimestamp();
  request.message_id = ++messageId;

  // Command correlation (setLastCommand()): the requesting client matches
  // the cluster state it gets back against the command it sent via these
  // two fields - both reference clients send them on every PUT.
  {
    std::lock_guard<std::mutex> lock(lastCommandMutex);
    if (!lastCommandSentByDeviceId.empty()) {
      request.last_command_message_id = lastCommandMessageId;
      pbPutString(lastCommandSentByDeviceId,
                  request.last_command_sent_by_device_id);
    }
  }

  // Epoch ms since this device became active - server uses it to order
  // devices (both reference clients send it).
  uint64_t activeSince = activeSinceMs;
  if (activeSince != 0) {
    request.started_playing_at = activeSince;
  }

  // Set from trackLoadedCallback (constructor) whenever a track loads -
  // mirrors go-librespot's Player.HasBeenPlayingFor().
  int64_t trackStartedAt;
  {
    std::lock_guard<std::mutex> lock(engineMutex);
    trackStartedAt = currentTrackStartedAtMs;
  }
  if (trackStartedAt != 0) {
    request.has_been_playing_for_ms =
        (uint64_t)(ctx->timeProvider->getSyncedTimestamp() - trackStartedAt);
  }

  // update_context's payload (§36) - written by handlePlayerCommand()'s
  // "update_context" case.
  std::string repeatContextReason, repeatTrackReason, shuffleReason;
  std::vector<std::pair<std::string, std::string>> metadata;
  {
    std::lock_guard<std::mutex> lock(engineMutex);
    repeatContextReason = restrictionRepeatContext;
    repeatTrackReason = restrictionRepeatTrack;
    shuffleReason = restrictionShuffle;
    metadata = contextMetadata;
  }
  if (!repeatContextReason.empty() || !repeatTrackReason.empty() ||
      !shuffleReason.empty()) {
    request.device.player_state.has_context_restrictions = true;
    auto& restrictions = request.device.player_state.context_restrictions;
    pbPutString(repeatContextReason,
                restrictions.disallow_toggling_repeat_context_reasons);
    pbPutString(repeatTrackReason,
                restrictions.disallow_toggling_repeat_track_reasons);
    pbPutString(shuffleReason, restrictions.disallow_toggling_shuffle_reasons);
  }
  request.device.player_state.context_metadata_count =
      (pb_size_t)metadata.size();
  for (size_t i = 0; i < metadata.size(); i++) {
    pbPutString(metadata[i].first,
                request.device.player_state.context_metadata[i].key);
    pbPutString(metadata[i].second,
                request.device.player_state.context_metadata[i].value);
  }

  // Queue display ("playing next"/"previously played") - only uri is
  // populated (TrackReference has nothing else ProvidedTrack could use).
  auto fillProvidedTracks = [](const std::vector<TrackReference>& src,
                               connectstate_ProvidedTrack* dst,
                               pb_size_t& count, size_t maxCount) {
    count = (pb_size_t)std::min(src.size(), maxCount);
    for (pb_size_t i = 0; i < count; i++) {
      pbPutString(src[i].uri, dst[i].uri);
    }
  };
  fillProvidedTracks(trackQueue->getPrevTracks(3),
                     request.device.player_state.prev_tracks,
                     request.device.player_state.prev_tracks_count, 3);
  fillProvidedTracks(trackQueue->getNextTracks(3),
                     request.device.player_state.next_tracks,
                     request.device.player_state.next_tracks_count, 3);

  // DeviceInfo travels on every PUT, not just registration.
  request.has_device = true;
  request.device.has_device_info = true;
  buildDeviceInfo(request.device.device_info);

  std::vector<uint8_t> body;
  try {
    body = pbEncode(connectstate_PutStateRequest_fields, &request);
  } catch (const std::exception& e) {
    CSPOT_LOG(error, "connect-state encode failed: %s", e.what());
    return false;
  }

  std::scoped_lock lock(putMutex);

  // Bounded retry (2 attempts, 1s apart): 4xx is permanent (no retry), a
  // dropped connection or 5xx is transient. putConnection/spclientHost are
  // reset only on a genuine transport exception, never on a mere non-200 -
  // see the member comment (ConnectStateHandler.h).
  try {
    return HttpRetry(2, std::chrono::milliseconds(1000), "connect-state PUT")
        .run([&]() -> bool {
          if (spclientHost.empty()) {
            spclientHost = ApResolve("").fetchFirstSpclientAddress();
            contextResolver.seedSpclientHost(spclientHost);
          }
          auto url = "https://" + spclientHost + "/connect-state/v1/devices/" +
                     ctx->config.deviceId;

          bell::HTTPClient::Headers headers = {
              {"Authorization", "Bearer " + accessToken},
              {"Client-Token", clientToken},
              {"X-Spotify-Connection-Id", connId},
              {"Content-Type", "application/x-protobuf"}};

          try {
            if (putConnection == nullptr) {
              putConnection = bell::HTTPClient::put(url, headers, body);
            } else {
              putConnection->put(url, headers, body);
            }
          } catch (const std::exception& e) {
            putConnection.reset();
            spclientHost.clear();
            throw std::runtime_error(std::string("request failed: ") +
                                     e.what());
          }

          int status = putConnection->statusCode();
          std::string responseBody(putConnection->body());
          if (status == 200) {
            CSPOT_LOG(info, "connect-state PUT ok (reason %d)",
                     (int)request.put_state_reason);
            return true;
          }

          std::string reason =
              "status " + std::to_string(status) + ": " + responseBody;
          if (status == 429) {
            throw RateLimitedError(
                parseRetryAfter(putConnection->header("retry-after")),
                reason);
          }
          if (status >= 400 && status < 500) {
            throw PermanentHttpFailure(reason);
          }
          throw std::runtime_error(reason);
        });
  } catch (const RateLimitedError& e) {
    rateLimitedUntil = std::chrono::steady_clock::now() + e.retryAfter;
    CSPOT_LOG(error, "connect-state PUT rate-limited, backing off %llds",
             (long long)e.retryAfter.count());
    return false;
  } catch (const std::exception&) {
    return false;  // HttpRetry already logged the final giving-up message
  }
}

bool ConnectStateHandler::putStateInactive() {
  std::string connId;
  {
    std::lock_guard<std::mutex> lock(connectionIdMutex);
    connId = connectionId;
  }
  auto accessToken = login5->getToken();
  auto clientToken = login5->getClientToken();
  if (connId.empty() || accessToken.empty() || clientToken.empty()) {
    CSPOT_LOG(error, "inactive PUT skipped: missing connection id/token(s)");
    return false;
  }

  std::scoped_lock lock(putMutex);
  try {
    if (spclientHost.empty()) {
      spclientHost = ApResolve("").fetchFirstSpclientAddress();
    }
    // notify=false, matching what go-librespot's stopPlayback() passes.
    auto url = "https://" + spclientHost + "/connect-state/v1/devices/" +
               ctx->config.deviceId + "/inactive?notify=false";
    bell::HTTPClient::Headers headers = {
        {"Authorization", "Bearer " + accessToken},
        {"Client-Token", clientToken},
        {"X-Spotify-Connection-Id", connId}};

    if (putConnection == nullptr) {
      putConnection = bell::HTTPClient::put(url, headers, {});
    } else {
      putConnection->put(url, headers, {});
    }

    int status = putConnection->statusCode();
    (void)putConnection->body();  // drain - see PUT above, never logged here
    // 204 expected (go-librespot checks exactly that); tolerate any 2xx.
    if (status >= 200 && status < 300) {
      CSPOT_LOG(info, "connect-state inactive PUT ok (%d)", status);
      return true;
    }
    CSPOT_LOG(error, "connect-state inactive PUT failed, status %d", status);
    return false;
  } catch (const std::exception& e) {
    putConnection.reset();
    spclientHost.clear();
    CSPOT_LOG(error, "connect-state inactive PUT failed: %s", e.what());
    return false;
  }
}

void ConnectStateHandler::adoptOrRegenerateSessionId(
    const char* transferredId) {
  std::lock_guard<std::mutex> lock(engineMutex);
  if (transferredId != nullptr && transferredId[0] != '\0') {
    sessionId = transferredId;
  } else {
    Crypto crypto;
    sessionId = Crypto::base64Encode(crypto.generateVectorWithRandomData(16));
  }
}

void ConnectStateHandler::resetPlayerState() {
  std::lock_guard<std::mutex> lock(engineMutex);
  playerState = connectstate_PlayerState_init_zero;
  playerState.is_system_initiated = true;
  playerState.has_options = true;
  playerState.playback_speed = 1.0;
}

bool ConnectStateHandler::putState(connectstate_PutStateReason reason,
                                   bool isActive) {
  connectstate_PutStateRequest request = connectstate_PutStateRequest_init_zero;
  request.put_state_reason = reason;
  request.is_active = isActive;

  // Minimal PlayerState (matches go-librespot's State.reset): enough for a
  // device that isn't playing anything yet.
  request.device.has_player_state = true;
  {
    std::lock_guard<std::mutex> lock(engineMutex);
    request.device.player_state = playerState;
    pbPutString(sessionId, request.device.player_state.session_id);
  }

  if (isActive) {
    // Only stamp on the false->true transition - re-stamping every PUT
    // would drift started_playing_at forward.
    if (!isActiveDevice.exchange(true)) {
      activeSinceMs = ctx->timeProvider->getSyncedTimestamp();
    }
  }
  return sendPutStateRequest(request);
}

bool ConnectStateHandler::putBufferingState(const std::string& trackUri,
                                            uint32_t positionMs,
                                            bool paused) {
  connectstate_PutStateRequest request = connectstate_PutStateRequest_init_zero;
  request.put_state_reason = connectstate_PutStateReason_PLAYER_STATE_CHANGED;
  // Same "genuinely playing something" gate as runTask()'s PUT below.
  request.is_active = true;
  if (!isActiveDevice.exchange(true)) {
    activeSinceMs = ctx->timeProvider->getSyncedTimestamp();
  }

  request.device.has_player_state = true;
  // playback_id deliberately not set here - not known until the stream
  // actually opens.
  {
    std::lock_guard<std::mutex> lock(engineMutex);
    playerState.is_playing = true;
    playerState.is_paused = paused;
    playerState.is_buffering = true;
    playerState.playback_speed = 0;  // not progressing while buffering
    playerState.timestamp = ctx->timeProvider->getSyncedTimestamp();
    playerState.position_as_of_timestamp = (int64_t)positionMs;
    // Unconditional either way - a live, reused struct leaks a stale track
    // forward if this only ever sets the true case. See the member comment.
    playerState.has_track = !trackUri.empty();
    if (playerState.has_track) {
      pbPutString(trackUri, playerState.track.uri);
    }

    request.device.player_state = playerState;
    pbPutString(sessionId, request.device.player_state.session_id);
    if (!contextUri.empty()) {
      pbPutString(contextUri, request.device.player_state.context_uri);
    }
  }

  return sendPutStateRequest(request);
}

void ConnectStateHandler::updatePlayerState(bool isPlaying,
                                            const std::string& trackUri,
                                            uint32_t positionMs,
                                            uint32_t durationMs,
                                            connectstate_PutStateReason reason) {
  // Non-blocking: the actual PUT runs on this class's own task (runTask()),
  // never the caller's - doing it inline overflowed small caller stacks.
  std::lock_guard<std::mutex> lock(pendingMutex);
  hasPending = true;
  pendingIsPlaying = isPlaying;
  pendingTrackUri = trackUri;
  pendingPositionMs = positionMs;
  pendingDurationMs = durationMs;
  pendingReason = reason;
  pendingCv.notify_one();
}

void ConnectStateHandler::runTask() {
  std::scoped_lock lifetimeLock(taskLifetimeMutex);

  // §6.6 rate-limiting: default-constructed = epoch, so the very first
  // update is never held back.
  auto lastPutTime = std::chrono::steady_clock::time_point{};

  while (running) {
    bool isPlaying;
    std::string trackUri;
    uint32_t positionMs, durationMs;
    connectstate_PutStateReason reason;
    {
      std::unique_lock<std::mutex> lock(pendingMutex);
      pendingCv.wait_for(lock, std::chrono::milliseconds(PENDING_WAIT_MS),
                        [this] { return hasPending || !running; });
      if (!running) {
        break;
      }
      if (!hasPending) {
        continue;
      }
      isPlaying = pendingIsPlaying;
      trackUri = pendingTrackUri;
      positionMs = pendingPositionMs;
      durationMs = pendingDurationMs;
      reason = pendingReason;
      hasPending = false;
    }

    // §6.6: at most one PUT every PUT_MIN_INTERVAL_MS, coalescing a burst
    // of events into the latest pending state - also honors rateLimitedUntil
    // (set by sendPutStateRequest() from a 429). Re-check for an even
    // fresher pending update that arrived during the wait.
    auto now = std::chrono::steady_clock::now();
    auto minIntervalWait = std::chrono::milliseconds(PUT_MIN_INTERVAL_MS) -
                           std::chrono::duration_cast<std::chrono::milliseconds>(
                               now - lastPutTime);
    auto rateLimitWait = std::chrono::duration_cast<std::chrono::milliseconds>(
        rateLimitedUntil.load() - now);
    auto wait = std::max({minIntervalWait, rateLimitWait,
                          std::chrono::milliseconds(0)});
    if (wait > std::chrono::milliseconds(0)) {
      BELL_SLEEP_MS(wait.count());

      std::lock_guard<std::mutex> lock(pendingMutex);
      if (hasPending) {
        isPlaying = pendingIsPlaying;
        trackUri = pendingTrackUri;
        positionMs = pendingPositionMs;
        durationMs = pendingDurationMs;
        reason = pendingReason;
        hasPending = false;
      }
    }

    connectstate_PutStateRequest request = connectstate_PutStateRequest_init_zero;
    request.put_state_reason = reason;
    // Always true here, unlike putState()'s default: this only runs off a
    // genuine playback event (track loaded or play/pause).
    request.is_active = true;
    if (!isActiveDevice.exchange(true)) {
      activeSinceMs = ctx->timeProvider->getSyncedTimestamp();
    }

    request.device.has_player_state = true;
    {
      std::lock_guard<std::mutex> lock(engineMutex);
      // is_playing means "session active", NOT !is_paused - getting this
      // wrong grayed out resume on the real client. See §19.
      playerState.is_playing = !trackUri.empty();
      playerState.is_paused = !isPlaying;
      // Not tied to is_paused: buffering means audio isn't loaded yet,
      // which is already false by the time this fires (TrackPlayer is
      // producing frames), independent of pause state. See §30.
      playerState.is_buffering = false;
      playerState.playback_speed = isPlaying ? 1.0 : 0.0;
      playerState.timestamp = ctx->timeProvider->getSyncedTimestamp();
      playerState.position_as_of_timestamp = (int64_t)positionMs;
      playerState.duration = (int64_t)durationMs;
      // Unconditional either way - see putBufferingState()'s comment.
      playerState.has_track = !trackUri.empty();
      if (playerState.has_track) {
        pbPutString(trackUri, playerState.track.uri);
      }

      // session_id/playback_id/context_uri - see the member comment
      // (ConnectStateHandler.h).
      request.device.player_state = playerState;
      pbPutString(sessionId, request.device.player_state.session_id);
      if (!playbackId.empty()) {
        pbPutString(playbackId, request.device.player_state.playback_id);
      }
      if (!contextUri.empty()) {
        pbPutString(contextUri, request.device.player_state.context_uri);
      }
    }

    sendPutStateRequest(request);
    lastPutTime = std::chrono::steady_clock::now();
  }
}

void ConnectStateHandler::handleClusterUpdate(
    const std::vector<uint8_t>& payload) {
  // pbDecode() needs a mutable buffer - cluster updates are infrequent, a
  // copy is cheap enough.
  auto payloadCopy = payload;

  connectstate_ClusterUpdate update = connectstate_ClusterUpdate_init_zero;
  pbDecode(update, connectstate_ClusterUpdate_fields, payloadCopy);

  CSPOT_LOG(info,
           "Dealer: cluster update, reason=%d active_device_id=%s "
           "(%d bytes decoded)",
           (int)update.update_reason,
           (update.has_cluster && update.cluster.active_device_id != nullptr)
               ? update.cluster.active_device_id
               : "?",
           (int)payload.size());

  // Someone else just became the active device while we thought we were -
  // back off.
  if (isActiveDevice && update.has_cluster &&
      update.cluster.active_device_id != nullptr &&
      ctx->config.deviceId != update.cluster.active_device_id) {
    CSPOT_LOG(info, "another device (%s) took over, stopping playback",
             update.cluster.active_device_id);
    isActiveDevice = false;
    activeSinceMs = 0;
    trackPlayer->stop();
    // go-librespot's stopPlayback() resets its own PlayerState right here
    // too (State.reset(), the only other call site besides initState()).
    resetPlayerState();
    // Tell the server too (/inactive) or the cluster keeps stale state for
    // this device.
    putStateInactive();
    sendEngineEvent(EventType::DISC);
  }

  pb_release(connectstate_ClusterUpdate_fields, &update);
}

void ConnectStateHandler::handleSetVolume(const std::vector<uint8_t>& payload) {
  auto payloadCopy = payload;

  connectstate_SetVolumeCommand cmd = connectstate_SetVolumeCommand_init_zero;
  pbDecode(cmd, connectstate_SetVolumeCommand_fields, payloadCopy);

  int volume = cmd.volume;
  ctx->config.volume = volume;
  CSPOT_LOG(info, "Dealer: set volume to %d", volume);
  sendEngineEvent(EventType::VOLUME, volume);

  // §55: buildDeviceInfo() reads volume fresh on every PUT, but nothing
  // here triggered one - re-send the cached PlayerState with
  // VOLUME_CHANGED so the app sees the new value promptly, matching
  // go-librespot. Guarded on isActiveDevice: updatePlayerState() always
  // forces is_active=true, which would wrongly announce this device as
  // active from a volume tweak before it's ever played anything.
  if (isActiveDevice) {
    std::string trackUri;
    uint32_t durationMs;
    bool playing;
    {
      std::lock_guard<std::mutex> lock(engineMutex);
      trackUri = std::string(playerState.track.uri);
      durationMs = (uint32_t)playerState.duration;
      playing = isPlayingState;
    }
    updatePlayerState(playing, trackUri, getPositionMs(), durationMs,
                      connectstate_PutStateReason_VOLUME_CHANGED);
  }
}

void ConnectStateHandler::setLastCommand(uint32_t messageId,
                                         const std::string& sentByDeviceId) {
  std::lock_guard<std::mutex> lock(lastCommandMutex);
  lastCommandMessageId = messageId;
  lastCommandSentByDeviceId = sentByDeviceId;
}

// handlePlayerCommand() and its per-endpoint implementations (transfer/play/
// pause/resume/skip/seek/repeat/update_context/add_to_queue/set_queue), plus
// loadTracks(), now live in ConnectStateHandlerCommands.cpp - see that
// file's header comment.

void ConnectStateHandler::setEventHandler(EventHandler handler) {
  engineEventHandler = handler;
}

void ConnectStateHandler::sendEngineEvent(EventType type) {
  if (!engineEventHandler) return;
  auto event = std::make_unique<Event>();
  event->eventType = type;
  event->data = {};
  engineEventHandler(std::move(event));
}

void ConnectStateHandler::sendEngineEvent(EventType type, EventData data) {
  if (!engineEventHandler) return;
  auto event = std::make_unique<Event>();
  event->eventType = type;
  event->data = data;
  engineEventHandler(std::move(event));
}

void ConnectStateHandler::setPlaybackPlaying(bool playing) {
  std::lock_guard<std::mutex> lock(engineMutex);
  if (isPlayingState && !playing) {
    positionMs += (uint32_t)(ctx->timeProvider->getSyncedTimestamp() -
                             positionMeasuredAt);
  }
  positionMeasuredAt = ctx->timeProvider->getSyncedTimestamp();
  isPlayingState = playing;
}

uint32_t ConnectStateHandler::getPositionMs() {
  // Real decoder position (Vorbis only) beats the wall-clock estimate
  // below - freezes on its own while paused, see TrackPlayer.h's comment.
  uint32_t decoderPosition;
  if (trackPlayer->getDecoderPositionMs(decoderPosition)) {
    return decoderPosition;
  }

  std::lock_guard<std::mutex> lock(engineMutex);
  uint32_t position = positionMs;
  if (isPlayingState) {
    position += (uint32_t)(ctx->timeProvider->getSyncedTimestamp() -
                           positionMeasuredAt);
  }
  return position;
}

void ConnectStateHandler::notifyAudioReachedPlayback(
    const std::string& trackId) {
  // TEMP DIAGNOSTIC (track-flicker investigation, 2026-07-18): confirm
  // whether this fires twice in quick succession with different trackIds -
  // remove once resolved.
  CSPOT_LOG(info, "notifyAudioReachedPlayback: called trackId=%s notifyPending=%d",
           trackId.c_str(), (int)trackQueue->notifyPending);

  int offset = 0;

  // consumeTrack() returns nullptr when the queue is exhausted - see F24,
  // same null-deref hazard SpircHandler's own copy of this logic guards
  // against.
  auto currentTrack = trackQueue->consumeTrack(nullptr, offset);
  if (currentTrack == nullptr) {
    CSPOT_LOG(info, "notifyAudioReachedPlayback: queue empty, nothing to report");
    return;
  }

  if (trackQueue->notifyPending) {
    trackQueue->notifyPending = false;
    std::lock_guard<std::mutex> lock(engineMutex);
    positionMs = currentTrack->requestedPosition;
    positionMeasuredAt = ctx->timeProvider->getSyncedTimestamp();
    currentTrack->requestedPosition = 0;
  } else {
    // preloadedTracks' own head can be more than one skip behind trackId:
    // TrackPlayer.cpp's own failure path (F89) already calls skipTrack()
    // when a track fails to load, but that pops whatever is CURRENTLY at
    // the head - which, if this class hasn't caught up from the PREVIOUS
    // transition yet, is the already-finished track, not the failed one.
    // Left uncorrected, the failed track stays stuck at the head and this
    // function can end up never finding trackId at all - silently never
    // sending TRACK_INFO for whatever's actually playing. Keep skipping
    // forward (never backward, never guessing) until the head really is
    // trackId. One skip covers the normal case; bounded generously beyond
    // that only as a guard against an actual bug elsewhere looping forever.
    constexpr int MAX_CATCHUP_SKIPS = 16;
    int skips = 0;
    while (currentTrack->identifier != trackId) {
      CSPOT_LOG(info,
               "notifyAudioReachedPlayback: catch-up skip #%d, head='%s' "
               "(uri=%s) != want='%s'",
               skips + 1, currentTrack->identifier.c_str(),
               currentTrack->ref.uri.c_str(), trackId.c_str());
      if (++skips > MAX_CATCHUP_SKIPS ||
          !trackQueue->skipTrack(TrackQueue::SkipDirection::NEXT, 0, false)) {
        CSPOT_LOG(error,
                 "notifyAudioReachedPlayback: couldn't catch up to '%s', "
                 "giving up",
                 trackId.c_str());
        return;
      }
      currentTrack = trackQueue->consumeTrack(nullptr, offset);
      if (currentTrack == nullptr) {
        CSPOT_LOG(info, "notifyAudioReachedPlayback: queue exhausted mid-catch-up");
        return;
      }
    }
    std::lock_guard<std::mutex> lock(engineMutex);
    positionMs = 0;
    positionMeasuredAt = ctx->timeProvider->getSyncedTimestamp();
  }

  // Fresh playback id per track start, hex-encoded (not base64, unlike
  // session_id - a real divergence found against go-librespot). See §39.
  {
    Crypto crypto;
    auto newPlaybackId = bytesToHexString(crypto.generateVectorWithRandomData(16));
    std::lock_guard<std::mutex> lock(engineMutex);
    playbackId = newPlaybackId;
  }

  CSPOT_LOG(info,
           "notifyAudioReachedPlayback: sending TRACK_INFO identifier=%s "
           "uri=%s name=%s",
           currentTrack->identifier.c_str(), currentTrack->ref.uri.c_str(),
           currentTrack->trackInfo.name.c_str());

  sendEngineEvent(EventType::TRACK_INFO, currentTrack->trackInfo);
}

void ConnectStateHandler::notifyAudioEnded() {
  {
    std::lock_guard<std::mutex> lock(engineMutex);
    isPlayingState = false;
    positionMs = 0;
    positionMeasuredAt = ctx->timeProvider->getSyncedTimestamp();
  }
  trackPlayer->resetState(true);

  // Also covers "gave up after every track failed to load" (same
  // eofCallback as a natural EOF, §30) - tell the app instead of leaving
  // it stuck on whatever putBufferingState() last announced.
  updatePlayerState(false, "", 0, 0);
}

void ConnectStateHandler::disconnect() {
  trackQueue->stopTask();
  trackPlayer->stop();
}