#include "ConnectStateHandler.h"

#include <iostream>
#include <random>

#include <tao/json.hpp>
#include "SessionContext.h"
#include "TrackQueue.h"
#include "Utils.h"
#include "api/SpClient.h"
#include "bell/Logger.h"
#include "bell/Result.h"
#include "connect.pb.h"
#include "events/EventLoop.h"
#include "mbedtls/base64.h"
#include "metadata.pb.h"
#include "proto/SpotifyId.h"

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
    std::shared_ptr<SessionContext> sessionContext,
    std::shared_ptr<SpClient> spClient, std::shared_ptr<ApClient> apClient)
    : sessionContext(std::move(sessionContext)),
      spClient(std::move(spClient)),
      apClient(std::move(apClient)) {
  trackQueue =
      std::make_shared<TrackQueue>(this->sessionContext, this->spClient);

  // this->sessionContext->eventLoop->registerHandler(
  //     EventLoop::EventType::CURRENT_TRACK_METADATA,
  //     [this](cspot::EventLoop::Event&& event) {
  //       auto& currentTrackMetadata =
  //           std::get<cspot::CurrentTrackMetadata>(event.payload);

  //       auto& playerState = putStateRequestProto.device.playerState;
  //       playerState.duration = currentTrackMetadata.durationMs;
  //     });

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
  deviceInfo.name = sessionContext->loginBlob->getDeviceName();

  deviceInfo.deviceType = DeviceType_SPEAKER;
  deviceInfo.deviceSoftwareVersion = deviceSoftwareVersion;
  deviceInfo.deviceId = sessionContext->loginBlob->getDeviceId();
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
  playerState.nextTracks.funcs.encode = TrackQueue::pbEncodeNextTracks;
  playerState.prevTracks.funcs.encode = TrackQueue::pbEncodePreviousTracks;
  playerState.nextTracks.arg = trackQueue.get();
  playerState.prevTracks.arg = trackQueue.get();
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

  return this->spClient->putConnectState(putStateRequestProto);
  return {};
}

bell::Result<> ConnectStateHandler::handleTransferCommand(
    std::string_view payloadDataStr, const tao::json::value& options) {
  size_t olen = 0;

  // Get the size of the base64 decoded data
  auto base64DecodeRes = mbedtls_base64_decode(
      nullptr, 0, &olen,
      reinterpret_cast<const uint8_t*>(payloadDataStr.data()),
      payloadDataStr.size());
  if (base64DecodeRes == 0) {
    BELL_LOG(error, LOG_TAG, "Failed to base64 decode payload data");
    return bell::make_unexpected_errc(std::errc::bad_message);
  }

  std::vector<uint8_t> decodedData(olen);
  // Decode the base64 data
  base64DecodeRes = mbedtls_base64_decode(
      decodedData.data(), decodedData.size(), &olen,
      reinterpret_cast<const uint8_t*>(payloadDataStr.data()),
      payloadDataStr.size());
  if (base64DecodeRes != 0) {
    BELL_LOG(error, LOG_TAG, "Failed to base64 decode payload data");
    return bell::make_unexpected_errc(std::errc::bad_message);
  }

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
  playerState.isPlaying = true;
  playerState.isBuffering = false;
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
  putStateRequestProto.startedPlayingAt = transferState.playback.timestamp;
  putStateRequestProto.hasBeenPlayingForMs = 0;

  trackQueue->setQueue(transferState.queue);

  SpotifyIdType trackType =
      SpotifyId::getTypeFromContext(transferState.current_session.context.uri);
  SpotifyId trackId =
      SpotifyId(trackType, transferState.playback.currentTrack.gid);

  auto provideRes = trackQueue->loadTrackAndContext(
      transferState.current_session.currentUid, trackId.uri,
      transferState.current_session.context);
  if (!provideRes) {
    BELL_LOG(error, LOG_TAG, "Failed to provide current track: {}",
             provideRes.error());
    return provideRes;
  }

  auto track = trackQueue->currentTrack();
  if (track) {
    playerState.track = *track;
    playerState.index.hasValue = true;
    playerState.index.value = trackQueue->currentContextIndex().value();
  } else {
    playerState.index.hasValue = false;
  }

  if (!putState()) {
    BELL_LOG(error, LOG_TAG, "Failed to put state");
    return {};
  }

  return {};
}

bell::Result<> ConnectStateHandler::handleSkipNextCommand() {
  auto res = trackQueue->skipToNextTrack();
  if (!res) {
    BELL_LOG(error, LOG_TAG, "Failed to skip next track");
    return res;
  }

  auto& playerState = putStateRequestProto.device.playerState;
  auto track = trackQueue->currentTrack();
  if (track) {
    playerState.track = *track;
    playerState.index.hasValue = true;
    playerState.index.value = trackQueue->currentContextIndex().value();
  } else {
    playerState.index.hasValue = false;
  }

  playerState.positionAsOfTimestamp = 0;
  playerState.timestamp =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();

  (void)putState();

  return {};
}

bell::Result<> ConnectStateHandler::handleSkipPrevCommand() {
  auto res = trackQueue->skipToPreviousTrack();
  if (!res) {
    BELL_LOG(error, LOG_TAG, "Failed to skip next track");
    return res;
  }

  auto& playerState = putStateRequestProto.device.playerState;
  auto track = trackQueue->currentTrack();
  if (track) {
    playerState.track = *track;
    playerState.index.hasValue = true;
    playerState.index.value = trackQueue->currentContextIndex().value();
  } else {
    playerState.index.hasValue = false;
  }

  playerState.positionAsOfTimestamp = 0;
  playerState.timestamp =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();

  if (!putState()) {
    BELL_LOG(error, LOG_TAG, "Failed to put state");
    return {};
  }

  return {};
}
