#include "ConnectStateHandler.h"

#include <algorithm>
#include <charconv>
#include <iostream>
#include <random>

#include <tao/json.hpp>
#include <tao/json/contrib/traits.hpp>
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
#include "nonstd/expected.hpp"
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

// playback_speed: 0 while paused or buffering, 1 otherwise.
double computePlaybackSpeed(bool isPaused, bool isBuffering) {
  return (!isPaused && !isBuffering) ? 1.0 : 0.0;
}

// Parses the {uri, uid} pair off a JSON object shaped like a Spotify
// ContextTrack.
cspot_proto::ContextTrack parseTrackRef(const tao::json::value& trackJson) {
  cspot_proto::ContextTrack track;
  track.uri = trackJson.optional<std::string>("uri").value_or("");
  track.uid = trackJson.optional<std::string>("uid").value_or("");
  return track;
}

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
    std::shared_ptr<TimeProvider> timeProvider,
    std::shared_ptr<AudioSink> audioSink,
    PlaybackNotificationCallback playbackNotificationCallback)
    // Stack sized for this task's own network work (the connect-state PUT
    // round-trip), same as every other network-doing task in this
    // codebase.
    : bell::Task("cspot_connect_state", 32 * 1024),
      eventLoop(std::move(eventLoop)),
      authInfo(std::move(authInfo)),
      spClient(std::move(spClient)),
      timeProvider(std::move(timeProvider)),
      audioSink(std::move(audioSink)),
      playbackNotificationCallback(std::move(playbackNotificationCallback)) {
  trackQueueHandler =
      createDefaultTrackQueueHandler(this->spClient, this->eventLoop);

  // StreamPlayer ran out of audio (natural end of track) - repeat-track is
  // honored here, unlike an explicit remote skip_next.
  this->eventLoop->registerHandler(
      EventLoop::EventType::TRACK_ENDED, [this](cspot::EventLoop::Event&&) {
        handleTrackAdvanceSignal(AdvanceTrigger::TrackEnded);
      });

  // StreamPlayer could never load the track (CDN/audio key failure) -
  // skipped like an explicit skip_next, regardless of repeat-track
  // (there's no audio to repeat).
  this->eventLoop->registerHandler(
      EventLoop::EventType::TRACK_UNPLAYABLE,
      [this](cspot::EventLoop::Event&&) {
        handleTrackAdvanceSignal(AdvanceTrigger::TrackUnplayable);
      });

  // Outward "now playing" notifications (PlaybackNotifications.h) - posted
  // from onPlayerStateUpdate()/handlePauseCommandLocked()/applySeekLocked()
  // while putStateMutex is held, delivered here (this class's own
  // registration, on the eventLoop's dispatch thread) once it's been
  // released - see EventLoop::EventType::LOCAL_TRACK_CHANGED's own comment
  // for why this can't reuse PLAYER_PLAY/PLAYER_SEEK.
  this->eventLoop->registerHandler(
      EventLoop::EventType::LOCAL_TRACK_CHANGED,
      [this](cspot::EventLoop::Event&& ev) {
        auto& metadata = std::get<TrackMetadata>(ev.payload);
        this->playbackNotificationCallback(
            {PlaybackNotification::TrackChanged, metadata});
      });
  this->eventLoop->registerHandler(
      EventLoop::EventType::LOCAL_PLAY_PAUSE_CHANGED,
      [this](cspot::EventLoop::Event&& ev) {
        auto paused = std::get<bool>(ev.payload);
        this->playbackNotificationCallback(
            {PlaybackNotification::PlayPauseChanged, paused});
      });
  this->eventLoop->registerHandler(
      EventLoop::EventType::LOCAL_SEEKED,
      [this](cspot::EventLoop::Event&& ev) {
        auto positionMs = std::get<int64_t>(ev.payload);
        this->playbackNotificationCallback(
            {PlaybackNotification::Seeked,
             static_cast<uint32_t>(positionMs)});
      });

  initialize();
  startTask();
}

ConnectStateHandler::~ConnectStateHandler() {
  stopTask();
}

void ConnectStateHandler::onPlayerStateUpdate(
    const PlayerStateUpdate& playerStateUpdate) {
  std::scoped_lock lock(putStateMutex);
  auto& playerState = putStateRequestProto.device.playerState;
  playerState.duration = playerStateUpdate.playbackDurationMs;
  playerState.positionAsOfTimestamp = playerStateUpdate.positionAsOfTimestamp;
  playerState.isPlaying = playerStateUpdate.isPlaying;
  playerState.isPaused = playerStateUpdate.isPaused;
  playerState.isBuffering = playerStateUpdate.isBuffering;
  playerState.playbackSpeed = computePlaybackSpeed(playerState.isPaused,
                                                    playerState.isBuffering);
  playerState.timestamp = playerStateUpdate.timestamp;
  // Left untouched when nullopt (the isBuffering=true announce, before a
  // playback id is known) rather than cleared.
  if (playerStateUpdate.playbackId) {
    playerState.playbackId = *playerStateUpdate.playbackId;
  }

  if (playerStateUpdate.trackMetadata) {
    eventLoop->post(EventLoop::EventType::LOCAL_TRACK_CHANGED,
                    *playerStateUpdate.trackMetadata);
  }

  (void)putStateLocked();
}

void ConnectStateHandler::initialize() {
  auto& deviceProto = putStateRequestProto.device;

  auto& deviceInfo = deviceProto.deviceInfo;
  deviceInfo.canPlay = true;
  deviceInfo.volume = 65535;
  deviceInfo.name = authInfo->deviceName;

  deviceInfo.deviceType = DeviceType_SPEAKER;
  deviceInfo.deviceSoftwareVersion = deviceSoftwareVersion;
  deviceInfo.deviceId = authInfo->deviceId;
  deviceInfo.clientId = clientId;
  deviceInfo.spircVersion = spircVersion;

  auto& capabilities = deviceInfo.capabilities.rawProto;

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
  // TODO: probar dejar esto vacío (como hace go-librespot, que no genera
  // sessionId hasta el primer transfer/play) en vez de generarlo acá -
  // matches master hoy, no go-librespot.
  playerState.sessionId = generateSessionId();

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

  // Single critical section for the whole dispatch, not just the two
  // fields below - every specific handler assumes putStateMutex is
  // already held (see each xxxLocked()'s own declaration comment) rather
  // than taking it itself, so this is the one place responsible for that
  // for the player-command path (advanceToNextTrackLocked()'s other
  // caller, the TRACK_ENDED handler, takes it independently).
  std::scoped_lock lock(putStateMutex);
  putStateRequestProto.lastCommandMessageId =
      payload.at("message_id").get_unsigned();
  putStateRequestProto.lastCommandSentByDeviceId =
      payload.at("sent_by_device_id").get_string();

  if (endpoint == "transfer") {
    BELL_LOG(info, LOG_TAG, "Received transfer command");
    std::string_view payloadDataStr = command.as<std::string_view>("data");
    return handleTransferCommandLocked(payloadDataStr, command["options"]);
  } else if (endpoint == "skip_next") {
    BELL_LOG(info, LOG_TAG, "Received skip_next command");
    return handleSkipNextCommandLocked(command);
  } else if (endpoint == "skip_prev") {
    BELL_LOG(info, LOG_TAG, "Received skip_prev command");
    return handleSkipPrevCommandLocked();
  } else if (endpoint == "pause") {
    BELL_LOG(info, LOG_TAG, "Received pause command");
    return handlePauseCommandLocked(true);
  } else if (endpoint == "resume") {
    BELL_LOG(info, LOG_TAG, "Received resume command");
    return handlePauseCommandLocked(false);
  } else if (endpoint == "play") {
    BELL_LOG(info, LOG_TAG, "Received play command");
    return handlePlayCommandLocked(command);
  } else if (endpoint == "seek_to") {
    BELL_LOG(info, LOG_TAG, "Received seek_to command");
    return handleSeekCommandLocked(command);
  } else if (endpoint == "update_context") {
    BELL_LOG(info, LOG_TAG, "Received update_context command");
    return handleUpdateContextCommandLocked(command);
  } else if (endpoint == "set_repeating_context") {
    BELL_LOG(info, LOG_TAG, "Received set_repeating_context command");
    return applyRepeatContextLocked(command.optional<bool>("value"));
  } else if (endpoint == "set_repeating_track") {
    BELL_LOG(info, LOG_TAG, "Received set_repeating_track command");
    applyPlayerOptionsLocked(std::nullopt, command.optional<bool>("value"),
                             std::nullopt);
    return putStateLocked();
  } else if (endpoint == "set_shuffling_context") {
    BELL_LOG(info, LOG_TAG, "Received set_shuffling_context command");
    applyPlayerOptionsLocked(std::nullopt, std::nullopt,
                             command.optional<bool>("value"));
    return putStateLocked();
  } else if (endpoint == "set_options") {
    BELL_LOG(info, LOG_TAG, "Received set_options command");
    applyPlayerOptionsLocked(command.optional<bool>("repeating_context"),
                             command.optional<bool>("repeating_track"),
                             command.optional<bool>("shuffling_context"));
    return putStateLocked();
  } else if (endpoint == "set_queue") {
    BELL_LOG(info, LOG_TAG, "Received set_queue command");
    return handleSetQueueCommandLocked(command);
  } else if (endpoint == "add_to_queue") {
    BELL_LOG(info, LOG_TAG, "Received add_to_queue command");
    return handleAddToQueueCommandLocked(command);
  } else {
    BELL_LOG(info, LOG_TAG, "Received unknown command: {}", endpoint);
    return bell::make_unexpected_errc(std::errc::operation_not_supported);
  }

  return {};
}

bool ConnectStateHandler::requestPlayPause(bool play) {
  std::scoped_lock lock(putStateMutex);
  return bool(handlePauseCommandLocked(!play));
}

bool ConnectStateHandler::requestNext() {
  std::scoped_lock lock(putStateMutex);
  // No JSON to parse for a local button press - go straight to the
  // non-JSON half, same as requestSeek() bypasses handleSeekCommandLocked()
  // in favor of applySeekLocked().
  return bool(advanceToNextTrackLocked(AdvanceTrigger::LocalControl));
}

bool ConnectStateHandler::requestPrevious() {
  std::scoped_lock lock(putStateMutex);
  return bool(handleSkipPrevCommandLocked());
}

bool ConnectStateHandler::requestSeek(uint32_t positionMs) {
  std::scoped_lock lock(putStateMutex);
  return bool(applySeekLocked(static_cast<int64_t>(positionMs)));
}

bool ConnectStateHandler::requestSetRepeatContext(bool enabled) {
  std::scoped_lock lock(putStateMutex);
  return bool(applyRepeatContextLocked(enabled));
}

uint32_t ConnectStateHandler::getPositionMs() {
  std::scoped_lock lock(putStateMutex);
  auto nowMs = timeProvider->getSyncedTimestamp();
  auto& playerState = putStateRequestProto.device.playerState;
  return static_cast<uint32_t>(std::clamp<int64_t>(
      currentPositionMsLocked(nowMs), 0, playerState.duration));
}

bell::Result<> ConnectStateHandler::putState(PutStateReason reason) {
  std::scoped_lock lock(putStateMutex);
  return putStateLocked(reason);
}

bell::Result<> ConnectStateHandler::putStateLocked(PutStateReason reason) {
  constexpr auto kStatePutMinInterval = std::chrono::milliseconds(200);

  // Only schedules a flush for runTask() to send (see this method's own
  // declaration comment in the header). "Last reason wins": always
  // overwrite the pending reason, but only set the due time once per
  // burst - repeated calls inside the same window mustn't keep pushing
  // it out.
  pendingPutStateReason = reason;
  if (putStatePending) {
    return {};
  }
  putStatePending = true;
  putStateCv.notify_one();

  auto now = std::chrono::steady_clock::now();
  if (!lastPutStateTime || now - *lastPutStateTime >= kStatePutMinInterval) {
    putStateDueTime = now;
  } else {
    putStateDueTime = *lastPutStateTime + kStatePutMinInterval;
  }

  return {};
}

bool ConnectStateHandler::prepareAndEncodeLocked(
    PutStateReason reason, std::vector<std::byte>& outBody) {
  putStateRequestProto.clientSideTimestamp =
      timeProvider->getSyncedTimestamp();
  putStateRequestProto.memberType = MemberType_CONNECT_STATE;
  putStateRequestProto.putStateReason = reason;
  // This device's own outgoing sequence number - distinct from
  // lastCommandMessageId (which echoes an incoming command's id back).
  // Must increment on every PUT (matches master's own
  // sendPutStateRequest()), or the backend may read a repeated
  // message_id=0 as a replay rather than a new update.
  putStateRequestProto.messageId = ++nextMessageId;

  lastPutStateTime = std::chrono::steady_clock::now();
  putStatePending = false;

  {
    auto& ps = putStateRequestProto.device.playerState;
    BELL_LOG(debug, LOG_TAG,
             "PUT DIAG: "
             "hasTrack={} track.uri={} index=[{},{}] "
             "isPlaying={} isPaused={} isBuffering={} "
             "messageId={} lastCommandMessageId={} "
             "lastCommandSentByDeviceId={} startedPlayingAt={}",
             ps.track.hasValue,
             ps.track.value.uri, ps.index.value.page, ps.index.value.track,
             ps.isPlaying, ps.isPaused, ps.isBuffering,
             putStateRequestProto.messageId,
             putStateRequestProto.lastCommandMessageId,
             putStateRequestProto.lastCommandSentByDeviceId,
             putStateRequestProto.startedPlayingAt);
  }

  // Still under putStateMutex here: encoding touches
  // putStateRequestProto/trackQueueHandler (the next_tracks/prev_tracks
  // pb_callback reads trackQueueHandler's live windows). Everything
  // after this point in runTask() is a plain byte buffer, safe to send
  // unlocked.
  bool encodeRes = nanopb_helper::encodeToVector(putStateRequestProto, outBody);

  // Mirrors the RAW TransferState dump above, for our own outgoing side.
  if (encodeRes &&
      bell::BaseLogger::instance().shouldLog(bell::LogLevel::debug)) {
    std::string hex;
    hex.reserve(outBody.size() * 2);
    static const char* hexDigits = "0123456789abcdef";
    for (std::byte b : outBody) {
      auto v = std::to_integer<uint8_t>(b);
      hex += hexDigits[v >> 4];
      hex += hexDigits[v & 0x0f];
    }
    BELL_LOG(debug, LOG_TAG, "RAW outgoing PutStateRequest bytes ({}): {}",
             outBody.size(), hex);
  }

  return encodeRes;
}

void ConnectStateHandler::runTask() {
  std::scoped_lock runningLock(taskRunningMutex);
  taskRunning = true;

  std::unique_lock<std::mutex> lock(putStateMutex);
  while (taskRunning) {
    if (!putStatePending) {
      putStateCv.wait(lock,
                      [this] { return putStatePending || !taskRunning; });
      continue;
    }
    if (std::chrono::steady_clock::now() < putStateDueTime) {
      putStateCv.wait_until(lock, putStateDueTime,
                            [this] { return !taskRunning; });
      continue;
    }

    PutStateReason reason = pendingPutStateReason;
    std::vector<std::byte> encodedBody;
    bool encodeOk = prepareAndEncodeLocked(reason, encodedBody);
    std::string deviceId = authInfo->deviceId;
    std::string sessionId = authInfo->sessionId;
    bool isActive = putStateRequestProto.isActive;
    bool isBuffering = putStateRequestProto.device.playerState.isBuffering;
    bool isPaused = putStateRequestProto.device.playerState.isPaused;

    lock.unlock();

    if (!encodeOk) {
      BELL_LOG(error, LOG_TAG, "Failed to encode PutStateRequest, dropping "
                               "this flush");
    } else {
      // This runs with putStateMutex already released (see the unlock()
      // above). Used to run locked, but the round-trip itself can take
      // 250-1500ms normally, up to ~11s worst case (SpClient's own
      // retry/timeout budget) - holding the lock that long blocked every
      // other state mutator, including StreamPlayer's synchronous
      // onPlayerStateUpdate(), which sits right on the track-load path.
      // Safe to call unlocked because putConnectStateRaw() takes the
      // already-encoded body, not the live proto.
      //
      // The heap snapshots around it: this device can have up to three
      // other TLS contexts alive at the same time (AP connection, dealer
      // websocket, CDN stream), so it's worth watching memory here too.
      logHeapStatus(LOG_TAG, "before putConnectState");
      auto putStartTime = std::chrono::steady_clock::now();
      auto res = spClient->putConnectStateRaw(std::move(encodedBody),
                                              deviceId, sessionId);
      auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - putStartTime)
                           .count();

      logHeapStatus(LOG_TAG, "after putConnectState");

      // Round-trip time logged on both outcomes - this PUT is what the
      // initiating client's "Connecting..." wait actually depends on.
      if (res) {
        BELL_LOG(info, LOG_TAG,
                 "Put state succeeded in {}ms (reason={}, isActive={}, "
                 "isBuffering={}, isPaused={})",
                 elapsedMs, static_cast<int>(reason), isActive, isBuffering,
                 isPaused);
      } else {
        BELL_LOG(error, LOG_TAG,
                 "Put state failed after {}ms (reason={}, isActive={}): {}",
                 elapsedMs, static_cast<int>(reason), isActive, res.error());
      }
    }

    lock.lock();
  }
}

bell::Result<> ConnectStateHandler::handleClusterUpdate(
    std::string_view payloadDataStr) {
  auto decodedData = base64Decode(payloadDataStr);
  if (!decodedData) {
    BELL_LOG(error, LOG_TAG, "Failed to base64 decode cluster update");
    return nonstd::make_unexpected(decodedData.error());
  }
  cspot_proto::ClusterUpdate clusterUpdate;

  bool res = nanopb_helper::decodeFromVector(clusterUpdate, *decodedData);
  if (!res) {
    BELL_LOG(error, LOG_TAG, "Failed to decode cluster update");
    return bell::make_unexpected_errc(std::errc::bad_message);
  }

  std::unique_lock<std::mutex> lock(putStateMutex);

  BELL_LOG(debug, LOG_TAG,
           "CLUSTER DIAG: weThinkActive={} ourDeviceId={} "
           "activeDeviceId={} updateReason={} playerState.hasValue={} "
           "playerState.timestamp={}",
           putStateRequestProto.isActive, authInfo->deviceId,
           clusterUpdate.cluster.activeDeviceId,
           static_cast<int>(clusterUpdate.updateReason),
           clusterUpdate.cluster.playerState.hasValue,
           clusterUpdate.cluster.playerState.value.timestamp);

  // Someone else just became the active device while we thought we were -
  // back off unconditionally (matches master).
  bool stopBeingActive = putStateRequestProto.isActive &&
                         clusterUpdate.cluster.activeDeviceId !=
                             authInfo->deviceId;

  if (!stopBeingActive) {
    return {};
  }

  BELL_LOG(info, LOG_TAG, "Playback was transferred to device {}",
           clusterUpdate.cluster.activeDeviceId);

  putStateRequestProto.isActive = false;
  eventLoop->post(EventLoop::EventType::PLAYER_PLAY, false);

  {
    auto& ps = putStateRequestProto.device.playerState;
    BELL_LOG(debug, LOG_TAG,
             "LAST STATE DIAG (before putInactive): contextUri={} "
             "contextUrl={} track.hasValue={} track.uri={} playbackId={} "
             "index.hasValue={} index=[{},{}] shuffling={} repeating={} "
             "repeatingTrack={} playOrigin.feature={} playbackSpeed={} "
             "positionAsOfTimestamp={} duration={} isPlaying={} "
             "isBuffering={} isPaused={} sessionId={} position={} "
             "timestamp={}",
             ps.contextUri, ps.contextUrl, ps.track.hasValue,
             ps.track.value.uri, ps.playbackId, ps.index.hasValue,
             ps.index.value.page, ps.index.value.track,
             ps.options.shufflingContext, ps.options.repeatingContext,
             ps.options.repeatingTrack, ps.playOrigin.featureIdentifier,
             ps.playbackSpeed, ps.positionAsOfTimestamp, ps.duration,
             ps.isPlaying, ps.isBuffering, ps.isPaused, ps.sessionId,
             ps.position, ps.timestamp);
  }

  // Mutations are done - release the lock before the network round-trip
  // so this doesn't block onPlayerStateUpdate() (audio thread,
  // synchronous) or any request*() local-control entry point for
  // whatever putInactive() takes (up to ~11s worst case).
  lock.unlock();

  auto inactiveRes = spClient->putInactive(authInfo->deviceId,
                                           authInfo->sessionId);
  if (!inactiveRes) {
    BELL_LOG(error, LOG_TAG, "Failed to put inactive state: {}",
             inactiveRes.error());
    return inactiveRes;
  }

  return {};
}

bell::Result<> ConnectStateHandler::putInactive() {
  return spClient->putInactive(authInfo->deviceId, authInfo->sessionId);
}

bell::Result<> ConnectStateHandler::handleSetVolume(
    std::string_view payloadDataStr) {
  auto decodedData = base64Decode(payloadDataStr);
  if (!decodedData) {
    BELL_LOG(error, LOG_TAG, "Failed to base64 decode set volume command");
    return nonstd::make_unexpected(decodedData.error());
  }
  cspot_proto::SetVolumeCommand setVolumeCommand;

  bool res = nanopb_helper::decodeFromVector(setVolumeCommand, *decodedData);
  if (!res) {
    BELL_LOG(error, LOG_TAG, "Failed to decode set volume command");
    return bell::make_unexpected_errc(std::errc::bad_message);
  }

  uint16_t volume = static_cast<uint16_t>(
      std::clamp<int32_t>(setVolumeCommand.volume, 0, 65535));
  BELL_LOG(info, LOG_TAG, "Set volume to {}", volume);

  bell::Result<> putRes;
  {
    std::scoped_lock lock(putStateMutex);
    putStateRequestProto.device.deviceInfo.volume = volume;
    putRes = putStateLocked(PutStateReason_VOLUME_CHANGED);
  }

  // Runs unlocked - AudioSink's contract doesn't guarantee a non-blocking
  // implementation, and putStateLocked() above already scheduled the
  // flush (it never sends inline), so nothing past this point needs
  // putStateMutex.
  audioSink->volumeChanged(volume);

  if (!putRes) {
    BELL_LOG(error, LOG_TAG, "Failed to put state after volume change: {}",
             putRes.error());
    return putRes;
  }

  return {};
}

bell::Result<> ConnectStateHandler::handleTransferCommandLocked(
    std::string_view payloadDataStr, const tao::json::value& options) {
  auto decodedDataRes = base64Decode(payloadDataStr);
  if (!decodedDataRes) {
    BELL_LOG(error, LOG_TAG, "Failed to base64 decode transfer state");
    return nonstd::make_unexpected(decodedDataRes.error());
  }
  auto& decodedData = *decodedDataRes;
  cspot_proto::TransferState transferState;

  // Raw hex dump to catch a field-number mismatch in this file's
  // hand-written ConnectPb.h bindings, which would silently decode to
  // defaults without nanopb ever erroring. Gated behind the debug log
  // level so building it isn't a cost paid on every transfer command.
  if (bell::BaseLogger::instance().shouldLog(bell::LogLevel::debug)) {
    std::string hex;
    hex.reserve(decodedData.size() * 2);
    static const char* hexDigits = "0123456789abcdef";
    for (std::byte b : decodedData) {
      auto v = std::to_integer<uint8_t>(b);
      hex += hexDigits[v >> 4];
      hex += hexDigits[v & 0x0f];
    }
    BELL_LOG(debug, LOG_TAG, "RAW TransferState bytes ({}): {}",
             decodedData.size(), hex);
  }

  bool res = nanopb_helper::decodeFromVector(transferState, decodedData);
  if (!res) {
    BELL_LOG(error, LOG_TAG, "Failed to decode transfer state");
    return bell::make_unexpected_errc(std::errc::bad_message);
  }

  BELL_LOG(info, LOG_TAG, "Transfer state decoded successfully");

  consecutiveUnplayableSkips = 0;

  putStateRequestProto.isActive = true;

  auto& playerState = putStateRequestProto.device.playerState;

  if (transferState.current_session.originalSessionId.hasValue) {
    playerState.sessionId =
        transferState.current_session.originalSessionId.value;
  } else {
    playerState.sessionId = generateSessionId();
  }
  BELL_LOG(info, LOG_TAG,
           "TransferState session id: hadOriginal={}, using sessionId={}",
           transferState.current_session.originalSessionId.hasValue,
           playerState.sessionId);

  // isPlaying means "session active", not "audio already flowing" -
  // stays true through buffering.
  playerState.isPlaying = true;
  playerState.isBuffering = true;
  // Our own clock, not transferState.playback.timestamp (the source
  // device's own, already-stale timestamp) - matches every other
  // handler in this file and both reference engines, neither of which
  // lets that raw source timestamp reach the network.
  int64_t nowMs = timeProvider->getSyncedTimestamp();
  playerState.timestamp = nowMs;

  bool shouldPause =
      transferState.playback.isPaused &&
      options.optional<std::string>("restore_paused") == "restore";

  // Extrapolate the source's position forward by however long this
  // command took to arrive/process - transferState.playback.timestamp can
  // be well in the past by now. Not needed while paused.
  int64_t effectivePositionMs = transferState.playback.positionAsOfTimestamp;
  if (!shouldPause && transferState.playback.timestamp > 0) {
    constexpr int64_t kMaxReasonableElapsedMs = 10 * 60 * 1000;
    int64_t elapsedMs = nowMs - transferState.playback.timestamp;
    if (elapsedMs >= 0 && elapsedMs <= kMaxReasonableElapsedMs) {
      effectivePositionMs += elapsedMs;
    }
  }

  BELL_LOG(info, LOG_TAG,
           "Transfer playback state: sourceIsPaused={}, restore_paused={}, "
           "shouldPause={} (posting PLAYER_PLAY={}), "
           "positionAsOfTimestamp={}ms, extrapolated to {}ms",
           transferState.playback.isPaused,
           options.optional<std::string>("restore_paused").value_or("<none>"),
           shouldPause, !shouldPause,
           transferState.playback.positionAsOfTimestamp, effectivePositionMs);

  playerState.isPaused = shouldPause;
  playerState.playbackSpeed =
      computePlaybackSpeed(playerState.isPaused, playerState.isBuffering);
  playerState.contextUri = transferState.current_session.context.uri;
  playerState.contextUrl = transferState.current_session.context.url;
  // options (shuffle/repeat) not copied from transferState here -
  // matches master (no PlayerState.options write found anywhere in its
  // tree). go-librespot does copy this from TransferState.Options on
  // every transfer - a known gap, not yet fixed.
  playerState.suppressions = transferState.current_session.suppressions;
  playerState.playOrigin = transferState.current_session.playOrigin;
  playerState.playOrigin.deviceIdentifier =
      putStateRequestProto.lastCommandSentByDeviceId;
  playerState.position = 0;
  playerState.positionAsOfTimestamp = effectivePositionMs;
  putStateRequestProto.startedPlayingAt = nowMs;
  putStateRequestProto.hasBeenPlayingForMs = 0;

  // Clears any context left over from an earlier transfer in this same
  // session before deciding what this one actually needs - haveContext's
  // own loadContext() call below re-populates it from scratch anyway, so
  // this only changes behavior for the other three branches, which
  // otherwise left a stale contextPages/contextIndex silently readable
  // by currentTrack()/currentContextIndex().
  // TODO: fixes a bug reasoned out from code review (go-librespot
  // comparison), not yet reproduced on hardware - unconfirmed whether a
  // real transfer sequence actually hits this. Revisit if it turns out
  // not to be needed.
  trackQueueHandler->clearContext();

  nextManualQueueId = 0;
  for (auto& track : transferState.queue.tracks) {
    const std::string& uid = track.uid;
    if (uid.size() > 1 && uid[0] == 'q') {
      uint64_t n = 0;
      auto [ptr, ec] = std::from_chars(uid.data() + 1,
                                       uid.data() + uid.size(), n);
      if (ec == std::errc() && ptr == uid.data() + uid.size()) {
        nextManualQueueId = std::max(nextManualQueueId, n);
      }
    }
  }

  bool haveContext = !transferState.current_session.context.uri.empty();

  if (haveContext) {
    SpotifyIdType trackType = SpotifyId::getTypeFromContext(
        transferState.current_session.context.uri);
    std::string currentTrackUri =
        transferState.playback.currentTrack.resolvedUri(trackType);

    // context.uri/currentUid are needed to resolve "the current track" -
    // both master (contextResolver.resolve()) and go-librespot
    // (loadContext(), before loadCurrentTrack()) resolve first too.
    //
    // This network fetch runs with putStateMutex still held (by the
    // caller) rather than released around it, so a half-updated transfer
    // never gets flushed - but it blocks every other lock-taker for its
    // duration: onPlayerStateUpdate() (audio thread, synchronous),
    // runTask() (only delays its own flush, not a deadlock), and all six
    // request*() local-control entry points, two of which (requestNext(),
    // requestPrevious()) can independently reach the network the same way
    // via TrackQueueHandler::ensureEnoughTracks().
    auto loadRes = trackQueueHandler->loadContext(
        transferState.current_session.context.uri, currentTrackUri,
        transferState.current_session.currentUid);
    if (!loadRes) {
      BELL_LOG(error, LOG_TAG, "Failed to load context: {}", loadRes.error());
      return nonstd::make_unexpected(loadRes.error());
    }

    trackQueueHandler->setQueue(transferState.queue.tracks);
    trackQueueHandler->setPlayingQueue(transferState.queue.isPlayingQueue);
  } else if (!transferState.queue.tracks.empty()) {
    // The queue itself is the whole playback source here, not an
    // override layered on top of a context - matches master's own
    // haveContext==false, has_queue branch.
    BELL_LOG(info, LOG_TAG,
             "Transfer has no context, playing queue directly ({} tracks)",
             transferState.queue.tracks.size());
    trackQueueHandler->setQueue(transferState.queue.tracks);
    trackQueueHandler->setPlayingQueue(true);
  } else if (!transferState.playback.currentTrack
                  .resolvedUri(SpotifyIdType::Track)
                  .empty()) {
    // Single-track transfer: no context/queue, but a real current_track -
    // matches master's own single-track fallback. Modeled as a one-entry
    // queue (this file has no separate "just this track" concept).
    BELL_LOG(info, LOG_TAG,
             "Transfer has no context/queue, playing single track {}",
             transferState.playback.currentTrack.resolvedUri(
                 SpotifyIdType::Track));
    trackQueueHandler->setQueue({transferState.playback.currentTrack});
    trackQueueHandler->setPlayingQueue(true);
  } else {
    // Genuinely empty transfer (device selected while nothing plays
    // anywhere yet)
    BELL_LOG(info, LOG_TAG,
             "Transfer has no context/queue/track - becoming active with "
             "nothing loaded");
    trackQueueHandler->setQueue({});
    trackQueueHandler->setPlayingQueue(false);
  }

  trackQueueHandler->updateTrackWindows();

  auto track = trackQueueHandler->currentTrack();
  // hasValue set explicitly - omitted (not sent empty) when there's no
  // current track. Copies the whole ProvidedTrack (uri, uid, provider) -
  // a bare uri drops uid, which Spotify needs to locate this track within
  // its context/queue.
  playerState.track.hasValue = static_cast<bool>(track);
  if (track) {
    playerState.track.value = *track;
  }

  // Index of the current track within its context - go-librespot sets
  // this on every playback transition; master never sends it.
  auto contextIndex = trackQueueHandler->currentContextIndex();
  playerState.index.hasValue = contextIndex.has_value();
  if (contextIndex) {
    playerState.index.value = *contextIndex;
  }

  BELL_LOG(info, LOG_TAG, "Current track after transfer: {}",
           track ? track->uri : "none");
  if (!putStateLocked()) {
    BELL_LOG(error, LOG_TAG, "Failed to put state");
    return {};
  }

  // Posted only after trackQueueHandler's context/queue/windows are
  // resolved above, so StreamPlayer never reopens a stale currentFile
  // left over from before this transfer.
  eventLoop->post(EventLoop::EventType::PLAYER_FLUSH,
                  FlushResumeState{effectivePositionMs, !shouldPause});

  return {};
}

bell::Result<> ConnectStateHandler::handlePlayCommandLocked(
    const tao::json::value& command) {
  const tao::json::value& context = command.at("context");
  const tao::json::value& options = command.at("options");
  // skip_to isn't present on every play command (e.g. a plain "resume my
  // library" play has none) - options.at("skip_to") threw and silently
  // dropped the whole command on real hardware.
  static const tao::json::value emptySkipTo = tao::json::empty_object;
  const tao::json::value* skipToPtr = options.find("skip_to");
  const tao::json::value& skipTo = skipToPtr ? *skipToPtr : emptySkipTo;
  auto contextUri = context.optional<std::string>("uri");
  auto skipToUid = skipTo.optional<std::string>("track_uid");
  auto skipToUri = skipTo.optional<std::string>("track_uri");
  // Only meaningful when neither uid nor uri is present - matches
  // go-librespot's own uid/uri/index priority order (daemon/player.go's
  // skipToFunc). >0 rather than a presence check because Options.SkipTo
  // isn't a pointer on go-librespot's side either, so index 0 there is
  // already indistinguishable from "absent" - harmless either way, since
  // both fall back to the same "start from the beginning" default.
  std::optional<uint32_t> skipToTrackIndex;
  if (!skipToUid && !skipToUri) {
    auto skipToTrackIndexRaw = skipTo.optional<int>("track_index");
    if (skipToTrackIndexRaw && *skipToTrackIndexRaw > 0) {
      skipToTrackIndex = static_cast<uint32_t>(*skipToTrackIndexRaw);
    }
  }
  bool initiallyPaused =
      options.optional<bool>("initially_paused").value_or(false);
  // Only overrides fields actually present in the JSON (optional<bool>
  // tells "absent" from "sent false") - more precise than go-librespot's
  // own handling of this same field, which overwrites all three
  // unconditionally whenever the override object is present at all
  // (ContextPlayerOptionOverrides uses plain bool, not optional, in its
  // own proto).
  const tao::json::value* overrideJson =
      options.find("player_options_override");

  if (!contextUri) {
    BELL_LOG(error, LOG_TAG, "Play command missing context URI");
    return bell::make_unexpected_errc(std::errc::bad_message);
  }

  consecutiveUnplayableSkips = 0;

  // A bare "play" (no preceding transfer) is just as much "this device
  // is now active" as a transfer is - without this, isActive stayed
  // false even while genuinely playing audio.
  putStateRequestProto.isActive = true;
  putStateRequestProto.device.playerState.sessionId = generateSessionId();

  // See handleTransferCommandLocked()'s own comment on this same network
  // fetch running with putStateMutex still held.
  auto loadRes = trackQueueHandler->loadContext(*contextUri, skipToUri,
                                                skipToUid, skipToTrackIndex);
  if (!loadRes) {
    return nonstd::make_unexpected(loadRes.error());
  }

  trackQueueHandler->updateTrackWindows();

  auto track = trackQueueHandler->currentTrack();

  eventLoop->post(EventLoop::EventType::PLAYER_FLUSH, std::monostate{});
  eventLoop->post(EventLoop::EventType::PLAYER_PLAY, !initiallyPaused);

  auto& playerState = putStateRequestProto.device.playerState;
  // isPlaying=true even mid-buffering - same reasoning as
  // handleTransferCommandLocked().
  playerState.isPlaying = true;
  playerState.isBuffering = true;
  playerState.isPaused = initiallyPaused;
  playerState.playbackSpeed =
      computePlaybackSpeed(playerState.isPaused, playerState.isBuffering);

  if (overrideJson) {
    applyPlayerOptionsLocked(overrideJson->optional<bool>("repeating_context"),
                             overrideJson->optional<bool>("repeating_track"),
                             overrideJson->optional<bool>("shuffling_context"));
  }

  // hasValue set explicitly - omitted (not sent empty) when there's no
  // current track. Copies the whole ProvidedTrack (uri, uid, provider) -
  // see the same assignment in handleTransferCommandLocked() for why.
  playerState.track.hasValue = static_cast<bool>(track);
  if (track) {
    playerState.track.value = *track;
  }

  // Track index within context - see handleTransferCommandLocked()'s comment.
  auto contextIndex = trackQueueHandler->currentContextIndex();
  playerState.index.hasValue = contextIndex.has_value();
  if (contextIndex) {
    playerState.index.value = *contextIndex;
  }

  // contextUri/contextUrl/playOrigin/suppressions, unlike
  // handleTransferCommandLocked(), were never set here - left holding the
  // PREVIOUS transfer/play's values while track.value.uri already pointed
  // at the new context, an internally contradictory PUT (confirmed on
  // real hardware to surface as "Spotify can't play this right now").
  // play_origin/suppressions match go-librespot's own "play" case
  // (PlayOrigin = req.Command.PlayOrigin, DeviceIdentifier always
  // overwritten; Suppressions = req.Command.Options.Suppressions,
  // unconditional either way).
  playerState.contextUri = *contextUri;
  playerState.contextUrl = context.optional<std::string>("url").value_or("");
  static const tao::json::value emptyPlayOrigin = tao::json::empty_object;
  const tao::json::value* playOriginJsonPtr = command.find("play_origin");
  const tao::json::value& playOriginJson =
      playOriginJsonPtr ? *playOriginJsonPtr : emptyPlayOrigin;
  playerState.playOrigin.featureIdentifier =
      playOriginJson.optional<std::string>("feature_identifier").value_or("");
  playerState.playOrigin.referrerIdentifier =
      playOriginJson.optional<std::string>("referrer_identifier").value_or("");
  playerState.playOrigin.deviceIdentifier =
      putStateRequestProto.lastCommandSentByDeviceId;

  playerState.suppressions.providers.clear();
  if (const tao::json::value* suppressionsJson = options.find("suppressions")) {
    if (const tao::json::value* providersJson =
            suppressionsJson->find("providers")) {
      providersJson->to(playerState.suppressions.providers);
    }
  }

  playerState.positionAsOfTimestamp = 0;
  playerState.timestamp =
      timeProvider->getSyncedTimestamp();

  auto putRes = putStateLocked();
  if (!putRes) {
    BELL_LOG(error, LOG_TAG, "Failed to put state after play command");
    return putRes;
  }

  return {};
}

bell::Result<> ConnectStateHandler::handleSkipNextCommandLocked(
    const tao::json::value& command) {
  // "track" is only present when the remote client named an explicit
  // track to jump to (e.g. clicking an item in the Queue panel) - a plain
  // "next" button press sends skip_next with no track at all.
  std::string targetTrackUri;
  std::string targetTrackUid;
  if (const tao::json::value* trackPtr = command.find("track")) {
    auto track = parseTrackRef(*trackPtr);
    targetTrackUri = track.uri;
    targetTrackUid = track.uid;
  }

  return advanceToNextTrackLocked(AdvanceTrigger::RemoteSkipNext,
                                  targetTrackUri, targetTrackUid);
}

bell::Result<> ConnectStateHandler::handleAddToQueueCommandLocked(
    const tao::json::value& command) {
  const tao::json::value* trackPtr = command.find("track");
  if (!trackPtr) {
    BELL_LOG(error, LOG_TAG, "add_to_queue command missing track");
    return bell::make_unexpected_errc(std::errc::bad_message);
  }

  cspot_proto::ContextTrack track = parseTrackRef(*trackPtr);
  // A track without a uri or uid would leave a hole in nextTracksWindow
  // that skipToTargetTrack() reads as end-of-window.
  if (track.uri.empty() && track.uid.empty()) {
    BELL_LOG(error, LOG_TAG, "add_to_queue command's track has no uri/uid");
    return bell::make_unexpected_errc(std::errc::bad_message);
  }
  if (track.uid.empty()) {
    track.uid = "q" + std::to_string(++nextManualQueueId);
  }

  trackQueueHandler->addToQueue(track);
  trackQueueHandler->updateTrackWindows();
  return putStateLocked();
}

bell::Result<> ConnectStateHandler::handleSetQueueCommandLocked(
    const tao::json::value& command) {
  // prev_tracks is accepted on the wire but unused - only next_tracks
  // carries anything reorderQueue() needs.
  std::vector<cspot_proto::ContextTrack> queuedPrefix;
  const tao::json::value* nextTracksPtr = command.find("next_tracks");
  if (nextTracksPtr && nextTracksPtr->is_array()) {
    for (const auto& trackJson : nextTracksPtr->get_array()) {
      const tao::json::value* metadataPtr = trackJson.find("metadata");
      bool isQueued = metadataPtr &&
          metadataPtr->optional<std::string>("is_queued").value_or("") ==
              "true";
      if (!isQueued) {
        break;
      }

      auto track = parseTrackRef(trackJson);
      // A track without a uri or uid would leave a hole in
      // nextTracksWindow that skipToTargetTrack() reads as end-of-window.
      if (track.uri.empty() && track.uid.empty()) {
        break;
      }
      queuedPrefix.push_back(std::move(track));
    }
  }

  trackQueueHandler->reorderQueue(queuedPrefix);
  trackQueueHandler->updateTrackWindows();
  return putStateLocked();
}

bell::Result<> ConnectStateHandler::advanceToNextTrackLocked(
    AdvanceTrigger trigger, const std::string& targetTrackUri,
    const std::string& targetTrackUid) {
  auto& playerState = putStateRequestProto.device.playerState;

  // See AdvanceTrigger's own comment for what each value means.
  bool forceNext = trigger != AdvanceTrigger::TrackEnded;
  bool streamPlayerCleared = trigger == AdvanceTrigger::TrackEnded ||
                              trigger == AdvanceTrigger::TrackUnplayable;

  bool hasNextTrack = true;

  if (!forceNext && playerState.options.repeatingTrack) {
    // Natural end of track with repeat-track on: don't advance, replay
    // the same track.
  } else {
    auto res = trackQueueHandler->skipToNextTrack(targetTrackUri, targetTrackUid);
    if (!res) {
      BELL_LOG(error, LOG_TAG, "Failed to skip next track");
      return nonstd::make_unexpected(res.error());
    }
    // Running off the end of the context always wraps the cursor to its
    // start - only treat that as a real next track when repeat-context
    // is on; otherwise pause there instead of looping unasked.
    hasNextTrack = (*res == TrackAdvanceResult::Advanced) ||
                   playerState.options.repeatingContext;
  }

  // See this method's own header comment on streamPlayerCleared - forces
  // past updateTrackWindows()'s dedup whenever StreamPlayer already
  // cleared its currentTrackId (repeat-track replay, or any case -
  // repeat-track or a wrapped-to-start/ad-hoc single track - where the
  // resulting current-track uri is unchanged from before).
  trackQueueHandler->updateTrackWindows(streamPlayerCleared);

  auto track = trackQueueHandler->currentTrack();
  // hasValue set explicitly - omitted (not sent empty) when there's no
  // current track. Copies the whole ProvidedTrack (uri, uid, provider) -
  // see the same assignment in handleTransferCommandLocked() for why.
  playerState.track.hasValue = static_cast<bool>(track);
  if (track) {
    playerState.track.value = *track;
  }

  // Track index within context - see handleTransferCommandLocked()'s comment.
  auto contextIndex = trackQueueHandler->currentContextIndex();
  playerState.index.hasValue = contextIndex.has_value();
  if (contextIndex) {
    playerState.index.value = *contextIndex;
  }

  // Re-announces isPlaying/isBuffering=true so this PUT doesn't pair the
  // new track/index with the PREVIOUS, just-finished track's buffering
  // state - traced on this repo's own master branch to a real playlist-
  // switch UI flicker. isPaused mirrors !hasNextTrack: a normal advance
  // (or a repeat wrap) always resumes, matching go-librespot's
  // advanceNext(); running out of context without repeat-context pauses
  // on the wrapped-to-start track instead of looping or freezing.
  playerState.isPlaying = true;
  playerState.isBuffering = true;
  playerState.isPaused = !hasNextTrack;
  playerState.playbackSpeed =
      computePlaybackSpeed(playerState.isPaused, playerState.isBuffering);

  playerState.positionAsOfTimestamp = 0;
  playerState.timestamp =
      timeProvider->getSyncedTimestamp();

  if (!hasNextTrack) {
    // StreamPlayer's own isPlaying otherwise stays true from before this
    // call (nothing else would clear it), which would start audio the
    // instant the wrapped-to-start track loads instead of pausing there.
    eventLoop->post(EventLoop::EventType::PLAYER_PLAY, false);
  }

  auto putRes = putStateLocked();
  if (!putRes) {
    BELL_LOG(error, LOG_TAG, "Failed to put state after skip next");
    return putRes;
  }

  return {};
}

void ConnectStateHandler::handleTrackAdvanceSignal(AdvanceTrigger trigger) {
  // Matches go-librespot's own maxConsecutiveUnplayableSkips (controls.go) -
  // guards against a context where every track is unplayable.
  constexpr int kMaxConsecutiveUnplayableSkips = 50;

  std::scoped_lock lock(putStateMutex);

  if (trigger == AdvanceTrigger::TrackUnplayable) {
    if (++consecutiveUnplayableSkips > kMaxConsecutiveUnplayableSkips) {
      BELL_LOG(error, LOG_TAG,
               "Giving up after {} consecutive unplayable tracks",
               consecutiveUnplayableSkips - 1);
      auto& playerState = putStateRequestProto.device.playerState;
      playerState.isPaused = true;
      playerState.isBuffering = false;
      playerState.playbackSpeed = computePlaybackSpeed(
          playerState.isPaused, playerState.isBuffering);
      eventLoop->post(EventLoop::EventType::PLAYER_PLAY, false);
      (void)putStateLocked();
      return;
    }
  } else {
    // Natural end of track proves the one that just finished WAS playable -
    // breaks any prior unplayable streak.
    consecutiveUnplayableSkips = 0;
  }

  auto res = advanceToNextTrackLocked(trigger);
  if (!res) {
    BELL_LOG(error, LOG_TAG, "Failed to advance after track {}: {}",
             trigger == AdvanceTrigger::TrackUnplayable ? "became unplayable"
                                                         : "ended",
             res.error());
  }
}

bell::Result<> ConnectStateHandler::handleSkipPrevCommandLocked() {
  auto res = trackQueueHandler->skipToPreviousTrack();
  if (!res) {
    BELL_LOG(error, LOG_TAG, "Failed to skip previous track");
    return res;
  }

  trackQueueHandler->updateTrackWindows();

  auto& playerState = putStateRequestProto.device.playerState;
  auto track = trackQueueHandler->currentTrack();
  // hasValue set explicitly - omitted (not sent empty) when there's no
  // current track. Copies the whole ProvidedTrack (uri, uid, provider) -
  // see the same assignment in handleTransferCommandLocked() for why.
  playerState.track.hasValue = static_cast<bool>(track);
  if (track) {
    playerState.track.value = *track;
  }

  // Track index within context - see handleTransferCommandLocked()'s comment.
  auto contextIndex = trackQueueHandler->currentContextIndex();
  playerState.index.hasValue = contextIndex.has_value();
  if (contextIndex) {
    playerState.index.value = *contextIndex;
  }

  // Re-announces isPlaying/isBuffering=true for the same reason as
  // advanceToNextTrackLocked(). isPaused is preserved (not forced false) -
  // matches go-librespot's skipPrev(), unlike its plain advanceNext().
  playerState.isPlaying = true;
  playerState.isBuffering = true;
  playerState.playbackSpeed =
      computePlaybackSpeed(playerState.isPaused, playerState.isBuffering);

  playerState.positionAsOfTimestamp = 0;
  playerState.timestamp =
      timeProvider->getSyncedTimestamp();

  (void)putStateLocked();

  return {};
}

int64_t ConnectStateHandler::currentPositionMsLocked(int64_t nowMs) const {
  auto& playerState = putStateRequestProto.device.playerState;
  return playerState.positionAsOfTimestamp +
         static_cast<int64_t>((nowMs - playerState.timestamp) *
                              playerState.playbackSpeed);
}

bell::Result<> ConnectStateHandler::handlePauseCommandLocked(bool pause) {
  // Not routed through StreamPlayer's own announceState() flow - that's
  // a no-op once the decoder is already open (the common pause/resume
  // case), so this needs its own explicit putState() call.
  eventLoop->post(EventLoop::EventType::PLAYER_PLAY, !pause);

  auto& playerState = putStateRequestProto.device.playerState;

  // Uses the OLD playbackSpeed/timestamp (before they're reassigned below).
  auto nowMs = timeProvider->getSyncedTimestamp();
  playerState.positionAsOfTimestamp = currentPositionMsLocked(nowMs);

  playerState.isPlaying = true;
  playerState.isPaused = pause;
  playerState.playbackSpeed =
      computePlaybackSpeed(playerState.isPaused, playerState.isBuffering);
  playerState.timestamp = nowMs;

  eventLoop->post(EventLoop::EventType::LOCAL_PLAY_PAUSE_CHANGED, pause);

  auto putRes = putStateLocked();
  if (!putRes) {
    BELL_LOG(error, LOG_TAG, "Failed to put state after pause/resume");
    return putRes;
  }

  return {};
}

bell::Result<> ConnectStateHandler::handleSeekCommandLocked(
    const tao::json::value& command) {
  auto& playerState = putStateRequestProto.device.playerState;

  if (!playerState.track.hasValue) {
    BELL_LOG(error, LOG_TAG, "seek_to with no current track");
    return bell::make_unexpected_errc(std::errc::invalid_argument);
  }

  // Same extrapolation handlePauseCommandLocked() uses - the OLD
  // playbackSpeed/timestamp, before applySeekLocked() reassigns them.
  auto nowMs = timeProvider->getSyncedTimestamp();
  int64_t currentPosition = currentPositionMsLocked(nowMs);

  auto relative = command.optional<std::string>("relative").value_or("");
  int64_t targetPosition;
  if (relative == "current") {
    targetPosition =
        currentPosition + command.optional<int64_t>("position").value_or(0);
  } else if (relative == "beginning") {
    targetPosition = command.optional<int64_t>("position").value_or(0);
  } else if (relative.empty()) {
    auto value = command.optional<double>("value");
    if (!value) {
      BELL_LOG(error, LOG_TAG, "seek_to missing value");
      return bell::make_unexpected_errc(std::errc::bad_message);
    }
    targetPosition = static_cast<int64_t>(*value);
  } else {
    BELL_LOG(error, LOG_TAG, "Unsupported seek_to relative: {}", relative);
    return bell::make_unexpected_errc(std::errc::invalid_argument);
  }

  return applySeekLocked(targetPosition);
}

bell::Result<> ConnectStateHandler::applySeekLocked(int64_t targetPositionMs) {
  auto& playerState = putStateRequestProto.device.playerState;

  auto nowMs = timeProvider->getSyncedTimestamp();

  targetPositionMs =
      std::clamp<int64_t>(targetPositionMs, 0, playerState.duration);

  eventLoop->post(EventLoop::EventType::PLAYER_SEEK, targetPositionMs);
  eventLoop->post(EventLoop::EventType::LOCAL_SEEKED, targetPositionMs);

  playerState.positionAsOfTimestamp = targetPositionMs;
  playerState.timestamp = nowMs;

  auto putRes = putStateLocked();
  if (!putRes) {
    BELL_LOG(error, LOG_TAG, "Failed to put state after seek");
    return putRes;
  }

  return {};
}

bell::Result<> ConnectStateHandler::handleUpdateContextCommandLocked(
    const tao::json::value& command) {
  const tao::json::value* contextItem = command.find("context");
  std::string incomingUri;
  if (contextItem) {
    auto uri = contextItem->optional<std::string>("uri");
    if (uri) {
      incomingUri = *uri;
    }
  }

  auto& playerState = putStateRequestProto.device.playerState;
  std::string currentUri = playerState.contextUri;
  if (incomingUri.empty() || incomingUri != currentUri) {
    // Acks anyway (returns success) - a uri mismatch just means this
    // update doesn't apply to us, not a failure. See this method's own
    // declaration comment for why never NAKing this command matters.
    BELL_LOG(info, LOG_TAG,
             "update_context: ignoring context update for wrong uri: {}",
             incomingUri);
    return {};
  }

  // Otherwise just an acknowledgment PUT - positionAsOfTimestamp *and*
  // timestamp deliberately left as-is. Touching timestamp alone breaks
  // the position-extrapolation pair (positionAsOfTimestamp + (now -
  // timestamp) * playbackSpeed), reading as a jump back to a frozen
  // position.
  return putStateLocked();
}

void ConnectStateHandler::applyPlayerOptionsLocked(
    std::optional<bool> repeatingContext, std::optional<bool> repeatingTrack,
    std::optional<bool> shufflingContext) {
  auto& options = putStateRequestProto.device.playerState.options;
  if (repeatingContext) {
    options.repeatingContext = *repeatingContext;
  }
  if (repeatingTrack) {
    options.repeatingTrack = *repeatingTrack;
  }
  if (shufflingContext) {
    options.shufflingContext = *shufflingContext;
  }
}

bell::Result<> ConnectStateHandler::applyRepeatContextLocked(
    std::optional<bool> repeatingContext) {
  applyPlayerOptionsLocked(repeatingContext, std::nullopt, std::nullopt);
  return putStateLocked();
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
