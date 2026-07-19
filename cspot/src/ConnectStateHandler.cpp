#include "ConnectStateHandler.h"

#include <algorithm>  // for min
#include <chrono>     // for milliseconds, steady_clock
#include <cstring>    // for strcpy
#include <optional>   // for nullopt
#include <utility>    // for move
#include <vector>     // for vector

#include "BellLogger.h"  // for AbstractLogger
#include "BellUtils.h"   // for BELL_SLEEP_MS
#include "CSpotContext.h"  // for Context
#include "Crypto.h"         // for Crypto::base64Encode
#include "Login5Client.h"
#include "Logger.h"        // for CSPOT_LOG
#include "NanoPBHelper.h"  // for pbDecode
#include "TimeProvider.h"
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
}  // namespace

ConnectStateHandler::ConnectStateHandler(
    std::shared_ptr<cspot::Context> ctx,
    std::shared_ptr<cspot::Login5Client> login5)
    : bell::Task("cspotConnectState", 32 * 1024, 1, 0), ctx(ctx),
      login5(login5), contextResolver(ctx, login5),
      playbackController(
          ctx,
          [this](std::shared_ptr<QueuedTrack> track, bool paused) {
            // Announce the new track immediately (is_buffering=true),
            // before the CDN fetch/decode that reachedPlaybackCallback
            // waits on - matches go-librespot's early loadCurrentTrack()
            // PUT. Without this, clients only learn the new track's
            // identity once decoding actually starts, and any PUT sent in
            // between (e.g. the PLAY_PAUSE event below, handled by the
            // app's own currentTrackUri cache) would still carry the
            // previous track's URI.
            updatePlayerState(!paused, track->ref.uri,
                              track->requestedPosition,
                              (uint32_t)track->trackInfo.duration,
                              connectstate_PutStateReason_PLAYER_STATE_CHANGED,
                              /*isBuffering=*/true);
            sendEngineEvent(EventType::PLAYBACK_START,
                           (int)track->requestedPosition);
            sendEngineEvent(EventType::PLAY_PAUSE, paused);
          },
          [this](std::shared_ptr<QueuedTrack> track) {
            // Fresh playback id per track start, hex-encoded (not base64,
            // unlike session_id - a real divergence found against
            // go-librespot). See §39.
            Crypto crypto;
            stateModel.setPlaybackId(
                bytesToHexString(crypto.generateVectorWithRandomData(16)));
            sendEngineEvent(EventType::TRACK_INFO, track->trackInfo);
          },
          [this] { sendEngineEvent(EventType::DEPLETED); }),
      putStateClient(PutStateClient::defaultHostResolver,
                    [this](const std::string& host) {
                      contextResolver.seedSpclientHost(host);
                    }) {
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

  // Set from PlaybackController's trackLoadedCallback whenever a track
  // loads - mirrors go-librespot's Player.HasBeenPlayingFor().
  int64_t trackStartedAt = playbackController.getCurrentTrackStartedAtMs();
  if (trackStartedAt != 0) {
    request.has_been_playing_for_ms =
        (uint64_t)(ctx->timeProvider->getSyncedTimestamp() - trackStartedAt);
  }

  // PlayerState + session_id/playback_id/context_uri/restrictions/
  // context_metadata - see ConnectStateModel.h.
  stateModel.fillIntoRequest(request);

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
  auto trackQueue = playbackController.getTrackQueue();
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

  return putStateClient.put(request, ctx->config.deviceId, accessToken,
                            clientToken, connId);
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

  return putStateClient.putInactive(ctx->config.deviceId, accessToken,
                                    clientToken, connId);
}

bool ConnectStateHandler::putState(connectstate_PutStateReason reason,
                                   bool isActive) {
  connectstate_PutStateRequest request = connectstate_PutStateRequest_init_zero;
  request.put_state_reason = reason;
  request.is_active = isActive;

  // Minimal PlayerState (matches go-librespot's State.reset): enough for a
  // device that isn't playing anything yet.
  request.device.has_player_state = true;

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
  // playback_id deliberately not touched here - not known until the
  // stream actually opens.
  stateModel.setPlaybackState(true, paused, /*isBuffering=*/true,
                              ctx->timeProvider->getSyncedTimestamp(),
                              positionMs, /*durationMs=*/std::nullopt,
                              trackUri);

  return sendPutStateRequest(request);
}

void ConnectStateHandler::updatePlayerState(bool isPlaying,
                                            const std::string& trackUri,
                                            uint32_t positionMs,
                                            uint32_t durationMs,
                                            connectstate_PutStateReason reason,
                                            bool isBuffering) {
  // Non-blocking: the actual PUT runs on this class's own task (runTask()),
  // never the caller's - doing it inline overflowed small caller stacks.
  std::lock_guard<std::mutex> lock(pendingMutex);
  hasPending = true;
  pendingIsPlaying = isPlaying;
  pendingTrackUri = trackUri;
  pendingPositionMs = positionMs;
  pendingDurationMs = durationMs;
  pendingReason = reason;
  pendingIsBuffering = isBuffering;
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
    bool isBuffering;
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
      isBuffering = pendingIsBuffering;
      hasPending = false;
    }

    // §6.6: at most one PUT every PUT_MIN_INTERVAL_MS, coalescing a burst
    // of events into the latest pending state - also honors
    // putStateClient's rate-limit tracking (set from a 429). Re-check for
    // an even fresher pending update that arrived during the wait.
    auto now = std::chrono::steady_clock::now();
    auto minIntervalWait = std::chrono::milliseconds(PUT_MIN_INTERVAL_MS) -
                           std::chrono::duration_cast<std::chrono::milliseconds>(
                               now - lastPutTime);
    auto rateLimitWait = std::chrono::duration_cast<std::chrono::milliseconds>(
        putStateClient.rateLimitedUntil() - now);
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
        isBuffering = pendingIsBuffering;
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
    // is_playing means "session active", NOT !is_paused - getting this
    // wrong grayed out resume on the real client. isBuffering: true only
    // for the early "new track, not yet decoding" announcement
    // (trackLoadedCallback) - false everywhere else, since by the time
    // any other caller fires, TrackPlayer is already producing frames.
    stateModel.setPlaybackState(/*isPlaying=*/!trackUri.empty(),
                                /*isPaused=*/!isPlaying, isBuffering,
                                ctx->timeProvider->getSyncedTimestamp(),
                                positionMs, durationMs, trackUri);

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
    playbackController.stop();
    // go-librespot's stopPlayback() resets its own PlayerState right here
    // too (State.reset(), the only other call site besides initState()).
    stateModel.reset();
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
    std::string trackUri = stateModel.trackUri();
    uint32_t durationMs = stateModel.duration();
    bool playing = playbackController.isPlaying();
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

uint32_t ConnectStateHandler::getPositionMs() {
  return playbackController.getPositionMs();
}

void ConnectStateHandler::notifyAudioEnded() {
  playbackController.reportEnded();

  // Also covers "gave up after every track failed to load" (same
  // eofCallback as a natural EOF, §30) - tell the app instead of leaving
  // it stuck on whatever putBufferingState() last announced.
  updatePlayerState(false, "", 0, 0);
}

void ConnectStateHandler::disconnect() {
  playbackController.disconnect();
}