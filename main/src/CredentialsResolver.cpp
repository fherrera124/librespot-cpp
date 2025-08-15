#include "api/CredentialsResolver.h"

// Library includes
#include <bell/http/Client.h>
#include <mutex>
#include <tao/json.hpp>
#include <tao/json/contrib/traits.hpp>
#include "bell/Logger.h"

// Own includes
#include "LoginBlob.h"
#include "NanoPBExtensions.h"

// Protobufs
#include "bell/Result.h"
#include "bell/http/Common.h"
#include "clienttoken.pb.h"
#include "login5.pb.h"
#include "tl/expected.hpp"

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
    "streaming,user-library-read,user-library-modify,user-top-read,user-read-"
    "recently-played";

// JSON keys
const std::string accessPointKey = "accesspoint";
const std::string dealerKey = "dealer-g2";
const std::string spClientKey = "spclient";
}  // namespace

CredentialsResolver::CredentialsResolver(std::shared_ptr<LoginBlob> loginBlob)
    : loginBlob(std::move(loginBlob)) {
  // Set expiration time to now, will be updated on first call
  this->addressesExpiresAt =
      std::chrono::system_clock::now() - std::chrono::hours(1);
  this->clientTokenExpiresAt =
      std::chrono::system_clock::now() - std::chrono::hours(1);
  this->accessKeyExpiresAt =
      std::chrono::system_clock::now() - std::chrono::hours(1);
}

bell::Result<std::string> CredentialsResolver::getApAddress(AddressType type) {
  std::scoped_lock lock(this->accessMutex);
  // Get current time in seconds
  auto currentTime = std::chrono::system_clock::now();

  // Check if the address is expired
  if (currentTime > addressesExpiresAt) {
    auto res = updateAddresses();
    if (!res) {
      return tl::make_unexpected(res.error());
    }
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

bell::Result<std::string> CredentialsResolver::getClientToken() {
  std::scoped_lock lock(this->accessMutex);
  // Get current time in seconds
  auto currentTime = std::chrono::system_clock::now();

  // Check if the token is expired
  if (currentTime > clientTokenExpiresAt) {
    auto res = updateClientToken();

    if (!res) {
      return tl::make_unexpected(res.error());
    }
  }

  return this->clientToken;
}

bell::Result<std::string> CredentialsResolver::getAccessKey() {
  std::scoped_lock lock(this->accessMutex);
  // Get current time in seconds
  auto currentTime = std::chrono::system_clock::now();

  // Check if the key is expired
  if (currentTime > accessKeyExpiresAt) {
    auto res = updateAccessKey();
    if (!res) {
      return tl::make_unexpected(res.error());
    }
  }

  return this->accessKey;
}

bell::Result<> CredentialsResolver::updateAddresses() {
  std::scoped_lock lock(this->accessMutex);

  // Fetch new addresses
  auto response = bell::http::request(bell::HTTPMethod::GET, apResolveUrl);
  if (!response) {
    return tl::make_unexpected(response.error());
  }

  if (response->getStatusCode() == 200) {
    auto responseStr = *response->getBodyStringView();

    // parse json
    const auto& json = tao::json::from_string(responseStr);

    if (json.at(accessPointKey).is_array() && json.at(dealerKey).is_array() &&
        json.at(spClientKey).is_array()) {
      this->apAddresses =
          json.at(accessPointKey).as<std::vector<std::string>>();
      json.at(accessPointKey).to(this->apAddresses);
      json.at(dealerKey).to(this->dealerAddresses);
      json.at(spClientKey).to(this->spClientAddresses);
    } else {
      return bell::make_unexpected_errc(std::errc::bad_message);
    }
  } else {
    return bell::make_unexpected_errc(
        std::errc::resource_unavailable_try_again);
  }

  // Set expiration time to 1 hour from now
  this->addressesExpiresAt =
      std::chrono::system_clock::now() + std::chrono::hours(1);

  return {};
}

bell::Result<> CredentialsResolver::updateAccessKey() {
  std::scoped_lock lock(this->accessMutex);

  if (!loginBlob->isAuthenticated()) {
    BELL_LOG(error, LOG_TAG,
             "Cannot fetch access key, user is not authenticated");

    return bell::make_unexpected_errc(std::errc::operation_not_permitted);
  }

  auto tokenRes = getClientToken();

  if (!tokenRes) {
    return tl::make_unexpected(tokenRes.error());
  }

  // Prepare a protobuf login request
  LoginRequest loginRequest = LoginRequest_init_zero;
  LoginResponse loginResponse = LoginResponse_init_zero;

  // Assign necessary request fields
  loginRequest.client_info.client_id.funcs.encode = &cspot::pbEncodeString;
  loginRequest.client_info.client_id.arg = &spotifyClientId;

  std::string deviceId = loginBlob->getDeviceId();
  std::string username = loginBlob->getUsername();

  loginRequest.client_info.device_id.funcs.encode = &cspot::pbEncodeString;
  loginRequest.client_info.device_id.arg = &deviceId;

  loginRequest.login_method.stored_credential.username.funcs.encode =
      &cspot::pbEncodeString;
  loginRequest.login_method.stored_credential.username.arg = &username;

  // Set login method to stored credential
  loginRequest.which_login_method = LoginRequest_stored_credential_tag;
  loginRequest.login_method.stored_credential.data.funcs.encode =
      &cspot::pbEncodeUint8Vector;

  std::vector<uint8_t> authData = loginBlob->getStoredAuthBlob();
  loginRequest.login_method.stored_credential.data.arg = &authData;

  auto encodedSizeRes =
      pbCalculateEncodedSize(LoginRequest_fields, &loginRequest);
  if (!encodedSizeRes) {
    return tl::make_unexpected(encodedSizeRes.error());
  }

  std::vector<std::byte> encodedBuffer(*encodedSizeRes);
  auto res =
      pbEncodeMessage(reinterpret_cast<uint8_t*>(encodedBuffer.data()),
                      encodedBuffer.size(), LoginRequest_fields, &loginRequest);
  if (!res) {
    // Could not encode the message
    return tl::make_unexpected(res.error());
  }

  auto httpConnectionResponse = bell::http::requestWithBody(
      bell::HTTPMethod::POST, "https://login5.spotify.com/v3/login",
      {{"Accept", "application/x-protobuf"},
       {
           "Content-Type",
           "application/x-protobuf",
       },
       {"Client-Token", *tokenRes}},
      encodedBuffer);
  if (!httpConnectionResponse) {
    return tl::make_unexpected(httpConnectionResponse.error());
  }

  if (httpConnectionResponse->getStatusCode() == 200) {
    loginResponse.ok.access_token.funcs.decode = &cspot::pbDecodeString;
    loginResponse.ok.access_token.arg = &this->accessKey;

    auto decodeRes =
        pbDecodeMessage(reinterpret_cast<const uint8_t*>(
                            *httpConnectionResponse->getBodyBytesPtr()),
                        *httpConnectionResponse->getBodyBytesLength(),
                        LoginResponse_fields, &loginResponse);

    if (!decodeRes) {
      return decodeRes;
    }

    if (loginResponse.has_error) {
      BELL_LOG(error, LOG_TAG,
               "Error while fetching access key (LoginError enum): {}",
               static_cast<int>(loginResponse.error));
      return bell::make_unexpected_errc(
          std::errc::resource_unavailable_try_again);
    }

    this->accessKeyExpiresAt =
        std::chrono::system_clock::now() +
        std::chrono::seconds(loginResponse.ok.access_token_expires_in);

    BELL_LOG(debug, LOG_TAG, "Access key received, expires in {}",
             loginResponse.ok.access_token_expires_in);
  } else {
    BELL_LOG(error, LOG_TAG,
             "Error while fetching access key (HTTP status code): {}",
             *httpConnectionResponse->getStatusCode());
    return bell::make_unexpected_errc(
        std::errc::resource_unavailable_try_again);
  }

  return {};
}

bell::Result<> CredentialsResolver::updateClientToken() {
  std::scoped_lock lock(this->accessMutex);
  BELL_LOG(debug, LOG_TAG, "Fetching client token");
  ClientTokenRequest request = ClientTokenRequest_init_zero;

  // Prepare request
  request.request_type = ClientTokenRequestType_REQUEST_CLIENT_DATA_REQUEST;
  request.which_request = ClientTokenRequest_client_data_tag;

  std::string clientVersion = "0.1.0";
  std::string deviceId = loginBlob->getDeviceId();

  ClientDataRequest* clientDataRequest = &request.request.client_data;
  clientDataRequest->client_version.funcs.encode = pbEncodeString;
  clientDataRequest->client_version.arg = &clientVersion;

  clientDataRequest->client_id.funcs.encode = &pbEncodeString;
  clientDataRequest->client_id.arg = &spotifyClientId;
  clientDataRequest->which_data = ClientDataRequest_connectivity_sdk_data_tag;
  clientDataRequest->data.connectivity_sdk_data.device_id.funcs.encode =
      &pbEncodeString;
  clientDataRequest->data.connectivity_sdk_data.device_id.arg = &deviceId;

  clientDataRequest->data.connectivity_sdk_data.has_platform_specific_data =
      true;
  clientDataRequest->data.connectivity_sdk_data.platform_specific_data
      .which_data = PlatformSpecificData_desktop_linux_tag;
  clientDataRequest->data.connectivity_sdk_data.platform_specific_data.data
      .desktop_linux = NativeDesktopLinuxData_init_zero;

  auto encodedSizeRes =
      pbCalculateEncodedSize(ClientTokenRequest_fields, &request);

  if (!encodedSizeRes) {
    return tl::make_unexpected(encodedSizeRes.error());
  }

  std::vector<std::byte> encodedBuffer(*encodedSizeRes);
  auto res = pbEncodeMessage(reinterpret_cast<uint8_t*>(encodedBuffer.data()),
                             encodedBuffer.size(), ClientTokenRequest_fields,
                             &request);
  if (!res) {
    // Could not encode the message
    return tl::make_unexpected(res.error());
  }

  auto httpConnectionResponse = bell::http::requestWithBody(
      bell::HTTPMethod::POST, "https://clienttoken.spotify.com/v1/clienttoken",
      {{"Accept", "application/x-protobuf"},
       {
           "Content-Type",
           "application/x-protobuf",
       }},
      encodedBuffer);
  if (!httpConnectionResponse) {
    return tl::make_unexpected(httpConnectionResponse.error());
  }

  if (httpConnectionResponse->getContentLength() > 0) {
    ClientTokenResponse tokenResponse = ClientTokenResponse_init_zero;
    std::string clientTokenString;

    tokenResponse.granted_token.token.funcs.decode = &pbDecodeString;
    tokenResponse.granted_token.token.arg = &clientTokenString;

    auto res = pbDecodeMessage(reinterpret_cast<const uint8_t*>(
                                   *httpConnectionResponse->getBodyBytesPtr()),
                               *httpConnectionResponse->getBodyBytesLength(),
                               ClientTokenResponse_fields, &tokenResponse);

    if (!res) {
      return res;
    }

    // Save the token
    this->clientToken = clientTokenString;
    this->clientTokenExpiresAt =
        std::chrono::system_clock::now() +
        std::chrono::seconds(tokenResponse.granted_token.expires_after_seconds);

    BELL_LOG(debug, LOG_TAG, "Client token received, expires in {}",
             tokenResponse.granted_token.expires_after_seconds);
  }

  return {};
}
