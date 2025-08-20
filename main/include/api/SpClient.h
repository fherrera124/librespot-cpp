#pragma once

// Standard includes
#include <string>
#include <vector>

// Library includes
#include "bell/Result.h"
#include "bell/http/Client.h"

#include "SessionContext.h"

// Protobufs
#include "proto/ConnectPb.h"
#include "proto/MetadataPb.h"
#include "proto/SpotifyId.h"

namespace cspot {
class SpClient {
 public:
  SpClient(std::shared_ptr<bell::HTTPClient> httpClient,
           std::shared_ptr<SessionContext> sessionContext);

  bell::Result<> putConnectState(cspot_proto::PutStateRequest& stateRequest);
  bell::Result<bell::HTTPResponse> contextResolve(
      const std::string& contextUri);

  bell::Result<bell::HTTPResponse> contextAutoplayResolve(
      cspot_proto::AutoplayContextRequest& request);

  bell::Result<cspot_proto::Track> trackMetadata(const SpotifyId& trackId);

  bell::Result<cspot_proto::Episode> episodeMetadata(
      const SpotifyId& episodeId);

  bell::Result<std::string> resolveStorageInteractive(
      const std::vector<std::byte>& fileId, bool prefetch = false);

 private:
  const char* LOG_TAG = "SpClient";

  std::shared_ptr<bell::HTTPClient> httpClient;
  std::shared_ptr<SessionContext> sessionContext;
  std::vector<std::byte> requestBuffer;

  // Credentials fetched from CredentialResolver
  std::string accessToken;
  std::string clientToken;
  std::string spClientAddress;

  // Updates credentials if expired
  bell::Result<> updateCredentials();
};
}  // namespace cspot
