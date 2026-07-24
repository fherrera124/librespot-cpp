#pragma once

// Standard includes
#include <string>
#include <vector>

// Library includes
#include "bell/Result.h"
#include "bell/http/Client.h"

// Protobufs
#include "proto/ConnectPb.h"
#include "proto/ExtendedMetadataPb.h"
#include "proto/MetadataPb.h"
#include "proto/SpotifyId.h"

// Own includes
#include "api/CredentialsResolver.h"

namespace cspot {

class SpClient {
 public:
  virtual ~SpClient() = default;

  /**
   * @brief Makes an /connect-state/ PUT request, used to publish spotify device state
   */
  virtual bell::Result<> putConnectState(
      cspot_proto::PutStateRequest& stateRequest, const std::string& deviceId,
      const std::string& sessionId) = 0;

  virtual bell::Result<bell::HTTPResponse> contextResolve(
      const std::string& contextUri) = 0;

  virtual bell::Result<bell::HTTPResponse> contextAutoplayResolve(
      cspot_proto::AutoplayContextRequest& request) = 0;

  virtual bell::Result<bell::HTTPResponse> rawRequest(
      const std::string& uri) = 0;

  virtual bell::Result<cspot_proto::Track> trackMetadata(
      const SpotifyId& trackId) = 0;

  virtual bell::Result<cspot_proto::Episode> episodeMetadata(
      const SpotifyId& episodeId) = 0;

  virtual bell::Result<std::string> resolveStorageInteractive(
      const std::vector<std::byte>& fileId, bool prefetch = false) = 0;

  /**
   * @brief Fetches the AUDIO_FILES extended-metadata for an entity (track
   * or episode) URI - modern spclient no longer serves AudioFile entries
   * through trackMetadata()/episodeMetadata() at all, they live behind
   * this separate extended-metadata API instead (matches go-librespot's
   * own real, current behavior).
   */
  virtual bell::Result<std::vector<cspot_proto::AudioFile>> resolveAudioFiles(
      const std::string& entityUri) = 0;
};

std::unique_ptr<SpClient> createDefaultSpClient(
    std::shared_ptr<bell::HTTPClient> httpClient,
    std::shared_ptr<CredentialsResolver> credentialsResolver);
}  // namespace cspot
