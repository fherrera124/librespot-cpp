#pragma once

#include <unordered_map>
#include <utility>
#include "SessionContext.h"
#include "api/ApConnection.h"
#include "proto/SpotifyId.h"

namespace cspot {
class ApClient {
 public:
  ApClient(std::shared_ptr<SessionContext> sessionContext);

  bell::Result<> connectAndAuthenticate();

  bell::Result<> requestAudioKey(const SpotifyId& trackId,
                                 const std::vector<uint8_t>& fileId);

  void doHousekeeping();

 private:
  const char* LOG_TAG = "ApClient";

  std::shared_ptr<SessionContext> sessionContext;
  std::unique_ptr<ApConnection> apConnection;

  uint32_t audioKeySequence = 0;

  // Holds a mapping of audio key requests to track IDs
  std::unordered_map<uint32_t, std::pair<SpotifyId, std::vector<uint8_t>>>
      audioKeyRequests;

  void apPacketHandler(uint8_t packetType, const uint8_t* data, size_t len);
};
};  // namespace cspot
