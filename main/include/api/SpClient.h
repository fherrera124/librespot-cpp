#pragma once

// Standard includes
#include <string>
#include <vector>

// Library includes
#include <bell/Result.h>

// Protobufs
#include "bell/http/Reader.h"

#include "SessionContext.h"
#include "proto/ConnectPb.h"
#include "proto/MetadataPb.h"
#include "proto/SpotifyId.h"

namespace cspot {
class SpClient {
 public:
  SpClient(std::shared_ptr<SessionContext> sessionContext);

  bell::Result<> putConnectStateInactive(int retryCount = 3);
  bell::Result<> putConnectState(cspot_proto::PutStateRequest& stateRequest,
                                 int retryCount = 3);
  bell::Result<bell::HTTPReader> contextResolve(const std::string& contextUri);

  bell::Result<bell::HTTPReader> contextAutoplayResolve(
      cspot_proto::AutoplayContextRequest& request);

  bell::Result<bell::HTTPReader> doRequest(bell::HTTPMethod method,
                                           const std::string& requestUrl);

  bell::Result<cspot_proto::Track> trackMetadata(const SpotifyId& trackId);

  bell::Result<cspot_proto::Episode> episodeMetadata(
      const SpotifyId& episodeId);

  bell::Result<std::string> resolveStorageInteractive(
      const std::vector<uint8_t>& fileId, bool prefetch = false);

 private:
  const char* LOG_TAG = "SpClient";

  std::shared_ptr<SessionContext> sessionContext;
  std::vector<std::uint8_t> requestBuffer;
};
}  // namespace cspot
