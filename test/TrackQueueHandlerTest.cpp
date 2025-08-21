#include <doctest/doctest.h>
#include <fstream>
#include <iostream>
#include <tao/json.hpp>
#include <trompeloeil.hpp>
#include "Utils.h"
#include "api/CredentialsResolver.h"
#include "api/SpClient.h"
#include "authentication.pb.h"
#include "bell/http/Client.h"
#include "tracks/TrackQueueHandler.h"

// #include "mocks/MockSpClient.h"

TEST_CASE("ContextTrackProvider tests") {
  using trompeloeil::_;  // wild card for matching any value
  auto authInfo = std::make_shared<cspot::AuthInfo>();
  auto httpClient = std::make_shared<bell::HTTPClient>();
  std::shared_ptr<cspot::CredentialsResolver> credentialsResolver =
      cspot::createDefaultCredentialsResolver(httpClient, authInfo);
  std::shared_ptr<cspot::SpClient> spClient =
      cspot::createDefaultSpClient(httpClient, credentialsResolver);
  auto queueHandler = cspot::createDefaultTrackQueueHandler(
      spClient, std::make_shared<cspot::EventLoop>());

  std::ifstream sessionFile("session.json", std::ios::binary);
  if (sessionFile.is_open()) {
    std::string sessionBlob((std::istreambuf_iterator<char>(sessionFile)),
                            std::istreambuf_iterator<char>());
    sessionFile.close();
    auto jsonData = tao::json::from_string(sessionBlob);

    std::string authBlob = jsonData["authBlob"].as<std::string>();
    std::string deviceId = jsonData["deviceId"].as<std::string>();
    authInfo->loginCredentials->authData = cspot::base64Decode(authBlob);
    authInfo->loginCredentials->username =
        jsonData["username"].as<std::string>();
    authInfo->deviceId = deviceId;
    authInfo->loginCredentials->type =
        AuthenticationType_AUTHENTICATION_STORED_SPOTIFY_CREDENTIALS;

    // auto res =
    //     spClient->contextResolve("spotify:show:4wEuac2C7cpuvy8HBjfvW7");

    //     // spClient->rawRequest("radio-apollo/v3/tracks/spotify:show:4wEuac2C7cpuvy8HBjfvW7");
    // if (res) {
    //     std::cout << res->statusCode << " " << res->statusMessage << std::endl;
    //   std::cout << *res->text() << std::endl;
    // } else {
    //     std::cout << "Error" << std::endl;
    //     std::cout << res.error() << std::endl;
    // }

    (void)queueHandler->loadContext("spotify:artist:7fVp0A6oCMfiQJihMnY0SZ",
                                    std::nullopt,
                                    "toptrack3edXLmc4WGO8r2g5gjh0Ux");
  }
}
