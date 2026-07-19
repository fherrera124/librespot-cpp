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
}  // namespace

ConnectStateHandler::ConnectStateHandler(
    std::shared_ptr<cspot::Context> ctx,
    std::shared_ptr<cspot::Login5Client> login5)
    : bell::Task("cspotConnectState", 32 * 1024, 1, 0), ctx(ctx),
      login5(login5), contextResolver(ctx, login5),
      putStateClient(PutStateClient::defaultHostResolver,
                    [this](const std::string& host) {
                      contextResolver.seedSpclientHost(host);
                    }) {
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
    // Announce the new track immediately (is_buffering=true), before the
    // CDN fetch/decode that notifyAudioReachedPlayback() waits on -
    // matches go-librespot's early loadCurrentTrack() PUT. Without this,
    // clients only learn the new track's identity once decoding actually
    // starts, and any PUT sent in between (e.g. the PLAY_PAUSE event
    // below, handled by the app's own currentTrackUri cache) would still
    // carry the previous track's URI.
    updatePlayerState(!paused, track->ref.uri, track->requestedPosition,
                      (uint32_t)track->trackInfo.duration,
                      connectstate_PutStateReason_PLAYER_STATE_CHANGED,
                      /*isBuffering=*/true);
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
    trackPlayer->stop();
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
    bool playing;
    {
      std::lock_guard<std::mutex> lock(engineMutex);
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
    stateModel.setPlaybackId(newPlaybackId);
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