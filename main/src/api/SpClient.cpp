#include "api/SpClient.h"

#include <fmt/format.h>
#include <iostream>
#include <memory>
#include <tao/json.hpp>
#include "bell/Logger.h"
#include "bell/Result.h"
#include "bell/http/Client.h"
#include "bell/http/Common.h"
#include "tl/expected.hpp"

using namespace cspot;

SpClient::SpClient(std::shared_ptr<SessionContext> sessionContext)
    : sessionContext(std::move(sessionContext)) {}

bell::Result<> SpClient::putConnectStateInactive(int retryCount) {
  // PutStateRequest stateRequest = PutStateRequest_init_zero;
  // return putConnectState(stateRequest, retryCount);

  return {};
}

bell::Result<> SpClient::putConnectState(
    cspot_proto::PutStateRequest& stateRequest, int retryCount) {
  std::vector<uint8_t> freshBuffer;
  auto encodeRes = nanopb_helper::encodeToVector(stateRequest, freshBuffer);
  if (!encodeRes) {
    BELL_LOG(error, LOG_TAG, "Error while encoding message");
    return bell::make_unexpected_errc(std::errc::bad_message);
  }

  auto addrRes = sessionContext->credentialsResolver->getApAddress(
      CredentialsResolver::AddressType::SpClient);

  if (!addrRes) {
    return tl::make_unexpected(addrRes.error());
  }

  std::string spClientAddress = *addrRes;

  auto keyRes = sessionContext->credentialsResolver->getAccessKey();
  if (!keyRes) {
    return tl::make_unexpected(keyRes.error());
  }

  uint32_t salt = std::rand();
  auto httpResponse = bell::http::requestWithBodyPtr(
      bell::HTTPMethod::PUT,
      fmt::format("https://{}/connect-state/v1/devices/{}?product=0&salt={}",
                  spClientAddress, sessionContext->loginBlob->getDeviceId(),
                  salt),
      {
          {
              "Content-Type",
              "application/x-protobuf",
          },
          {"X-Spotify-Connection-Id", sessionContext->sessionId},
          {"Authorization", fmt::format("Bearer {}", *keyRes)},
      },
      reinterpret_cast<const std::byte*>(freshBuffer.data()),
      freshBuffer.size());

  if (!httpResponse) {
    BELL_LOG(error, LOG_TAG, "Error while sending request: {}",
             httpResponse.error());
    return tl::make_unexpected(httpResponse.error());
  }

  if (httpResponse->getStatusCode() && httpResponse->getStatusCode() != 200) {
    BELL_LOG(error, LOG_TAG, "Error while sending request: {}",
             *httpResponse->getStatusCode());
    return bell::make_unexpected_errc(std::errc::bad_message);
  }

  return {};
}

bell::Result<std::string> SpClient::resolveStorageInteractive(
    const std::vector<uint8_t>& fileId, bool prefetch) {
  auto addrRes = sessionContext->credentialsResolver->getApAddress(
      CredentialsResolver::AddressType::SpClient);

  if (!addrRes) {
    return addrRes;
  }

  std::string spClientAddress = *addrRes;

  auto keyRes = sessionContext->credentialsResolver->getAccessKey();
  if (!keyRes) {
    return keyRes;
  }
  auto accessToken = *keyRes;

  auto clientTokenRes = sessionContext->credentialsResolver->getClientToken();
  if (!clientTokenRes) {
    return clientTokenRes;
  }
  auto clientToken = *clientTokenRes;

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

  auto response = bell::http::request(
      bell::HTTPMethod::GET, endpoint,
      {
          {"Client-Token", clientToken},
          {"Authorization", fmt::format("Bearer {}", accessToken)},
      });

  if (!response) {
    BELL_LOG(error, LOG_TAG, "Error while sending request: {}",
             response.error());
    return tl::make_unexpected(response.error());
  }

  auto responseBody = *response->getBodyStringView();

  BELL_LOG(info, LOG_TAG, "Response body: {}", responseBody);
  tao::json::value obj = tao::json::from_string(responseBody);

  if (obj.at("cdnurl").is_array()) {
    return obj.at("cdnurl").get_array().at(0).get_string();
  }

  return bell::make_unexpected_errc<std::string>(std::errc::bad_message);
}

bell::Result<bell::HTTPReader> SpClient::contextResolve(
    const std::string& contextUri) {
  auto addrRes = sessionContext->credentialsResolver->getApAddress(
      CredentialsResolver::AddressType::SpClient);

  if (!addrRes) {
    return tl::make_unexpected(addrRes.error());
  }

  std::string spClientAddress = *addrRes;

  auto keyRes = sessionContext->credentialsResolver->getAccessKey();
  if (!keyRes) {
    return tl::make_unexpected(keyRes.error());
  }

  auto clientTokenRes = sessionContext->credentialsResolver->getClientToken();
  if (!clientTokenRes) {
    return tl::make_unexpected(clientTokenRes.error());
  }
  auto response = bell::http::request(
      bell::HTTPMethod::GET,
      fmt::format("https://{}/context-resolve/v1/{}", spClientAddress,
                  contextUri),
      {
          {"Client-Token", *clientTokenRes},
          {"Authorization", fmt::format("Bearer {}", *keyRes)},
      });

  if (!response) {
    BELL_LOG(error, LOG_TAG, "Error while sending request: {}",
             response.error());
    return tl::make_unexpected(response.error());
  }

  return response;
}

bell::Result<bell::HTTPReader> SpClient::contextAutoplayResolve(
    cspot_proto::AutoplayContextRequest& request) {
  auto addrRes = sessionContext->credentialsResolver->getApAddress(
      CredentialsResolver::AddressType::SpClient);

  if (!addrRes) {
    return tl::make_unexpected(addrRes.error());
  }

  auto keyRes = sessionContext->credentialsResolver->getAccessKey();
  if (!keyRes) {
    return tl::make_unexpected(keyRes.error());
  }
  auto accessToken = *keyRes;

  auto clientTokenRes = sessionContext->credentialsResolver->getClientToken();
  if (!clientTokenRes) {
    return tl::make_unexpected(clientTokenRes.error());
  }

  std::vector<uint8_t> encodedBytes{};
  bool encodeResult = nanopb_helper::encodeToVector(request, encodedBytes);
  if (!encodeResult) {
    BELL_LOG(error, LOG_TAG, "Error while encoding AutoplayContextRequest");
    return bell::make_unexpected_errc<bell::HTTPReader>(std::errc::bad_message);
  }

  auto response = bell::http::requestWithBodyPtr(
      bell::HTTPMethod::POST,
      fmt::format("https://{}/context-resolve/v1/autoplay", *addrRes),
      {
          {"Client-Token", *clientTokenRes},
          {"Authorization", fmt::format("Bearer {}", accessToken)},
      },
      reinterpret_cast<std::byte*>(encodedBytes.data()), encodedBytes.size());

  if (!response) {
    BELL_LOG(error, LOG_TAG, "Error while sending request: {}",
             response.error());
    return tl::make_unexpected(response.error());
  }

  return response;
}

bell::Result<bell::HTTPReader> SpClient::doRequest(
    bell::HTTPMethod method, const std::string& requestUrl) {

  auto addrRes = sessionContext->credentialsResolver->getApAddress(
      CredentialsResolver::AddressType::SpClient);

  if (!addrRes) {
    return tl::make_unexpected(addrRes.error());
  }

  auto keyRes = sessionContext->credentialsResolver->getAccessKey();
  if (!keyRes) {
    return tl::make_unexpected(keyRes.error());
  }

  auto clientTokenRes = sessionContext->credentialsResolver->getClientToken();
  if (!clientTokenRes) {
    return tl::make_unexpected(clientTokenRes.error());
  }
  auto clientToken = *clientTokenRes;

  auto response = bell::http::request(
      method, fmt::format("https://{}/{}", *addrRes, requestUrl),
      {
          {"Client-Token", clientToken},
          {"Authorization", fmt::format("Bearer {}", *keyRes)},
      });

  if (!response) {
    BELL_LOG(error, LOG_TAG, "Error while sending request: {}",
             response.error());
    return tl::make_unexpected(response.error());
  }

  return response;
}

// bell::Result<tao::json::value> SpClient::radioApollo(
//     const std::string& scope, const std::string& contextUri, bool autoplay,
//     int pageSize) {
//   return hmRequest(
//       fmt::format("hm://radio-apollo/v3/{}/{}/?autoplay={}&count={}", scope,
//                   contextUri, autoplay, pageSize));
// }

bell::Result<cspot_proto::Track> SpClient::trackMetadata(
    const SpotifyId& trackId) {
  if (trackId.type != SpotifyIdType::Track) {
    BELL_LOG(error, LOG_TAG, "Invalid track ID type: expected Track, got {}",
             static_cast<int>(trackId.type));
    return bell::make_unexpected_errc<cspot_proto::Track>(
        std::errc::invalid_argument);
  }

  auto reader = doRequest(bell::http::Method::GET,
                       fmt::format("metadata/4/track/{}", trackId.hexGid()));
  if (!reader) {
    return tl::make_unexpected(reader.error());
  }

  if (reader->getStatusCode() != 200) {
    BELL_LOG(error, LOG_TAG, "Error while fetching track metadata: {}",
             *reader->getStatusCode());
    return bell::make_unexpected_errc<cspot_proto::Track>(
        std::errc::bad_message);
  }

  auto resultBytes = reader->getBodyBytes();

  cspot_proto::Track trackProto;

  bool decodeRes = nanopb_helper::decodeFromBuffer(
      trackProto,
      reinterpret_cast<const uint8_t*>(*reader->getBodyBytesPtr()),
      *reader->getBodyBytesLength());

  if (!decodeRes) {
    BELL_LOG(error, LOG_TAG, "Error while decoding track metadata");
    return bell::make_unexpected_errc<cspot_proto::Track>(
        std::errc::bad_message);
  }

  return trackProto;
}
