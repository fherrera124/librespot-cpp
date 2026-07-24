#pragma once

#include <unordered_map>
#include <utility>
#include "AuthInfo.h"
#include "api/ApConnection.h"
#include "events/EventLoop.h"
#include "proto/SpotifyId.h"

namespace cspot {
class ApClient {
 public:
  ApClient(std::shared_ptr<cspot::EventLoop> eventLoop,
           std::shared_ptr<cspot::AuthInfo> authInfo);

  bell::Result<> connectAndAuthenticate(
      const std::string& apAddress,
      const std::shared_ptr<bell::SocketPollListener>& socketPoll);

  bell::Result<> requestAudioKey(const SpotifyId& trackId,
                                 const std::vector<std::byte>& fileId);

  void doHousekeeping();

  // Empty until the AP sends its CountryCode packet, shortly after
  // connecting. Used to resolve region-restricted tracks to a playable
  // alternative (see FileProvider.cpp).
  const std::string& getCountryCode() const { return countryCode; }

 private:
  const char* LOG_TAG = "ApClient";

  std::shared_ptr<cspot::EventLoop> eventLoop;
  std::shared_ptr<cspot::AuthInfo> authInfo;
  std::unique_ptr<ApConnection> apConnection;

  std::string countryCode;

  uint32_t audioKeySequence = 0;

  // Holds a mapping of audio key requests to track IDs
  std::unordered_map<uint32_t, std::pair<SpotifyId, std::vector<std::byte>>>
      audioKeyRequests;

  void apPacketHandler(uint8_t packetType, const std::byte* data, size_t len);
};
};  // namespace cspot
