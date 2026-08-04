#include "api/ApClient.h"
#include <sys/time.h>
#include <cstdint>
#include <cstring>
#include "Utils.h"
#include "bell/Logger.h"
#include "bell/Result.h"
#include "events/EventModels.h"
#include "proto/SpotifyId.h"

using namespace cspot;

namespace {
enum class ApCommandType : std::uint8_t {
  Ping = 0x04,
  LoginSuccess = 0x4C,
  LoginDeclined = 0x4D,
  Pong = 0x49,
  CountryCode = 0x1B,
  AudioKeyRequest = 0x0C,
  AudioKeyResponseSuccess = 0x0D,
  AudioKeyResponseError = 0x0E,
  APWelcome = 0xAC,
  SecretBlock = 0x02,
  LicenseVersion = 0x76,
  ProductInfo = 0x50,
  MercuryEvent = 0x5b,
  PongAck = 0x4a,
};

// Real Spotify APs send a keep-alive Ping roughly every ~2 minutes even on
// an otherwise idle connection - matches master's own MercurySession
// watchdog value (125s).
const auto pingTimeout = std::chrono::seconds(125);
}  // namespace

ApClient::ApClient(std::shared_ptr<cspot::EventLoop> eventLoop,
                   std::shared_ptr<cspot::AuthInfo> authInfo)
    : eventLoop(std::move(eventLoop)), authInfo(std::move(authInfo)) {
  apConnection = std::make_unique<ApConnection>(this->authInfo);
  apConnection->setPacketHandler(
      [this](uint8_t packetType, const std::byte* data, size_t len) {
        this->apPacketHandler(packetType, data, len);
      });
}

bell::Result<> ApClient::connectAndAuthenticate(
    const std::string& apAddress,
    const std::shared_ptr<bell::SocketPollListener>& socketPoll) {
  if (!authInfo->loginCredentials) {
    BELL_LOG(error, LOG_TAG, "No login credentials available");
    return bell::make_unexpected_errc(std::errc::permission_denied);
  }

  // Orphaned by whatever connection attempt preceded this one - the AP
  // never got (or never will get) a chance to respond to these, and
  // sequence IDs aren't reused, so nothing will ever claim them. Failed
  // explicitly (not just dropped) so FileProvider's own
  // handleAudioKeyResponse() failure path runs for each one, same as a
  // real AudioKeyResponseError. Silently dropping these leaves
  // FileProvider waiting forever on a response that will never arrive.
  for (auto& [sequence, ids] : audioKeyRequests) {
    AudioKeyResponse response = {
        .success = false,
        .trackId = ids.first,
        .fileId = ids.second,
        .audioKey = {},
    };
    eventLoop->post(EventLoop::EventType::AUDIO_KEY, response);
  }
  audioKeyRequests.clear();

  // Reset for this attempt - see loginWasDeclined()'s own comment.
  loginDeclined = false;

  // Full grace period before doHousekeeping() can decide the link is
  // dead - otherwise a stale lastPingTime would look expired
  // immediately, before the AP has had any chance to send one.
  lastPingTime = std::chrono::steady_clock::now();

  return apConnection->connect(apAddress, socketPoll);
}

void ApClient::doHousekeeping() {
  if (!apConnection->isConnected()) {
    return;
  }

  if (std::chrono::steady_clock::now() - lastPingTime > pingTimeout) {
    BELL_LOG(error, LOG_TAG,
             "No ping received from AP in over {}s, treating connection as "
             "dead",
             std::chrono::duration_cast<std::chrono::seconds>(pingTimeout)
                 .count());
    apConnection->disconnect();
  }
}

std::optional<std::chrono::steady_clock::time_point>
ApClient::nextDeadline() const {
  if (!apConnection->isConnected()) {
    return std::nullopt;
  }
  return lastPingTime + pingTimeout;
}

ApClient::State ApClient::state() const {
  if (apConnection->isConnected()) {
    return State::Connected;
  }
  if (apConnection->hasFailed()) {
    return State::Failed;
  }
  return State::Connecting;
}

bell::Result<> ApClient::requestAudioKey(const SpotifyId& trackId,
                                         const std::vector<std::byte>& fileId) {
  if (!apConnection->getSocket()) {
    BELL_LOG(error, LOG_TAG, "AP connection is not established");
    return bell::make_unexpected_errc(std::errc::not_connected);
  }

  audioKeyRequests.insert({audioKeySequence, {trackId, fileId}});

  // Wire format: [FILEID] [TRACKID] [4 BYTES SEQUENCE ID] [0x00, 0x00]
  std::vector<std::byte> requestData = fileId;
  requestData.insert(requestData.end(), trackId.gid.begin(), trackId.gid.end());

  // Must read audioKeySequence before incrementing it - the map above is
  // keyed by the pre-increment value, and this must match exactly what
  // goes on the wire (the AP echoes it back to match the response).
  // Incrementing before building requestData sent the post-increment
  // value while the map still held the pre-increment key - reproduced on
  // real hardware as "unknown sequence ID" for every request.
  uint32_t sequence = htonl(audioKeySequence);
  requestData.insert(
      requestData.end(), reinterpret_cast<std::byte*>(&sequence),
      reinterpret_cast<std::byte*>(&sequence) + sizeof(sequence));
  requestData.push_back(std::byte{0x00});
  requestData.push_back(std::byte{0x00});
  audioKeySequence++;

  return apConnection->sendPacket(
      static_cast<uint8_t>(ApCommandType::AudioKeyRequest), requestData.data(),
      requestData.size());
}

void ApClient::apPacketHandler(uint8_t packetType, const std::byte* data,
                               size_t len) {
  switch (static_cast<ApCommandType>(packetType)) {
    case ApCommandType::Ping: {
      BELL_LOG(info, LOG_TAG, "Received ping request from AP");
      lastPingTime = std::chrono::steady_clock::now();

      // Spotify's own AP ping carries a real Unix timestamp (seconds,
      // big-endian) in its first 4 bytes, avoiding any NTP dependency.
      // Sets the system clock directly so every system_clock::now()
      // call already in this codebase is correct without touching any of
      // them. Recalibrated on every ping (~2min) to correct for clock
      // drift.
      if (len >= sizeof(uint32_t)) {
        uint32_t remoteSeconds;
        std::memcpy(&remoteSeconds, data, sizeof(remoteSeconds));
        remoteSeconds = ntohl(remoteSeconds);
        struct timeval tv = {.tv_sec = static_cast<time_t>(remoteSeconds),
                             .tv_usec = 0};
        settimeofday(&tv, nullptr);
      }

      auto res = apConnection->sendPacket(
          static_cast<uint8_t>(ApCommandType::Pong), data, len);
      if (!res) {
        BELL_LOG(error, LOG_TAG, "Failed to send pong response: {}",
                 res.error());
      }
      break;
    }
    case ApCommandType::PongAck: {
      BELL_LOG(info, LOG_TAG, "Received pong ack from AP");
      break;
    }
    case ApCommandType::CountryCode: {
      this->countryCode.assign(reinterpret_cast<const char*>(data), len);
      BELL_LOG(info, LOG_TAG, "Received country code: {}", this->countryCode);
      break;
    }
    case ApCommandType::AudioKeyResponseError:
    case ApCommandType::AudioKeyResponseSuccess: {
      if (len < 4) {
        BELL_LOG(error, LOG_TAG,
                 "Received audio key response with invalid length");
        return;
      }

      uint32_t sequence = ntohl(*reinterpret_cast<const uint32_t*>(data));
      auto it = audioKeyRequests.find(sequence);
      if (it == audioKeyRequests.end()) {
        BELL_LOG(error, LOG_TAG,
                 "Received audio key response for unknown sequence ID: {}",
                 sequence);
        return;
      }

      auto ids = it->second;
      audioKeyRequests.erase(it);

      bool success =
          (packetType ==
           static_cast<uint8_t>(ApCommandType::AudioKeyResponseSuccess));

      BELL_LOG(info, LOG_TAG,
               "Received audio key for track ID: {}, success: {}",
               ids.first.hexGid(), success);

      AudioKeyResponse response = {
          .success = success,
          .trackId = ids.first,
          .fileId = ids.second,
          .audioKey = std::vector<std::byte>(data + 4, data + len),
      };

      this->eventLoop->post(EventLoop::EventType::AUDIO_KEY, response);
      BELL_LOG(info, LOG_TAG, "Audio key response posted for track ID: {}",
               ids.first.hexGid());
      break;
    }
    case ApCommandType::APWelcome: {
      BELL_LOG(info, LOG_TAG, "Received AP welcome packet");
      break;
    }
    case ApCommandType::LoginDeclined: {
      // Distinct from a transient/network Failed: without this, an
      // explicit credential rejection fell into default: below and was
      // silently logged as "unknown packet type", leaving state()
      // claiming Connected forever (ApConnection was already at
      // CONNECTED_SHANNON before this response). loginDeclined lets
      // callers (Session/main.cpp) tell the two apart - see
      // loginWasDeclined()'s own comment. disconnect() forces
      // ApConnection into ERROR so state() stops claiming Connected too.
      BELL_LOG(error, LOG_TAG, "AP declined login - credentials rejected");
      loginDeclined = true;
      apConnection->disconnect();
      break;
    }
    default:
      BELL_LOG(warn, LOG_TAG, "Received unknown packet type: {}", packetType);
      break;
  }
}
