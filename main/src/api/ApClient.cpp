#include "api/ApClient.h"
#include <cstdint>
#include "Utils.h"
#include "bell/Logger.h"
#include "bell/Result.h"
#include "events/EventModels.h"
#include "proto/SpotifyId.h"

using namespace cspot;

namespace {
// Enumeration of AP command types
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
// an otherwise idle connection - same signal master's MercurySession uses
// for its own triggerTimeout()/PING_TIMEOUT_MS watchdog (125s there).
// Kept identical here: generous enough not to false-positive on a slightly
// late ping, tight enough to notice a silently-dead link well before a
// user would give up waiting for playback to recover.
const auto pingTimeout = std::chrono::seconds(125);
}  // namespace

ApClient::ApClient(std::shared_ptr<cspot::EventLoop> eventLoop,
                   std::shared_ptr<cspot::AuthInfo> authInfo)
    : eventLoop(std::move(eventLoop)), authInfo(std::move(authInfo)) {
  apConnection = std::make_unique<ApConnection>(this->authInfo);
  // Assign the packet handler for AP packets
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
  // never got (or never will get) a chance to respond to these, and the
  // sequence IDs aren't reused, so nothing will ever claim them.
  audioKeyRequests.clear();

  // Reset for this attempt - see loginWasDeclined()'s own comment.
  loginDeclined = false;

  // Full grace period before doHousekeeping() can decide the link is
  // dead, same as DealerClient::connect() does for its own pong watchdog -
  // otherwise a default-constructed/stale lastPingTime would look
  // "expired" immediately, before the AP has had any chance to send one.
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

  // Remember the track ID for the audio key request
  audioKeyRequests.insert(
      {audioKeySequence, {trackId, fileId}});  // Store the request

  // Structure: [FILEID] [TRACKID] [4 BYTES SEQUENCE ID] [0x00, 0x00]
  std::vector<std::byte> requestData = fileId;

  // Track ID
  requestData.insert(requestData.end(), trackId.gid.begin(), trackId.gid.end());

  // Sequence ID - must match the value used as the audioKeyRequests key
  // above exactly, since the AP just echoes it back and that's how the
  // response gets matched to a track. This used to read audioKeySequence
  // before it was ever incremented (correct) - keep it that way; the
  // increment below must stay AFTER this read, not before it.
  uint32_t sequence = htonl(audioKeySequence);
  requestData.insert(
      requestData.end(), reinterpret_cast<std::byte*>(&sequence),
      reinterpret_cast<std::byte*>(&sequence) + sizeof(sequence));

  // Append the 0x00, 0x00 bytes
  requestData.push_back(std::byte{0x00});
  requestData.push_back(std::byte{0x00});

  // Must be unique per in-flight request - audioKeyRequests is keyed by
  // this value, and the AP just echoes back whatever we send. Never
  // incrementing it at all meant every request (e.g. the 3-4 lookahead
  // tracks FileProvider fetches per queue update) went out as sequence 0:
  // insert() above no-ops for every request after the first (the key
  // already exists), so only one track's mapping ever survived - fixed
  // by incrementing here. A first attempt at this fix incremented before
  // building requestData above, which sent the AP the POST-increment
  // value while the map still held the PRE-increment key - reproduced on
  // real hardware as "unknown sequence ID: 1", "2", "3", ... for every
  // single request, an even worse regression than the original bug.
  // Incrementing here, after requestData already captured the correct
  // pre-increment value, keeps the map key and the wire value identical.
  audioKeySequence++;

  // Send the audio key request packet
  return apConnection->sendPacket(
      static_cast<uint8_t>(ApCommandType::AudioKeyRequest), requestData.data(),
      requestData.size());
}

void ApClient::apPacketHandler(uint8_t packetType, const std::byte* data,
                               size_t len) {
  switch (static_cast<ApCommandType>(packetType)) {
    case ApCommandType::Ping: {
      // Handle ping packet
      BELL_LOG(info, LOG_TAG, "Received ping request from AP");
      lastPingTime = std::chrono::steady_clock::now();

      auto res = apConnection->sendPacket(
          static_cast<uint8_t>(ApCommandType::Pong), data, len);
      if (!res) {
        BELL_LOG(error, LOG_TAG, "Failed to send pong response: {}",
                 res.error());
      }
      break;
    }
    case ApCommandType::PongAck: {
      // Handle pong ack packet
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

      // Handle the audio key response
      BELL_LOG(info, LOG_TAG,
               "Received audio key for track ID: {}, success: {}",
               ids.first.hexGid(), success);

      AudioKeyResponse response = {
          .success = success,
          .trackId = ids.first,
          .fileId = ids.second,
          .audioKey = std::vector<std::byte>(data + 4, data + len),
      };

      // Post the audio key response event
      this->eventLoop->post(EventLoop::EventType::AUDIO_KEY, response);
      BELL_LOG(info, LOG_TAG, "Audio key response posted for track ID: {}",
               ids.first.hexGid());
      break;
    }
    case ApCommandType::APWelcome: {
      // Handle AP welcome packet
      BELL_LOG(info, LOG_TAG, "Received AP welcome packet");
      break;
    }
    case ApCommandType::LoginDeclined: {
      // The AP explicitly rejected our credentials (revoked/expired
      // persisted blob, most commonly) - previously fell into default:
      // and was silently logged as an "unknown packet type", leaving
      // ApConnection's own state at CONNECTED_SHANNON (reached before
      // this response, see ApConnection::connect()'s own SENT_HELLO
      // branch) forever: state() kept reporting Connected even though no
      // real session was ever established. loginDeclined lets callers
      // (Session/main.cpp) tell this apart from a transient/network
      // Failed - see loginWasDeclined()'s own comment. disconnect() forces
      // ApConnection into its own ERROR state so state() also correctly
      // stops claiming Connected.
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
