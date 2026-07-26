#include "ConnectStateHandler.h"

#include <algorithm>
#include <iostream>
#include <random>

#include <tao/json.hpp>
#include "SessionContext.h"
#include "Utils.h"
#include "api/SpClient.h"
#include "bell/Logger.h"
#include "bell/Result.h"
#include "bell/utils/Utils.h"
#include "connect.pb.h"
#include "events/EventLoop.h"
#include "events/EventModels.h"
#include "mbedtls/base64.h"
#include "metadata.pb.h"
#include "pb.h"
#include "proto/SpotifyId.h"
#include "tao/json/to_string.hpp"
#include "tl/expected.hpp"
#include "tracks/TrackQueueHandler.h"

using namespace cspot;

namespace {
std::string spircVersion = "3.2.6";
std::string deviceSoftwareVersion = "1.0.0";
std::string clientId = "65b708073fc0480ea92a077233ca87bd";  // Spotify client ID
std::string connectCapabilities;
std::vector<std::string> supportedTypes = {"audio/track", "audio/episode"};
std::string sessionIdChars =
    "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

// Generates a random session ID of 16 characters
std::string generateSessionId() {
  static std::independent_bits_engine<std::default_random_engine, CHAR_BIT,
                                      unsigned char>
      randomEngine{std::default_random_engine(std::random_device{}())};
  std::string sessionId;
  sessionId.reserve(16);  // Reserve space for 16 characters

  std::generate_n(std::back_inserter(sessionId), 16, []() {
    return sessionIdChars[randomEngine() % sessionIdChars.size()];
  });
  return sessionId;
}
};  // namespace

ConnectStateHandler::ConnectStateHandler(
    std::shared_ptr<cspot::EventLoop> eventLoop,
    std::shared_ptr<AuthInfo> authInfo, std::shared_ptr<SpClient> spClient,
    VolumeChangedCallback volumeChangedCallback)
    // Only ever sleeps and occasionally performs the same HTTPS PUT as
    // every other handler here - sized like every other network-doing
    // task in this codebase (cspot_event_loop/cspot_player/
    // cspot_file_provider), not trimmed down, given this project's own
    // history of stack-overflow crashes from undersized task stacks.
    : bell::Task("cspot_connect_state", 32 * 1024),
      eventLoop(std::move(eventLoop)),
      authInfo(std::move(authInfo)),
      spClient(std::move(spClient)),
      volumeChangedCallback(std::move(volumeChangedCallback)) {
  trackQueueHandler =
      createDefaultTrackQueueHandler(this->spClient, this->eventLoop);

  this->eventLoop->registerHandler(
      EventLoop::EventType::PLAYER_STATE_UPDATED,
      [this](cspot::EventLoop::Event&& event) {
        auto& playerStateUpdate =
            std::get<cspot::PlayerStateUpdate>(event.payload);

        auto& playerState = putStateRequestProto.device.playerState;
        playerState.duration = playerStateUpdate.playbackDurationMs;
        playerState.position = playerStateUpdate.positionAsOfTimestamp;
        playerState.isPlaying = playerStateUpdate.isPlaying;
        playerState.isPaused = playerStateUpdate.isPaused;
        playerState.isBuffering = playerStateUpdate.isBuffering;
        playerState.timestamp = playerStateUpdate.timestamp;
        // Deliberately not touched when empty (the isBuffering=true
        // announce, before a playback id is known) - leaves whatever the
        // previous track's id was in place transiently rather than
        // clearing it, matching this repo's own PlayerEngine.cpp
        // precedent for the same field.
        if (!playerStateUpdate.playbackId.empty()) {
          playerState.playbackId = playerStateUpdate.playbackId;
        }

        (void)putState();
      });

  // StreamPlayer ran out of audio (natural end of track) - advance the same
  // way a remote skip_next would, so TrackQueueHandler's position and the
  // Spotify-visible track/index stay correct even when nothing remote ever
  // asked us to move on.
  this->eventLoop->registerHandler(
      EventLoop::EventType::TRACK_ENDED, [this](cspot::EventLoop::Event&&) {
        auto res = advanceToNextTrack();
        if (!res) {
          BELL_LOG(error, LOG_TAG, "Failed to advance after track end: {}",
                   res.error());
        }
      });

  // this->sessionContext->eventLoop->registerHandler(
  //     EventLoop::EventType::TRACKPROVIDER_UPDATED,
  //     [this](cspot::EventLoop::Event&& event) {
  //       auto& playerState = putStateRequestProto.device.playerState;
  //       // playerState.track = trackQueue->getCurrentTrack();
  //       playerState.nextTracks = trackQueue->getNextTracks();
  //       playerState.prevTracks = trackQueue->getPreviousTracks();

  //       auto currentContextIndex = trackQueue->getCurrentContextIndex();
  //       if (currentContextIndex.has_value()) {
  //         playerState.index.value = currentContextIndex.value();
  //         playerState.index.hasValue = true;
  //       } else {
  //         playerState.index.hasValue = false;
  //       }

  //       BELL_LOG(info, LOG_TAG, "Updated player state with current track: {}",
  //                playerState.track.uri);
  //       // Update state
  //       this->putState();
  //     });

  initialize();
  startTask();
}

ConnectStateHandler::~ConnectStateHandler() {
  stopTask();
}

void ConnectStateHandler::initialize() {
  auto& deviceProto = putStateRequestProto.device;

  auto& deviceInfo = deviceProto.deviceInfo;
  deviceInfo.canPlay = true;
  deviceInfo.volume = 100;
  deviceInfo.name = authInfo->deviceName;

  deviceInfo.deviceType = DeviceType_SPEAKER;
  deviceInfo.deviceSoftwareVersion = deviceSoftwareVersion;
  deviceInfo.deviceId = authInfo->deviceId;
  deviceInfo.clientId = clientId;
  deviceInfo.spircVersion = spircVersion;

  auto& capabilities = deviceInfo.capabilities.rawProto;

  // Init capatilities
  capabilities.can_be_player = true;
  capabilities.restrict_to_local = false;
  capabilities.gaia_eq_connect_id = true;
  capabilities.supports_logout = true; // TODO: only if zeroconfEnabled
  capabilities.is_observable = true;
  capabilities.volume_steps = 100;
  capabilities.command_acks = true;
  capabilities.supports_rename = false;
  capabilities.hidden = false;
  capabilities.disable_volume = false;
  capabilities.connect_disabled = false;
  capabilities.supports_playlist_v2 = true;
  capabilities.is_controllable = true;
  capabilities.supports_external_episodes = false;
  capabilities.supports_set_backend_metadata = true;
  capabilities.supports_transfer_command = true;
  capabilities.supports_command_request = true;
  capabilities.is_voice_enabled = false;
  capabilities.needs_full_player_state = false;
  capabilities.supports_set_options_command = true;
  capabilities.supports_gzip_pushes = false;  // TODO: Should we support this?
  capabilities.has_supports_hifi = false;

  deviceInfo.capabilities.supportedTypes = supportedTypes;

  auto& playerState = deviceProto.playerState;
  playerState.isSystemInitiated = true;

  // Assign next and previous tracks encode callbacks
  playerState.nextTracks.funcs.encode = pbEncodeNextTracks;
  playerState.prevTracks.funcs.encode = pbEncodePreviousTracks;
  playerState.nextTracks.arg = this;
  playerState.prevTracks.arg = this;
}

bell::Result<> ConnectStateHandler::handlePlayerCommand(
    tao::json::value& messageJson) {
  auto& payload = messageJson.at("payload");
  auto& command = payload.at("command");
  std::string endpoint = command.at("endpoint").get_string();

  // Assign the last message ID and device ID
  putStateRequestProto.lastCommandMessageId =
      payload.at("message_id").get_unsigned();
  putStateRequestProto.lastCommandSentByDeviceId =
      payload.at("sent_by_device_id").get_string();

  if (endpoint == "transfer") {
    BELL_LOG(info, LOG_TAG, "Received transfer command");
    std::string_view payloadDataStr = command.as<std::string_view>("data");
    return handleTransferCommand(payloadDataStr, command["options"]);
  } else if (endpoint == "skip_next") {
    BELL_LOG(info, LOG_TAG, "Received skip_next command");
    return handleSkipNextCommand();
  } else if (endpoint == "skip_prev") {
    BELL_LOG(info, LOG_TAG, "Received skip_prev command");
    return handleSkipPrevCommand();
  } else if (endpoint == "pause") {
    BELL_LOG(info, LOG_TAG, "Received pause command");
    return handlePauseCommand(true);
  } else if (endpoint == "resume") {
    BELL_LOG(info, LOG_TAG, "Received resume command");
    return handlePauseCommand(false);
  } else if (endpoint == "play") {
    BELL_LOG(info, LOG_TAG, "Received play command");
    return handlePlayCommand(command);
  } else {
    BELL_LOG(info, LOG_TAG, "Received unknown command: {}", endpoint);
    return bell::make_unexpected_errc(std::errc::operation_not_supported);
  }

  return {};
}

bell::Result<> ConnectStateHandler::putState(PutStateReason reason) {
  std::scoped_lock lock(putStateMutex);

  constexpr auto kStatePutMinInterval = std::chrono::milliseconds(200);

  auto now = std::chrono::steady_clock::now();
  if (now - lastPutStateTime >= kStatePutMinInterval) {
    return flushStateNowLocked(reason);
  }

  // Inside the rate-limit window - coalesce into a single deferred PUT
  // (taskLoop() below), "last reason wins". Don't push the due time out
  // further on repeated calls within the same window, or a steady stream
  // of state changes could defer it forever.
  pendingPutStateReason = reason;
  if (!putStatePending) {
    putStatePending = true;
    putStateDueTime = lastPutStateTime + kStatePutMinInterval;
  }

  return {};
}

bell::Result<> ConnectStateHandler::flushStateNowLocked(
    PutStateReason reason) {
  // get milliseconds since epoch;
  putStateRequestProto.clientSideTimestamp =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();
  putStateRequestProto.memberType = MemberType_CONNECT_STATE;
  putStateRequestProto.putStateReason = reason;

  lastPutStateTime = std::chrono::steady_clock::now();
  putStatePending = false;

  // Temp diagnostic: dump exactly what this PUT's PlayerState carries, to
  // rule out a field silently not making it onto the wire the way the C++
  // side thinks it did (this repo's own master branch hit and logged
  // exactly this class of doubt during a real investigation).
  {
    auto& ps = putStateRequestProto.device.playerState;
    // deviceId/connectionId included because this PUT's URL path is
    // /connect-state/v1/devices/{deviceId} and its X-Spotify-Connection-Id
    // header is connectionId (authInfo->sessionId) - neither is part of
    // the PutStateRequest body itself, so nothing above this line would
    // ever reveal if one of them were stale/empty. A device whose PUT
    // succeeds (200) but whose connection id doesn't correlate to a real,
    // currently-open dealer session from the server's own point of view
    // would explain isActive=true on our side while the server's own
    // cluster keeps reporting a different device as active.
    BELL_LOG(info, LOG_TAG,
             "PUT DIAG: deviceId={} connectionId={} isActive={} "
             "playerSessionId={} track.uri={} index=[{},{}] isPlaying={} "
             "isPaused={} isBuffering={} playbackId={}",
             authInfo->deviceId, authInfo->sessionId,
             putStateRequestProto.isActive, ps.sessionId, ps.track.uri,
             ps.index.value.page, ps.index.value.track, ps.isPlaying,
             ps.isPaused, ps.isBuffering, ps.playbackId);
  }

  auto putStartTime = std::chrono::steady_clock::now();
  auto res = this->spClient->putConnectState(putStateRequestProto,
                                             authInfo->deviceId,
                                             authInfo->sessionId);

  // Only errors were ever logged here before - on real hardware this PUT
  // is the thing the initiating client's "Connecting..." wait actually
  // depends on, and its round-trip time was otherwise invisible.
  if (res) {
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - putStartTime)
                        .count();
    BELL_LOG(info, LOG_TAG,
             "Put state succeeded in {}ms (reason={}, isActive={}, "
             "isBuffering={}, isPaused={})",
             elapsedMs, static_cast<int>(reason), putStateRequestProto.isActive,
             putStateRequestProto.device.playerState.isBuffering,
             putStateRequestProto.device.playerState.isPaused);
  }

  return res;
}

void ConnectStateHandler::taskLoop() {
  bell::utils::sleepMs(50);

  if (!putStatePending) {
    return;
  }

  std::scoped_lock lock(putStateMutex);
  if (!putStatePending || std::chrono::steady_clock::now() < putStateDueTime) {
    return;
  }

  auto res = flushStateNowLocked(pendingPutStateReason);
  if (!res) {
    BELL_LOG(error, LOG_TAG, "Deferred put state failed: {}", res.error());
  }
}

bell::Result<> ConnectStateHandler::handleClusterUpdate(
    std::string_view payloadDataStr) {
  auto decodedData = base64Decode(payloadDataStr);
  cspot_proto::ClusterUpdate clusterUpdate;

  bool res = nanopb_helper::decodeFromVector(clusterUpdate, decodedData);
  if (!res) {
    BELL_LOG(error, LOG_TAG, "Failed to decode cluster update");
    return bell::make_unexpected_errc(std::errc::bad_message);
  }

  // Temp diagnostic: this used to only log when we decided to deactivate -
  // every other cluster update (including ones confirming our own
  // activation) went by silently. Logging all of them lets us see whether
  // the backend ever contradicts what we just PUT (e.g. activeDeviceId
  // isn't us, or isn't anyone, right after we announced isActive=true).
  BELL_LOG(info, LOG_TAG,
           "Cluster update: activeDeviceId={} (ours={}) ts={} ourIsActive={} "
           "lastTransferTimestamp={}",
           clusterUpdate.cluster.activeDeviceId, authInfo->deviceId,
           clusterUpdate.cluster.playerState.timestamp,
           putStateRequestProto.isActive, lastTransferTimestamp);

  // Someone else just became the active device while we thought we were -
  // back off. The lastTransferTimestamp guard (matches go-librespot's
  // daemon/player.go) stops a stale/reordered ClusterUpdate from
  // deactivating us right after a legitimate transfer to this device.
  bool stopBeingActive =
      putStateRequestProto.isActive &&
      clusterUpdate.cluster.activeDeviceId != authInfo->deviceId &&
      clusterUpdate.cluster.playerState.timestamp > lastTransferTimestamp;

  if (!stopBeingActive) {
    return {};
  }

  BELL_LOG(info, LOG_TAG, "Playback was transferred to device {}",
           clusterUpdate.cluster.activeDeviceId);

  putStateRequestProto.isActive = false;
  eventLoop->post(EventLoop::EventType::PLAYER_PLAY, false);

  auto inactiveRes = spClient->putInactive(authInfo->deviceId,
                                           authInfo->sessionId);
  if (!inactiveRes) {
    BELL_LOG(error, LOG_TAG, "Failed to put inactive state: {}",
             inactiveRes.error());
    return inactiveRes;
  }

  return {};
}

bell::Result<> ConnectStateHandler::handleSetVolume(
    std::string_view payloadDataStr) {
  auto decodedData = base64Decode(payloadDataStr);
  cspot_proto::SetVolumeCommand setVolumeCommand;

  bool res = nanopb_helper::decodeFromVector(setVolumeCommand, decodedData);
  if (!res) {
    BELL_LOG(error, LOG_TAG, "Failed to decode set volume command");
    return bell::make_unexpected_errc(std::errc::bad_message);
  }

  // Connect-state volume is 0..65535 (player.MaxStateVolume in
  // go-librespot) - matches DeviceInfo.volume's own range, no rescaling
  // needed.
  uint16_t volume = static_cast<uint16_t>(
      std::clamp<int32_t>(setVolumeCommand.volume, 0, 65535));

  BELL_LOG(info, LOG_TAG, "Set volume to {}", volume);
  putStateRequestProto.device.deviceInfo.volume = volume;

  volumeChangedCallback(volume);

  auto putRes = putState(PutStateReason_VOLUME_CHANGED);
  if (!putRes) {
    BELL_LOG(error, LOG_TAG, "Failed to put state after volume change: {}",
             putRes.error());
    return putRes;
  }

  return {};
}

bell::Result<> ConnectStateHandler::handleTransferCommand(
    std::string_view payloadDataStr, const tao::json::value& options) {
  auto decodedData = base64Decode(payloadDataStr);
  cspot_proto::TransferState transferState;

  bool res = nanopb_helper::decodeFromVector(transferState, decodedData);
  if (!res) {
    BELL_LOG(error, LOG_TAG, "Failed to decode transfer state");
    return bell::make_unexpected_errc(std::errc::bad_message);
  }

  BELL_LOG(info, LOG_TAG, "Transfer state decoded successfully");

  // Set active state
  putStateRequestProto.isActive = true;

  auto& playerState = putStateRequestProto.device.playerState;

  if (transferState.current_session.originalSessionId.hasValue) {
    playerState.sessionId =
        transferState.current_session.originalSessionId.value;
  } else {
    // Generate random 16-byte session ID
    playerState.sessionId = generateSessionId();
  }
  // Temp diagnostic: confirm whether the source actually sent a session id
  // to continue, or whether we're minting a fresh one it has no way to
  // recognize - a client that correlates connect-state updates by session
  // id would never leave "Connecting" if every PUT looks like a brand new,
  // unrelated session to it.
  BELL_LOG(info, LOG_TAG,
           "TransferState session id: hadOriginal={}, using sessionId={}",
           transferState.current_session.originalSessionId.hasValue,
           playerState.sessionId);

  // isPlaying means "session active", NOT "audio already flowing" - stays
  // true through buffering, same as advanceToNextTrack()/
  // handleSkipPrevCommand() below and master's PlayerEngine::
  // putBufferingState() (hardcodes isPlaying=true) and go-librespot's
  // loadCurrentTrack() (IsPlaying=true set alongside IsBuffering=true).
  // Sending isPlaying=false here (as this used to) grayed out play/pause
  // and hid the progress bar on the real desktop client for the whole
  // buffering window of every transfer - confirmed against a real session.
  playerState.isPlaying = true;
  playerState.isBuffering = true;
  playerState.timestamp = transferState.playback.timestamp;
  lastTransferTimestamp = transferState.playback.timestamp;

  bool shouldPause =
      transferState.playback.isPaused &&
      options.optional<std::string>("restore_paused") == "restore";

  BELL_LOG(info, LOG_TAG,
           "Transfer playback state: sourceIsPaused={}, restore_paused={}, "
           "shouldPause={} (posting PLAYER_PLAY={})",
           transferState.playback.isPaused,
           options.optional<std::string>("restore_paused").value_or("<none>"),
           shouldPause, !shouldPause);

  playerState.isPaused = shouldPause;
  playerState.contextUri = transferState.current_session.context.uri;
  playerState.contextUrl = transferState.current_session.context.url;
  playerState.options = transferState.options;
  // go-librespot copies this from the incoming TransferState on every
  // transfer (daemon/player.go) and otherwise keeps a non-nil (if empty)
  // Suppressions on every PlayerState it PUTs - never omitting the field
  // entirely like this code used to.
  playerState.suppressions = transferState.current_session.suppressions;
  playerState.track.uid = transferState.current_session.currentUid;
  playerState.position = 0;
  playerState.positionAsOfTimestamp =
      transferState.playback.positionAsOfTimestamp;
  putStateRequestProto.startedPlayingAt =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();
  putStateRequestProto.hasBeenPlayingForMs = 0;

  SpotifyIdType trackType =
      SpotifyId::getTypeFromContext(transferState.current_session.context.uri);
  SpotifyId trackId =
      SpotifyId(trackType, transferState.playback.currentTrack.gid);

  // Still no PUT before loadContext() resolves - context.uri/currentUid
  // are needed to even know what "the current track" is, and this repo's
  // own master branch (PlayerEngine::handleTransfer -> contextResolver.
  // resolve()) and go-librespot (controls.go's loadContext(), called
  // before loadCurrentTrack()'s first PUT) both resolve first too.
  auto loadRes = trackQueueHandler->loadContext(
      transferState.current_session.context.uri, trackId.uri,
      transferState.current_session.currentUid);
  if (!loadRes) {
    BELL_LOG(error, LOG_TAG, "Failed to load context: {}", loadRes.error());
    return tl::make_unexpected(loadRes.error());
  }

  trackQueueHandler->setQueue(transferState.queue.tracks);
  trackQueueHandler->setPlayingQueue(transferState.queue.isPlayingQueue);

  trackQueueHandler->updateTrackWindows();

  auto track = trackQueueHandler->currentTrack();
  if (track) {
    playerState.track = *track;
  }

  auto contextIndex = trackQueueHandler->currentContextIndex();
  playerState.index.hasValue = contextIndex.has_value();
  if (contextIndex) {
    playerState.index.value = *contextIndex;
  }

  playerState.duration = 0;

  BELL_LOG(info, LOG_TAG, "Current track after transfer: {}",
           track ? track->uri : "none");
  if (!putState()) {
    BELL_LOG(error, LOG_TAG, "Failed to put state");
    return {};
  }

  // Unconditional flush: a transfer command is the source's own authority
  // on what should be playing right now, so always tear down and reopen
  // from what this command says - including on Spotify's own ~15s
  // duplicate resend of a transfer, which otherwise left StreamPlayer's
  // queue position silently ahead of (or behind) what was just PUT above,
  // e.g. reporting index 27 here while audio kept playing whatever a
  // natural EOF had already advanced it to.
  eventLoop->post(EventLoop::EventType::PLAYER_FLUSH, std::monostate{});
  eventLoop->post(EventLoop::EventType::PLAYER_PLAY, !shouldPause);

  return {};
}

bell::Result<> ConnectStateHandler::handlePlayCommand(
    const tao::json::value& command) {
  // TODO: Handle overrides
  const tao::json::value& context = command.at("context");
  const tao::json::value& options = command.at("options");
  // skip_to isn't present on every play command (e.g. a plain "resume my
  // library" play has none) - options.at("skip_to") threw and silently
  // dropped the whole command on real hardware ("JSON object key
  // \"skip_to\" not found", caught generically by EventLoop's handler
  // try/catch).
  static const tao::json::value emptySkipTo = tao::json::empty_object;
  const tao::json::value* skipToPtr = options.find("skip_to");
  const tao::json::value& skipTo = skipToPtr ? *skipToPtr : emptySkipTo;
  auto contextUri = context.optional<std::string>("uri");
  auto skipToUid = skipTo.optional<std::string>("track_uid");
  auto skipToUri = skipTo.optional<std::string>("track_uri");
  bool initiallyPaused =
      options.optional<bool>("initially_paused").value_or(false);

  if (!contextUri) {
    BELL_LOG(error, LOG_TAG, "Play command missing context URI");
    return bell::make_unexpected_errc(std::errc::bad_message);
  }

  // Set active state - a bare "play" (no preceding transfer, e.g. the user
  // already had this device selected and just picked something new to
  // play) is just as much "this device is now the active one" as a
  // transfer is. Matches go-librespot's setActive(true) in its own "play"
  // case (daemon/player.go). Missing this left every subsequent putState
  // claiming isActive=false even while genuinely playing audio - real
  // hardware confirmed this reads as "not connected" on the client.
  putStateRequestProto.isActive = true;

  auto loadRes =
      trackQueueHandler->loadContext(*contextUri, skipToUri, skipToUid);
  if (!loadRes) {
    return tl::make_unexpected(loadRes.error());
  }

  trackQueueHandler->updateTrackWindows();

  // Unconditional flush - see handleTransferCommand()'s own comment on
  // this same point.
  eventLoop->post(EventLoop::EventType::PLAYER_FLUSH, std::monostate{});
  eventLoop->post(EventLoop::EventType::PLAYER_PLAY, !initiallyPaused);

  auto& playerState = putStateRequestProto.device.playerState;
  // isPlaying=true here too, same reasoning as handleTransferCommand()'s
  // own comment: a session being loaded (even mid-buffering, before
  // StreamPlayer's async fetch/CDN-open pipeline gets there) is what
  // isPlaying reports - not "audio already flowing". Explicit here (not
  // left to whatever isPlaying/isBuffering/isPaused already held) since
  // this putState() call below fires synchronously, before that.
  playerState.isPlaying = true;
  playerState.isBuffering = true;
  playerState.isPaused = initiallyPaused;

  auto track = trackQueueHandler->currentTrack();
  if (track) {
    playerState.track = *track;
  }

  auto contextIndex = trackQueueHandler->currentContextIndex();
  playerState.index.hasValue = contextIndex.has_value();
  if (contextIndex) {
    playerState.index.value = *contextIndex;
  }

  playerState.positionAsOfTimestamp = 0;
  playerState.timestamp =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();

  auto putRes = putState();
  if (!putRes) {
    BELL_LOG(error, LOG_TAG, "Failed to put state after play command");
    return putRes;
  }

  return {};
}

bell::Result<> ConnectStateHandler::handleSkipNextCommand() {
  return advanceToNextTrack();
}

bell::Result<> ConnectStateHandler::advanceToNextTrack() {
  auto res = trackQueueHandler->skipToNextTrack();
  if (!res) {
    BELL_LOG(error, LOG_TAG, "Failed to skip next track");
    return res;
  }

  trackQueueHandler->updateTrackWindows();

  auto& playerState = putStateRequestProto.device.playerState;
  auto track = trackQueueHandler->currentTrack();
  if (track) {
    playerState.track = *track;
  }

  auto contextIndex = trackQueueHandler->currentContextIndex();
  playerState.index.hasValue = contextIndex.has_value();
  if (contextIndex) {
    playerState.index.value = *contextIndex;
  }

  // A plain advance (skip_next or natural end of track, the only variant
  // this implements - go-librespot's own "skip to a specific track"
  // branch is a separate case we don't have) always resumes, never stays
  // paused: matches go-librespot's advanceNext(), which hardcodes
  // paused=false into loadCurrentTrackOrSkip() for this exact path. Also
  // re-announces isPlaying/isBuffering=true here: without this, this
  // putState() below would send the NEW track/index alongside whatever
  // isBuffering/isPlaying/playbackId the PREVIOUS, just-finished track had
  // left behind - an incoherent state (new track, "not buffering") that
  // this repo's own master branch traced a real playlist-switch UI
  // flicker back to.
  playerState.isPlaying = true;
  playerState.isBuffering = true;
  playerState.isPaused = false;

  playerState.positionAsOfTimestamp = 0;
  playerState.timestamp =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();

  auto putRes = putState();
  if (!putRes) {
    BELL_LOG(error, LOG_TAG, "Failed to put state after skip next");
    return putRes;
  }

  return {};
}

bell::Result<> ConnectStateHandler::handleSkipPrevCommand() {
  auto res = trackQueueHandler->skipToPreviousTrack();
  if (!res) {
    BELL_LOG(error, LOG_TAG, "Failed to skip previous track");
    return res;
  }

  trackQueueHandler->updateTrackWindows();

  auto& playerState = putStateRequestProto.device.playerState;
  auto track = trackQueueHandler->currentTrack();
  if (track) {
    playerState.track = *track;
  }

  auto contextIndex = trackQueueHandler->currentContextIndex();
  playerState.index.hasValue = contextIndex.has_value();
  if (contextIndex) {
    playerState.index.value = *contextIndex;
  }

  // Re-announce isPlaying/isBuffering=true - a new track is loading, so
  // whatever isBuffering/playbackId the PREVIOUS track's "ready" state left
  // behind would otherwise go out alongside this new track/index (see
  // advanceToNextTrack()'s own comment for the same fix). isPaused is
  // deliberately NOT touched here, unlike advanceToNextTrack(): go-librespot's
  // skipPrev() passes its current p.state.player.IsPaused straight through
  // to loadCurrentTrack() unchanged (preserve pause across skip_prev),
  // rather than hardcoding false the way its plain advanceNext() does.
  playerState.isPlaying = true;
  playerState.isBuffering = true;

  playerState.positionAsOfTimestamp = 0;
  playerState.timestamp =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();

  (void)putState();

  return {};
}

bell::Result<> ConnectStateHandler::handlePauseCommand(bool pause) {
  // Not routed through StreamPlayer::announceState()'s own
  // PLAYER_STATE_UPDATED flow: maybeStartCurrentTrack() (the only thing
  // that calls announceState()) is a no-op once the decoder is already
  // open, which is exactly the common case for pause/resume during
  // ongoing playback - so this needs its own explicit putState() call,
  // same as every other command handler here.
  eventLoop->post(EventLoop::EventType::PLAYER_PLAY, !pause);

  auto& playerState = putStateRequestProto.device.playerState;
  // isPlaying stays true - a session is loaded either way, only isPaused
  // changes. See PlayerStateUpdate's comment (EventModels.h) for why
  // conflating the two breaks the real Spotify app.
  playerState.isPlaying = true;
  playerState.isPaused = pause;
  playerState.timestamp =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();

  auto putRes = putState();
  if (!putRes) {
    BELL_LOG(error, LOG_TAG, "Failed to put state after pause/resume");
    return putRes;
  }

  return {};
}

bool ConnectStateHandler::encodeProtoTracks(pb_ostream_t* stream,
                                            const pb_field_t* field,
                                            bool previous) {
  auto tracks = previous ? trackQueueHandler->previousTracks()
                         : trackQueueHandler->nextTracks();
  for (auto& track : tracks) {
    if (track.uri.empty())
      break;
    void* trackPtr = &track;
    if (!nanopb_helper::StructCodec<
            cspot_proto::ProvidedTrack>::encodeSubmessage(stream, field,
                                                          &trackPtr)) {
      return false;
    }
  }
  return true;
}
