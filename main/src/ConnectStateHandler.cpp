#include "ConnectStateHandler.h"

#include <iostream>
#include <random>

#include <tao/json.hpp>
#include "SessionContext.h"
#include "Utils.h"
#include "api/SpClient.h"
#include "bell/Logger.h"
#include "bell/Result.h"
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
      randomEngine;
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
    std::shared_ptr<AuthInfo> authInfo, std::shared_ptr<SpClient> spClient)
    : eventLoop(std::move(eventLoop)),
      authInfo(std::move(authInfo)),
      spClient(std::move(spClient)) {
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
        playerState.isBuffering = playerStateUpdate.isBuffering;
        playerState.timestamp = playerStateUpdate.timestamp;

        (void)putState();
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
  capabilities.supports_logout = true;
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
  // get milliseconds since epoch;
  putStateRequestProto.clientSideTimestamp =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();
  putStateRequestProto.memberType = MemberType_CONNECT_STATE;
  putStateRequestProto.putStateReason = reason;

  return this->spClient->putConnectState(
      putStateRequestProto, authInfo->deviceId, authInfo->sessionId);
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

  // No playback yet
  playerState.isPlaying = false;
  playerState.isBuffering = true;
  playerState.timestamp = transferState.playback.timestamp;

  bool shouldPause =
      transferState.playback.isPaused &&
      options.optional<std::string>("restore_paused") == "restore";

  playerState.isPaused = shouldPause;
  playerState.contextUri = transferState.current_session.context.uri;
  playerState.contextUrl = transferState.current_session.context.url;
  playerState.options = transferState.options;
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
  trackQueueHandler->updateTrackWindows();

  eventLoop->post(EventLoop::EventType::PLAYER_FLUSH, true);
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

  if (!contextUri) {
    BELL_LOG(error, LOG_TAG, "Play command missing context URI");
    return bell::make_unexpected_errc(std::errc::bad_message);
  }

  auto loadRes =
      trackQueueHandler->loadContext(*contextUri, skipToUri, skipToUid);
  if (!loadRes) {
    return tl::make_unexpected(loadRes.error());
  }

  trackQueueHandler->updateTrackWindows();

  eventLoop->post(EventLoop::EventType::PLAYER_FLUSH, true);
  eventLoop->post(EventLoop::EventType::PLAYER_PLAY, true);

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

bell::Result<> ConnectStateHandler::handleSkipNextCommand() {
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

  playerState.positionAsOfTimestamp = 0;
  playerState.timestamp =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();

  (void)putState();

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
