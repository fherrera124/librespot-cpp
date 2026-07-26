#include "api/SpClient.h"

#include <iomanip>
#include <memory>
#include <sstream>

// Library includes
#include "Utils.h"
#include "bell/http/Client.h"
#include "tao/json.hpp"

#include "api/CredentialsResolver.h"
#include "proto/ExtendedMetadataPb.h"
#include "proto/MetadataPb.h"

using namespace cspot;

namespace {
class DefaultSpClient : public SpClient {
 public:
  DefaultSpClient(std::shared_ptr<bell::HTTPClient> httpClient,
                  std::shared_ptr<CredentialsResolver> credentialsResolver)
      : httpClient(std::move(httpClient)),
        credentialsResolver(std::move(credentialsResolver)) {}

  bell::Result<> putConnectState(cspot_proto::PutStateRequest& stateRequest,
                                 const std::string& deviceId,
                                 const std::string& sessionId) override;
  bell::Result<> putInactive(const std::string& deviceId,
                             const std::string& sessionId) override;
  bell::Result<bell::HTTPResponse> contextResolve(
      const std::string& contextUri) override;
  bell::Result<bell::HTTPResponse> contextAutoplayResolve(
      cspot_proto::AutoplayContextRequest& request) override;
  bell::Result<bell::HTTPResponse> rawRequest(const std::string& uri) override;

  bell::Result<cspot_proto::Track> trackMetadata(
      const SpotifyId& trackId) override;

  bell::Result<cspot_proto::Episode> episodeMetadata(
      const SpotifyId& episodeId) override;

  bell::Result<std::string> resolveStorageInteractive(
      const std::vector<std::byte>& fileId, bool prefetch = false) override;

  bell::Result<std::vector<cspot_proto::AudioFile>> resolveAudioFiles(
      const std::string& entityUri) override;

 private:
  const char* LOG_TAG = "SpClient";

  std::shared_ptr<bell::HTTPClient> httpClient;
  std::shared_ptr<CredentialsResolver> credentialsResolver;

  // Cached credentials from credentials resolver
  std::string spClientAddress;
  std::string accessToken;
  std::string clientToken;

  // Fetches all required credentials from the credentials resolver
  bell::Result<> updateCredentials();

  // POSTs a single-entity, single-extension-kind BatchedEntityRequest and
  // returns the matching entry's raw inner (Any.value) bytes, ready to be
  // decoded into whatever message type the caller expects for that kind.
  bell::Result<std::vector<std::byte>> extendedMetadataRaw(
      const std::string& entityUri, ExtensionKind kind);
};

bell::Result<> DefaultSpClient::putConnectState(
    cspot_proto::PutStateRequest& stateRequest, const std::string& deviceId,
    const std::string& sessionId) {
  auto credentialsRes = updateCredentials();
  if (!credentialsRes) {
    // Could not fetch credentials
    return credentialsRes;
  }

  std::vector<std::byte> freshBuffer;
  auto encodeRes = nanopb_helper::encodeToVector(stateRequest, freshBuffer);

  if (!encodeRes) {
    BELL_LOG(error, LOG_TAG, "Error while encoding message");
    return bell::make_unexpected_errc(std::errc::bad_message);
  }

  // No ?product=/salt= query params and Client-Token now included - matches
  // this repo's own master branch (PutStateClient::put()) and
  // go-librespot's PutConnectState(), neither of which use those query
  // params; Client-Token was the one header every OTHER call in this file
  // already sends but this one didn't, matching master's reference PUT.
  // A device whose connect-state write returns 200 but whose cluster
  // never actually transfers "active" to it (server accepts the write,
  // never applies the transition) is consistent with the server
  // distrusting/deprioritizing a request missing this token.
  auto httpResponse = httpClient->put(
      fmt::format("https://{}/connect-state/v1/devices/{}", spClientAddress,
                  deviceId),
      {
          {
              "Content-Type",
              "application/x-protobuf",
          },
          {"Client-Token", clientToken},
          {"X-Spotify-Connection-Id", sessionId},
          {"Authorization", fmt::format("Bearer {}", accessToken)},
      },
      tcb::span(reinterpret_cast<std::byte*>(freshBuffer.data()),
                freshBuffer.size()));

  if (!httpResponse) {
    BELL_LOG(error, LOG_TAG, "Error while sending request: {}",
             httpResponse.error());
    return tl::make_unexpected(httpResponse.error());
  }

  // Drain the response body (on success, the full updated cluster state -
  // several KB) unconditionally, before even looking at the status code.
  // A pooled HTTP/1.1 connection is only safe to reuse once the current
  // response's body has been fully read, regardless of whether that
  // response was a 200 or an error - draining only the success case
  // (as this used to) left an error response's body (e.g. a 411/429) sitting
  // unread on the wire, so the next request to reuse this pooled connection
  // read that leftover error body instead of its own response headers.
  // Reproduced on real hardware: contextResolve() intermittently failed to
  // parse what should've been an HTTP response because it was actually
  // reading a stale error response body from an earlier, unrelated PUT.
  auto bodyRes = httpResponse->bytes();
  if (!bodyRes) {
    BELL_LOG(error, LOG_TAG, "Error while draining response body: {}",
             bodyRes.error());
    return tl::make_unexpected(bodyRes.error());
  }

  if (httpResponse->statusCode != 200) {
    BELL_LOG(error, LOG_TAG, "Error while sending request: {}",
             httpResponse->statusCode);
    return bell::make_unexpected_errc(std::errc::bad_message);
  }

  // Temp diagnostic: this response body IS the server's own updated cluster
  // state (per the comment above) - decoding it here gives ground truth,
  // right at the moment we succeed, on whether Spotify's backend actually
  // recorded us as the active device, instead of inferring it indirectly
  // from a later pubsub cluster update (which may never arrive addressed
  // to the device that just changed, or may lag behind).
  {
    cspot_proto::Cluster cluster;
    if (nanopb_helper::decodeFromVector(cluster, *bodyRes)) {
      BELL_LOG(info, LOG_TAG,
               "PUT response cluster: activeDeviceId={} (ours={})",
               cluster.activeDeviceId, deviceId);
    } else {
      BELL_LOG(warn, LOG_TAG,
               "Could not decode PUT response body as a Cluster ({} bytes)",
               bodyRes->size());
    }

    // Cluster.device (tag 4) is actually a map<string, DeviceInfo> in
    // Spotify's real schema (go-librespot's connect.proto) - every device
    // currently registered in the cluster, keyed by device id. Our own
    // generated Cluster struct still treats it as a single embedded
    // message (a schema gap, not yet fixed), so cluster.activeDeviceId
    // above can't tell us whether we're actually listed, only who's
    // active. Cheap substring search on the raw bytes as a stand-in until
    // proper map decoding is added: our own device id is a fixed hex
    // string, so if it's anywhere in this response, we're in that map.
    std::string_view rawBody(reinterpret_cast<const char*>(bodyRes->data()),
                             bodyRes->size());
    bool listedInResponse = rawBody.find(deviceId) != std::string_view::npos;
    BELL_LOG(info, LOG_TAG,
             "PUT response raw bytes contain our own device id: {} ({} "
             "bytes total)",
             listedInResponse, bodyRes->size());
  }

  return {};
}

bell::Result<> DefaultSpClient::putInactive(const std::string& deviceId,
                                            const std::string& sessionId) {
  auto credentialsRes = updateCredentials();
  if (!credentialsRes) {
    return credentialsRes;
  }

  // Separate endpoint from putConnectState() - not a PutStateRequest body,
  // matches go-librespot's PutConnectStateInactive and this repo's own
  // master branch's PutStateClient::putInactive. notify=false matches what
  // both references pass here.
  auto httpResponse = httpClient->put(
      fmt::format(
          "https://{}/connect-state/v1/devices/{}/inactive?notify=false",
          spClientAddress, deviceId),
      {
          {"X-Spotify-Connection-Id", sessionId},
          {"Authorization", fmt::format("Bearer {}", accessToken)},
      });

  if (!httpResponse) {
    BELL_LOG(error, LOG_TAG, "Error while sending inactive request: {}",
             httpResponse.error());
    return tl::make_unexpected(httpResponse.error());
  }

  // Drain unconditionally, before checking status - same pooled-connection
  // reuse hazard as putConnectState() above, and it applies on the error
  // path too, not just success.
  auto bodyRes = httpResponse->bytes();
  if (!bodyRes) {
    BELL_LOG(error, LOG_TAG, "Error while draining response body: {}",
             bodyRes.error());
    return tl::make_unexpected(bodyRes.error());
  }

  // This endpoint replies 204 No Content on success, not 200 (it's a
  // state-change acknowledgment with no body to return, unlike
  // putConnectState()'s 200 + full cluster state) - matches go-librespot's
  // own PutConnectStateInactive (spclient/spclient.go: "resp.StatusCode !=
  // 204"). Checking for 200 here treated every successful call as an
  // error.
  if (httpResponse->statusCode != 204) {
    BELL_LOG(error, LOG_TAG, "Error while sending inactive request: {}",
             httpResponse->statusCode);
    return bell::make_unexpected_errc(std::errc::bad_message);
  }

  return {};
}

bell::Result<> DefaultSpClient::updateCredentials() {
  auto spClientAddressRes = credentialsResolver->getApAddress(
      CredentialsResolver::AddressType::SpClient);
  if (!spClientAddressRes) {
    return tl::make_unexpected(spClientAddressRes.error());
  }

  spClientAddress = *spClientAddressRes;

  auto accessTokenRes = credentialsResolver->getAccessKey();
  if (!accessTokenRes) {
    return tl::make_unexpected(accessTokenRes.error());
  }

  accessToken = *accessTokenRes;

  auto clientTokenRes = credentialsResolver->getClientToken();
  if (!clientTokenRes) {
    return tl::make_unexpected(clientTokenRes.error());
  }

  clientToken = *clientTokenRes;

  return {};
}

bell::Result<bell::HTTPResponse> DefaultSpClient::contextResolve(
    const std::string& contextUri) {
  return rawRequest(fmt::format("context-resolve/v1/{}", contextUri));
}

bell::Result<bell::HTTPResponse> DefaultSpClient::contextAutoplayResolve(
    cspot_proto::AutoplayContextRequest& request) {
  auto credentialsRes = updateCredentials();
  if (!credentialsRes) {
    // Could not fetch credentials
    return tl::make_unexpected(credentialsRes.error());
  }

  std::vector<std::byte> encodedBytes{};
  bool encodeResult = nanopb_helper::encodeToVector(request, encodedBytes);
  if (!encodeResult) {
    BELL_LOG(error, LOG_TAG, "Error while encoding AutoplayContextRequest");
    return bell::make_unexpected_errc<bell::HTTPResponse>(
        std::errc::bad_message);
  }

  return httpClient->post(
      fmt::format("https://{}/context-resolve/v1/autoplay", spClientAddress),
      {
          {"Client-Token", clientToken},
          {"Authorization", fmt::format("Bearer {}", accessToken)},
      },
      tcb::span(reinterpret_cast<std::byte*>(encodedBytes.data()),
                encodedBytes.size()));
}

bell::Result<bell::HTTPResponse> DefaultSpClient::rawRequest(
    const std::string& requestUri) {
  auto credentialsRes = updateCredentials();
  if (!credentialsRes) {
    // Could not fetch credentials
    return tl::make_unexpected(credentialsRes.error());
  }
  return httpClient->get(
      fmt::format("https://{}/{}", spClientAddress, requestUri),
      {
          {"Client-Token", clientToken},
          {"Authorization", fmt::format("Bearer {}", accessToken)},
      });
}

bell::Result<std::vector<std::byte>> DefaultSpClient::extendedMetadataRaw(
    const std::string& entityUri, ExtensionKind kind) {
  auto credentialsRes = updateCredentials();
  if (!credentialsRes) {
    return tl::make_unexpected(credentialsRes.error());
  }

  cspot_proto::EntityRequest entityRequest;
  entityRequest.entityUri = entityUri;
  entityRequest.query.push_back({.extensionKind = kind});

  cspot_proto::BatchedEntityRequest request;
  request.entityRequest.push_back(entityRequest);

  std::vector<std::byte> requestBytes;
  if (!nanopb_helper::encodeToVector(request, requestBytes)) {
    BELL_LOG(error, LOG_TAG, "Failed to encode BatchedEntityRequest");
    return bell::make_unexpected_errc<std::vector<std::byte>>(
        std::errc::bad_message);
  }
  auto url = fmt::format("https://{}/extended-metadata/v0/extended-metadata",
                         spClientAddress);
  auto response = httpClient->post(
      url,
      {
          {"Content-Type", "application/x-protobuf"},
          {"Client-Token", clientToken},
          {"Authorization", fmt::format("Bearer {}", accessToken)},
      },
      tcb::span(requestBytes.data(), requestBytes.size()));

  if (!response) {
    return tl::make_unexpected(response.error());
  }

  // Drain unconditionally, before checking status - a pooled connection is
  // only safe to reuse once the body's been read, error responses
  // included (see putConnectState()'s comment for the real-hardware
  // failure this caused when skipped on the error path).
  auto resultBytes = response->bytes();
  if (!resultBytes) {
    return bell::make_unexpected_errc<std::vector<std::byte>>(
        std::errc::bad_message);
  }

  if (response->statusCode != 200) {
    BELL_LOG(error, LOG_TAG, "Extended metadata request failed: {}",
             response->statusCode);
    return bell::make_unexpected_errc<std::vector<std::byte>>(
        std::errc::bad_message);
  }
  cspot_proto::BatchedExtensionResponse batchResponse;
  if (!nanopb_helper::decodeFromVector(batchResponse, *resultBytes)) {
    BELL_LOG(error, LOG_TAG, "Failed to decode BatchedExtensionResponse");
    return bell::make_unexpected_errc<std::vector<std::byte>>(
        std::errc::bad_message);
  }

  for (auto& array : batchResponse.extendedMetadata) {
    if (array.extensionKind != kind) {
      continue;
    }
    for (auto& data : array.extensionData) {
      if (data.entityUri != entityUri) {
        continue;
      }
      if (data.header.hasValue && data.header.value.statusCode != 200) {
        BELL_LOG(error, LOG_TAG,
                 "Extended metadata entity {} returned status {}", entityUri,
                 data.header.value.statusCode);
        return bell::make_unexpected_errc<std::vector<std::byte>>(
            std::errc::bad_message);
      }
      if (!data.extensionData.hasValue) {
        return bell::make_unexpected_errc<std::vector<std::byte>>(
            std::errc::bad_message);
      }
      return data.extensionData.value.value;
    }
  }

  BELL_LOG(error, LOG_TAG,
           "No matching extended metadata entry for {} (kind {})", entityUri,
           static_cast<int>(kind));
  return bell::make_unexpected_errc<std::vector<std::byte>>(
      std::errc::bad_message);
}

bell::Result<cspot_proto::Track> DefaultSpClient::trackMetadata(
    const SpotifyId& trackId) {
  if (trackId.type != SpotifyIdType::Track) {
    BELL_LOG(error, LOG_TAG, "Invalid track ID type: expected Track, got {}",
             static_cast<int>(trackId.type));
    return bell::make_unexpected_errc<cspot_proto::Track>(
        std::errc::invalid_argument);
  }

  // Was a GET to /metadata/4/track/{gid} - confirmed by hand-decoding a
  // real response's raw wire bytes that this endpoint's modern schema
  // just doesn't carry restriction/file/alternative anymore (fields 11-13
  // absent entirely, not merely misparsed). Real, current clients
  // (go-librespot) fetch this same data via TRACK_V4 extended-metadata
  // instead - matching that exactly.
  auto rawBytes = extendedMetadataRaw(trackId.uri, ExtensionKind_TRACK_V4);
  if (!rawBytes) {
    return tl::make_unexpected(rawBytes.error());
  }

  cspot_proto::Track trackProto;
  if (!nanopb_helper::decodeFromVector(trackProto, *rawBytes)) {
    BELL_LOG(error, LOG_TAG, "Error while decoding track metadata");
    return bell::make_unexpected_errc<cspot_proto::Track>(
        std::errc::bad_message);
  }

  BELL_LOG(info, LOG_TAG,
           "Decoded track metadata: name={}, {} restrictions, "
           "{} alternatives",
           trackProto.name, trackProto.restrictions.size(),
           trackProto.alternativeTracks.size());

  return trackProto;
}

bell::Result<std::vector<cspot_proto::AudioFile>>
DefaultSpClient::resolveAudioFiles(const std::string& entityUri) {
  auto rawBytes = extendedMetadataRaw(entityUri, ExtensionKind_AUDIO_FILES);
  if (!rawBytes) {
    return tl::make_unexpected(rawBytes.error());
  }

  cspot_proto::AudioFilesExtensionResponse audioFilesResponse;
  if (!nanopb_helper::decodeFromVector(audioFilesResponse, *rawBytes)) {
    BELL_LOG(error, LOG_TAG, "Error while decoding audio files metadata");
    return bell::make_unexpected_errc<std::vector<cspot_proto::AudioFile>>(
        std::errc::bad_message);
  }

  std::vector<cspot_proto::AudioFile> files;
  for (auto& extendedFile : audioFilesResponse.files) {
    if (extendedFile.file.hasValue) {
      files.push_back(extendedFile.file.value);
    }
  }

  BELL_LOG(info, LOG_TAG, "Resolved {} audio files for {}", files.size(),
           entityUri);

  return files;
}

bell::Result<cspot_proto::Episode> DefaultSpClient::episodeMetadata(
    const SpotifyId& episodeId) {
  if (episodeId.type != SpotifyIdType::Episode) {
    BELL_LOG(error, LOG_TAG, "Invalid track ID type: expected Episode, got {}",
             static_cast<int>(episodeId.type));
    return bell::make_unexpected_errc<cspot_proto::Episode>(
        std::errc::invalid_argument);
  }

  // Didn't fetch its own credentials - relied on spClientAddress/tokens
  // already being populated as a side effect of some earlier call (e.g.
  // contextResolve). Same latent-ordering bug class as trackMetadata's
  // (fixed above); matching the explicit updateCredentials() pattern
  // every other method here already uses.
  auto credentialsRes = updateCredentials();
  if (!credentialsRes) {
    return tl::make_unexpected(credentialsRes.error());
  }

  auto response = httpClient->get(
      fmt::format("https://{}/metadata/4/episode/{}", spClientAddress,
                  episodeId.hexGid()),
      {
          {"Client-Token", clientToken},
          {"Authorization", fmt::format("Bearer {}", accessToken)},
      });

  if (!response) {
    return tl::make_unexpected(response.error());
  }

  // Drain unconditionally, before checking status - same pooled-connection
  // reuse hazard as putConnectState() above.
  auto resultBytes = response->bytes();
  if (!resultBytes) {
    return bell::make_unexpected_errc<cspot_proto::Episode>(
        std::errc::bad_message);
  }

  if (response->statusCode != 200) {
    BELL_LOG(error, LOG_TAG, "Error while fetching episode metadata: {}",
             response->statusCode);
    return bell::make_unexpected_errc<cspot_proto::Episode>(
        std::errc::bad_message);
  }

  cspot_proto::Episode episodeProto;

  bool decodeRes = nanopb_helper::decodeFromVector(episodeProto, *resultBytes);

  if (!decodeRes) {
    BELL_LOG(error, LOG_TAG, "Error while decoding episode metadata");
    return bell::make_unexpected_errc<cspot_proto::Episode>(
        std::errc::bad_message);
  }

  return episodeProto;
}

bell::Result<std::string> DefaultSpClient::resolveStorageInteractive(
    const std::vector<std::byte>& fileId, bool prefetch) {
  auto credentialsRes = updateCredentials();
  if (!credentialsRes) {
    // Could not fetch credentials
    return tl::make_unexpected(credentialsRes.error());
  }

  std::stringstream ss;
  ss << std::hex << std::setfill('0');  // Set hex output and pad with '0'

  for (const auto& byte : fileId) {
    ss << std::setw(2)
       << static_cast<unsigned>(byte);  // Convert byte to int for stream output
  }

  // Construct the endpoint URL depending on prefetch flag
  std::string endpoint =
      prefetch
          ? fmt::format(
                "https://{}/storage-resolve/files/audio/interactive_prefetch/"
                "{}?alt=json&product=9",
                spClientAddress, ss.str())
          : fmt::format(
                "https://{}/storage-resolve/files/audio/interactive/"
                "{}?alt=json&product=9",
                spClientAddress, ss.str());

  auto response = httpClient->get(
      endpoint, {
                    {"Client-Token", clientToken},
                    {"Authorization", fmt::format("Bearer {}", accessToken)},
                });

  if (!response) {
    BELL_LOG(error, LOG_TAG, "Error while sending request: {}",
             response.error());
    return tl::make_unexpected(response.error());
  }

  auto textRes = response->text();
  if (!textRes) {
    BELL_LOG(error, LOG_TAG, "Error while reading response body: {}",
             textRes.error());
    return tl::make_unexpected(textRes.error());
  }
  auto responseBody = *textRes;

  // BELL_LOG(debug, LOG_TAG, "Response body: {}", responseBody);
  tao::json::value obj = tao::json::from_string(responseBody);

  if (obj.at("cdnurl").is_array()) {
    return obj.at("cdnurl").get_array().at(0).get_string();
  }

  return bell::make_unexpected_errc<std::string>(std::errc::bad_message);
}
}  // namespace

std::unique_ptr<SpClient> cspot::createDefaultSpClient(
    std::shared_ptr<bell::HTTPClient> httpClient,
    std::shared_ptr<CredentialsResolver> credentialsResolver) {
  return std::make_unique<DefaultSpClient>(std::move(httpClient),
                                           std::move(credentialsResolver));
}
