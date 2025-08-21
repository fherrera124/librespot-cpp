#include "api/CredentialsResolver.h"

#include <mutex>

// Library includes
#include <tao/json.hpp>
#include <tao/json/contrib/traits.hpp>
#include "bell/Logger.h"
#include "bell/http/Client.h"

// Protobufs
#include "bell/Result.h"
#include "clienttoken.pb.h"
#include "proto/ClientTokenPb.h"
#include "proto/Login5Pb.h"
#include "proto/NanoPBHelper.h"

#include "AuthInfo.h"

using namespace cspot;

namespace {
// Endpoints
const std::string apResolveUrl =
    "https://apresolve.spotify.com/"
    "?type=spclient&type=dealer-g2&type=accesspoint";
const std::string clientTokenUrl =
    "https://clienttoken.spotify.com/v1/clienttoken";

// Spotify web client's client id
std::string spotifyClientId = "65b708073fc0480ea92a077233ca87bd";

// Required access scopes
const std::string accessTokenScopes =
    "app-remote-control,playlist-modify,playlist-modify-private,playlist-"
    "modify-public,playlist-read,playlist-read-collaborative,playlist-read-"
    "private,streaming,ugc-image-upload,user-follow-modify,user-follow-read,"
    "user-library-modify,user-library-read,user-modify,user-modify-playback-"
    "state,user-modify-private,user-personalized,user-read-birthdate,user-read-"
    "currently-playing,user-read-email,user-read-play-history,user-read-"
    "playback-position,user-read-playback-state,user-read-private,user-read-"
    "recently-player,user-top-read";

// JSON keys
const std::string accessPointKey = "accesspoint";
const std::string dealerKey = "dealer-g2";
const std::string spClientKey = "spclient";
}  // namespace

class DefaultCredentialsResolver : public CredentialsResolver {
 public:
  DefaultCredentialsResolver(std::shared_ptr<bell::HTTPClient> httpClient,
                             std::shared_ptr<AuthInfo> authInfo)
      : httpClient(std::move(httpClient)), authInfo(std::move(authInfo)) {
    // Set expiration time to now, will be updated on first call
    this->addressesExpiresAt =
        std::chrono::system_clock::now() - std::chrono::hours(1);
    this->clientTokenExpiresAt =
        std::chrono::system_clock::now() - std::chrono::hours(1);
    this->accessKeyExpiresAt =
        std::chrono::system_clock::now() - std::chrono::hours(1);
  }

  bell::Result<std::string> getApAddress(AddressType type,
                                         sysclock_timepoint now) override {
    std::scoped_lock lock(this->accessMutex);

    // Check if the address is expired
    if (now > addressesExpiresAt) {
      auto res = fetchApAdresses();
      if (!res) {
        return tl::make_unexpected(res.error());
      }

      // Copy returned addresses
      res->at(accessPointKey).to(apAddresses);
      res->at(dealerKey).to(dealerAddresses);
      res->at(spClientKey).to(spClientAddresses);

      // Expire in 1h
      addressesExpiresAt = now + std::chrono::hours(1);
    }

    if (apAddresses.empty() || dealerAddresses.empty() ||
        spClientAddresses.empty()) {
      return bell::make_unexpected_errc<std::string>(std::errc::bad_message);
    }

    switch (type) {
      case AddressType::AccessPoint:
        return this->apAddresses[0];
      case AddressType::Dealer:
        return this->dealerAddresses[0];
      case AddressType::SpClient:
        return this->spClientAddresses[0];
    }

    return bell::make_unexpected_errc<std::string>(std::errc::bad_message);
  }

  bell::Result<std::string> getClientToken(
      sysclock_timepoint now = std::chrono::system_clock::now()) override {
    std::scoped_lock lock(this->accessMutex);

    // Check if the address is expired
    if (now > clientTokenExpiresAt) {
      auto res = fetchClientToken();
      if (!res) {
        return tl::make_unexpected(res.error());
      }

      clientToken = res->first;
      clientTokenExpiresAt = now + std::chrono::seconds(res->second);
    }

    return clientToken;
  }

  bell::Result<std::string> getAccessKey(
      sysclock_timepoint now = std::chrono::system_clock::now()) override {
    std::scoped_lock lock(this->accessMutex);

    // Check if the address is expired
    if (now > accessKeyExpiresAt) {
      auto res = fetchAccessKey();
      if (!res) {
        return tl::make_unexpected(res.error());
      }

      accessKey = res->first;
      accessKeyExpiresAt = now + std::chrono::seconds(res->second);
    }

    return accessKey;
  }

 private:
  const char* LOG_TAG = "CredentialsResolver";

  std::shared_ptr<bell::HTTPClient> httpClient;
  std::shared_ptr<AuthInfo> authInfo;

  std::recursive_mutex accessMutex;

  // Expiry dates
  sysclock_timepoint addressesExpiresAt;
  sysclock_timepoint clientTokenExpiresAt;
  sysclock_timepoint accessKeyExpiresAt;

  // Cached values
  std::vector<std::string> apAddresses;
  std::vector<std::string> dealerAddresses;
  std::vector<std::string> spClientAddresses;
  std::string clientToken;
  std::string accessKey;

  /**
   * @brief Fetches a list of ap addresses
   */
  bell::Result<tao::json::value> fetchApAdresses() {
    // Fetch new addresses
    auto response = httpClient->get(apResolveUrl);
    if (!response) {
      return tl::make_unexpected(response.error());
    }

    if (response->statusCode == 200) {
      return tao::json::from_string(*response->text());
    }

    return bell::make_unexpected_errc<tao::json::value>(
        std::errc::resource_unavailable_try_again);
  }

  /**
   * @brief Fetches a new client token and its expireation date
   */
  bell::Result<std::pair<std::string, int32_t>> fetchClientToken() {
    BELL_LOG(debug, LOG_TAG, "Fetching client token");
    cspot_proto::ClientTokenRequest request;

    request.requestType = ClientTokenRequestType_REQUEST_CLIENT_DATA_REQUEST;
    request.clientData.clientId = spotifyClientId;
    request.clientData.clientVersion = "0.1.0";
    request.clientData.connectivitySdkData.deviceId = authInfo->deviceId;

    std::vector<std::byte> encodedBuffer;
    if (!nanopb_helper::encodeToVector(request, encodedBuffer)) {
      return bell::make_unexpected_errc<std::pair<std::string, int32_t>>(
          std::errc::bad_message);
    }

    auto clientTokenResponse =
        httpClient->post("https://clienttoken.spotify.com/v1/clienttoken",
                         {{"Accept", "application/x-protobuf"},
                          {
                              "Content-Type",
                              "application/x-protobuf",
                          }},
                         encodedBuffer);

    if (!clientTokenResponse) {
      return tl::make_unexpected(clientTokenResponse.error());
    }

    if (clientTokenResponse->statusCode == 200 &&
        clientTokenResponse->contentLength > 0) {
      cspot_proto::ClientTokenResponse tokenResponse;

      if (!nanopb_helper::decodeFromBuffer(
              tokenResponse, *clientTokenResponse->bytesPtr(),
              *clientTokenResponse->bytesLength())) {
        BELL_LOG(error, LOG_TAG, "Could not decode");
        return bell::make_unexpected_errc<std::pair<std::string, int32_t>>(
            std::errc::bad_message);
      }

      if (tokenResponse.responseType !=
          ClientTokenResponseType_RESPONSE_GRANTED_TOKEN_RESPONSE) {
        BELL_LOG(error, LOG_TAG, "Invalid client token response, type = {}",
                 static_cast<int>(tokenResponse.responseType));
        return bell::make_unexpected_errc<std::pair<std::string, int32_t>>(
            std::errc::resource_unavailable_try_again);
      }

      BELL_LOG(debug, LOG_TAG, "Client token received, expires in {}",
               tokenResponse.grantedToken.expiresAfterSeconds);

      return std::pair(tokenResponse.grantedToken.token,
                       tokenResponse.grantedToken.expiresAfterSeconds);
    }

    return bell::make_unexpected_errc<std::pair<std::string, int32_t>>(
        std::errc::resource_unavailable_try_again);
  }

  /**
   * @brief Fetches a new authenticated access key
   */
  bell::Result<std::pair<std::string, int32_t>> fetchAccessKey() {
    auto tokenRes = getClientToken();

    if (!tokenRes) {
      return tl::make_unexpected(tokenRes.error());
    }

    // Prepare a protobuf login request
    cspot_proto::LoginRequest loginRequest;

    // Assign necessary request fields
    loginRequest.clientInfo.clientId = spotifyClientId;
    loginRequest.clientInfo.deviceId = authInfo->deviceId;
    loginRequest.storedCredential.data = authInfo->loginCredentials->authData;
    loginRequest.storedCredential.username =
        authInfo->loginCredentials->username;

    std::vector<std::byte> encodedBuffer;
    if (!nanopb_helper::encodeToVector(loginRequest, encodedBuffer)) {
      return bell::make_unexpected_errc<std::pair<std::string, int32_t>>(
          std::errc::bad_message);
    }

    auto httpLoginResponse =
        httpClient->post("https://login5.spotify.com/v3/login",
                         {{"Accept", "application/x-protobuf"},
                          {
                              "Content-Type",
                              "application/x-protobuf",
                          },
                          {"Client-Token", *tokenRes}},
                         encodedBuffer);
    if (!httpLoginResponse) {
      return tl::make_unexpected(httpLoginResponse.error());
    }

    if (httpLoginResponse->statusCode == 200) {
      cspot_proto::LoginResponse loginResponse;

      if (!nanopb_helper::decodeFromBuffer(loginResponse,
                                           *httpLoginResponse->bytesPtr(),
                                           *httpLoginResponse->bytesLength())) {
        return bell::make_unexpected_errc<std::pair<std::string, int32_t>>(
            std::errc::bad_message);
      }
      if (loginResponse.loginError.hasValue) {
        BELL_LOG(error, LOG_TAG,
                 "Error while fetching access key (LoginError enum): {}",
                 static_cast<int>(loginResponse.loginError.value));
        return bell::make_unexpected_errc<std::pair<std::string, int32_t>>(
            std::errc::resource_unavailable_try_again);
      }

      return std::pair(loginResponse.loginOk.value.accessToken,
                       loginResponse.loginOk.value.accessTokenExpiresIn);
    }

    return bell::make_unexpected_errc<std::pair<std::string, int32_t>>(
        std::errc::bad_message);
  }
};

std::unique_ptr<CredentialsResolver> cspot::createDefaultCredentialsResolver(
    std::shared_ptr<bell::HTTPClient> httpClient,
    std::shared_ptr<AuthInfo> authInfo) {
  return std::make_unique<DefaultCredentialsResolver>(std::move(httpClient),
                                                      std::move(authInfo));
}
