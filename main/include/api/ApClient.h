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
                                 const std::vector<std::byte>& fileId);

  void doHousekeeping();

 private:
  const char* LOG_TAG = "ApClient";

  std::shared_ptr<SessionContext> sessionContext;
  std::unique_ptr<ApConnection> apConnection;

  uint32_t audioKeySequence = 0;

  // Holds a mapping of audio key requests to track IDs
  std::unordered_map<uint32_t, std::pair<SpotifyId, std::vector<std::byte>>>
      audioKeyRequests;

  void apPacketHandler(uint8_t packetType, const std::byte* data, size_t len);
};
};  // namespace cspot
