#include "ConnectStateHandler.h"

#include <algorithm>
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
    std::shared_ptr<AudioSink> audioSink,
    PlaybackNotificationCallback playbackNotificationCallback)
    // Stack sized for this task's own network work (the connect-state PUT
    // round-trip), same as every other network-doing task in this
    // codebase.
    : bell::Task("cspot_connect_state", 32 * 1024),
      eventLoop(std::move(eventLoop)),
      authInfo(std::move(authInfo)),
      spClient(std::move(spClient)),
      audioSink(std::move(audioSink)),
      playbackNotificationCallback(std::move(playbackNotificationCallback)) {
  trackQueueHandler =
      createDefaultTrackQueueHandler(this->spClient, this->eventLoop);

  // StreamPlayer ran out of audio (natural end of track) - forceNext=false
  // (unlike an explicit remote skip_next) so repeat-track is honored here.
  this->eventLoop->registerHandler(
      EventLoop::EventType::TRACK_ENDED, [this](cspot::EventLoop::Event&&) {
        handleTrackAdvanceSignal(/*forceNext=*/false);
      });

  // StreamPlayer could never load the track (CDN/audio key failure) -
  // forceNext=true so it's skipped like an explicit skip_next, regardless
  // of repeat-track (there's no audio to repeat).
  this->eventLoop->registerHandler(
      EventLoop::EventType::TRACK_UNPLAYABLE,
      [this](cspot::EventLoop::Event&&) {
        handleTrackAdvanceSignal(/*forceNext=*/true);
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
    return handleSkipNextCommandLocked();
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
  return bool(handleSkipNextCommandLocked());
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
  auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
                   .count();
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
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();
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
    BELL_LOG(info, LOG_TAG,
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
  return nanopb_helper::encodeToVector(putStateRequestProto, outBody);
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

  // Someone else just became the active device while we thought we were -
  // back off. lastTransferTimestamp (matches go-librespot's own
  // daemon/player.go) guards against a stale/reordered ClusterUpdate
  // deactivating us right after a legitimate transfer to this device.
  bool contradictsOurActiveState =
      putStateRequestProto.isActive &&
      clusterUpdate.cluster.activeDeviceId != authInfo->deviceId;
  bool stopBeingActive =
      contradictsOurActiveState &&
      clusterUpdate.cluster.playerState.timestamp > lastTransferTimestamp;

  if (contradictsOurActiveState && !stopBeingActive) {
    BELL_LOG(warn, LOG_TAG,
             "Ignoring cluster update showing activeDeviceId={} while we "
             "think we're active - older than our last transfer (ts={} <= "
             "lastTransferTimestamp={})",
             clusterUpdate.cluster.activeDeviceId,
             clusterUpdate.cluster.playerState.timestamp, lastTransferTimestamp);
  }

  if (!stopBeingActive) {
    return {};
  }

  BELL_LOG(info, LOG_TAG, "Playback was transferred to device {}",
           clusterUpdate.cluster.activeDeviceId);

  std::scoped_lock lock(putStateMutex);
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

  std::scoped_lock lock(putStateMutex);

  uint16_t volume = static_cast<uint16_t>(
      std::clamp<int32_t>(setVolumeCommand.volume, 0, 65535));

  BELL_LOG(info, LOG_TAG, "Set volume to {}", volume);
  putStateRequestProto.device.deviceInfo.volume = volume;

  audioSink->volumeChanged(volume);

  auto putRes = putStateLocked(PutStateReason_VOLUME_CHANGED);
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
  playerState.timestamp =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();
  // Keeps the source's own timestamp (not now()) - handleClusterUpdate()
  // compares it against incoming cluster updates, a different,
  // server-side clock domain than "when did we send our PUT".
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
  playerState.positionAsOfTimestamp =
      transferState.playback.positionAsOfTimestamp;
  putStateRequestProto.startedPlayingAt =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();
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
    // caller - see this function's own declaration comment) rather than
    // released around it: the only other lock-taker is this class's own
    // runTask(), which only holds the lock briefly to check/encode
    // (released during both its wait and its own network send), so the
    // worst case is a deferred flush, never a deadlock - and it avoids
    // ever flushing a half-updated transfer.
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
  // current track. .uri only, matching master (its only track setter
  // takes a bare uri string) - TrackQueueHandler's own ProvidedTrack
  // carries more, not copied here.
  playerState.track.hasValue = static_cast<bool>(track);
  if (track) {
    playerState.track.value = cspot_proto::ProvidedTrack{.uri = track->uri};
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

  eventLoop->post(EventLoop::EventType::PLAYER_FLUSH, std::monostate{});
  eventLoop->post(EventLoop::EventType::PLAYER_PLAY, !shouldPause);

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
  // current track.
  playerState.track.hasValue = static_cast<bool>(track);
  if (track) {
    playerState.track.value = cspot_proto::ProvidedTrack{.uri = track->uri};
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
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();

  auto putRes = putStateLocked();
  if (!putRes) {
    BELL_LOG(error, LOG_TAG, "Failed to put state after play command");
    return putRes;
  }

  return {};
}

bell::Result<> ConnectStateHandler::handleSkipNextCommandLocked() {
  return advanceToNextTrackLocked(/*forceNext=*/true,
                                  /*streamPlayerCleared=*/false);
}

bell::Result<> ConnectStateHandler::advanceToNextTrackLocked(
    bool forceNext, bool streamPlayerCleared) {
  auto& playerState = putStateRequestProto.device.playerState;

  bool hasNextTrack = true;

  if (!forceNext && playerState.options.repeatingTrack) {
    // Natural end of track with repeat-track on: don't advance, replay
    // the same track.
  } else {
    auto res = trackQueueHandler->skipToNextTrack();
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
  // current track.
  playerState.track.hasValue = static_cast<bool>(track);
  if (track) {
    playerState.track.value = cspot_proto::ProvidedTrack{.uri = track->uri};
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
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();

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

void ConnectStateHandler::handleTrackAdvanceSignal(bool forceNext) {
  // Matches go-librespot's own maxConsecutiveUnplayableSkips (controls.go) -
  // guards against a context where every track is unplayable.
  constexpr int kMaxConsecutiveUnplayableSkips = 50;

  std::scoped_lock lock(putStateMutex);

  if (forceNext) {
    // Only TRACK_UNPLAYABLE routes through this method with forceNext=true
    // (skip_next goes straight through handleSkipNextCommandLocked()), so
    // this is unambiguously "one more unplayable track in a row" here.
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

  // Both TRACK_ENDED and TRACK_UNPLAYABLE reach here only after
  // StreamPlayer has already cleared its own currentTrackId (see
  // advanceToNextTrackLocked()'s own comment) - unlike skip_next, which
  // routes straight to advanceToNextTrackLocked() without going through
  // this method at all.
  auto res = advanceToNextTrackLocked(forceNext, /*streamPlayerCleared=*/true);
  if (!res) {
    BELL_LOG(error, LOG_TAG, "Failed to advance after track {}: {}",
             forceNext ? "became unplayable" : "ended", res.error());
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
  // current track.
  playerState.track.hasValue = static_cast<bool>(track);
  if (track) {
    playerState.track.value = cspot_proto::ProvidedTrack{.uri = track->uri};
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
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();

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
  auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
                   .count();
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
  auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
                   .count();
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

  auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
                   .count();

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

  // Otherwise just an acknowledgment PUT, matching master's own
  // currentPlaybackSnapshot()+updatePlayerState() here -
  // restrictions/context_metadata from this command aren't captured
  // (see this method's own declaration comment). positionAsOfTimestamp
  // deliberately left as-is - not this command's concern.
  playerState.timestamp =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();

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
