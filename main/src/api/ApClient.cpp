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
}  // namespace

ApClient::ApClient(std::shared_ptr<SessionContext> sessionContext)
    : sessionContext(std::move(sessionContext)) {
  apConnection = std::make_unique<ApConnection>(this->sessionContext);

  // Assign the packet handler for AP packets
  apConnection->setPacketHandler(
      [this](uint8_t packetType, const uint8_t* data, size_t len) {
        this->apPacketHandler(packetType, data, len);
      });
}

bell::Result<> ApClient::connectAndAuthenticate() {
  if (!sessionContext->loginBlob->isAuthenticated()) {
    BELL_LOG(error, LOG_TAG, "Login blob is not authenticated");
    return bell::make_unexpected_errc(std::errc::not_connected);
  }

  return apConnection->connect();
}

bell::Result<> ApClient::requestAudioKey(const SpotifyId& trackId,
                                         const std::vector<uint8_t>& fileId) {
  if (!apConnection->getSocket()) {
    BELL_LOG(error, LOG_TAG, "AP connection is not established");
    return bell::make_unexpected_errc(std::errc::not_connected);
  }

  // Remember the track ID for the audio key request
  audioKeyRequests.insert(
      {audioKeySequence, {trackId, fileId}});  // Store the request

  // Structure: [FILEID] [TRACKID] [4 BYTES SEQUENCE ID] [0x00, 0x00]
  std::vector<uint8_t> requestData = fileId;

  // Track ID
  requestData.insert(requestData.end(), trackId.gid.begin(), trackId.gid.end());

  // Sequence ID
  uint32_t sequence = htonl(audioKeySequence);
  requestData.insert(requestData.end(), reinterpret_cast<uint8_t*>(&sequence),
                     reinterpret_cast<uint8_t*>(&sequence) + sizeof(sequence));

  // Append the 0x00, 0x00 bytes
  requestData.push_back(0x00);
  requestData.push_back(0x00);

  // Send the audio key request packet
  return apConnection->sendPacket(
      static_cast<uint8_t>(ApCommandType::AudioKeyRequest), requestData.data(),
      requestData.size());
}

void ApClient::apPacketHandler(uint8_t packetType, const uint8_t* data,
                               size_t len) {
  switch (static_cast<ApCommandType>(packetType)) {
    case ApCommandType::Ping: {
      // Handle ping packet
      BELL_LOG(info, LOG_TAG, "Received ping request from AP");

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
      std::string_view countryCode(reinterpret_cast<const char*>(data), len);
      BELL_LOG(info, LOG_TAG, "Received country code: {}", countryCode);
      // TODO: Set country code somewhere
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
      logDataBase64(data + 4, len - 4);

      AudioKeyResponse response = {
          .success = success,
          .trackId = ids.first,
          .fileId = ids.second,
          .audioKey = std::vector<uint8_t>(data + 4, data + len),
      };

      // Post the audio key response event
      this->sessionContext->eventLoop->post(EventLoop::EventType::AUDIO_KEY,
                                            response);
      BELL_LOG(info, LOG_TAG, "Audio key response posted for track ID: {}",
               ids.first.hexGid());
      break;
    }
    case ApCommandType::APWelcome: {
      // Handle AP welcome packet
      BELL_LOG(info, LOG_TAG, "Received AP welcome packet");
      auto fileId = base64Decode("JZzyapQR+mJpRCsudORv10UQlJU=");
      auto spotifyId = SpotifyId("spotify:track:2kzzAJYhIoNyy1kxEpDrYN");
      auto res = requestAudioKey(spotifyId, fileId);
      if (!res) {
        BELL_LOG(error, LOG_TAG, "Failed to request audio key: {}",
                 res.error());
      } else {
        BELL_LOG(info, LOG_TAG, "Audio key request sent successfully");
      }

      break;
    }
    default:
      BELL_LOG(warn, LOG_TAG, "Received unknown packet type: {}", packetType);
      break;
  }
}
