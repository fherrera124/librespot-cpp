#include "ZeroconfServer.h"

#include <utility>

#include "Utils.h"
#include "bell/Logger.h"
#include "tao/json.hpp"

using namespace cspot;

ZeroconfServer::ZeroconfServer(std::shared_ptr<AuthInfo> authInfo,
                               std::shared_ptr<bell::http::Server> httpServer,
                               std::function<void()> onNewCredentials)
    : authInfo(std::move(authInfo)),
      httpServer(std::move(httpServer)),
      onNewCredentials(std::move(onNewCredentials)) {}

std::unique_ptr<bell::mdns::Advertiser> ZeroconfServer::start(uint16_t port) {
  httpServer->registerGet(
      "/spotify_handler",
      [this](const std::unique_ptr<bell::http::Reader>& requestReader,
             const std::unique_ptr<bell::http::Writer>& responseWriter,
             const auto& routeParams) {
        auto queryParams = *requestReader->getQueryParams();
        BELL_LOG(info, "Zeroconf", "Received GET Request");
        cspot::logHeapStatus("Zeroconf", "GET /spotify_handler entry");

        if (queryParams.find("action") != queryParams.end() &&
            queryParams["action"] == "getInfo") {
          auto zeroConfString = authenticator.buildZeroconfJSONResponse(
              authInfo->deviceName, authInfo->deviceId, "");
          (void)responseWriter->writeResponseWithBody(
              200, {{"Content-Type", "application/json"}}, zeroConfString);
        } else {
          (void)responseWriter->writeResponseWithBody(500, {},
                                                      "Invalid action");
        }
        cspot::logHeapStatus("Zeroconf", "GET /spotify_handler exit");
      });

  httpServer->registerPost(
      "/spotify_handler",
      [this](const std::unique_ptr<bell::http::Reader>& requestReader,
             const std::unique_ptr<bell::http::Writer>& responseWriter,
             const auto& routeParams) {
        BELL_LOG(info, "Zeroconf", "Received POST Request");
        cspot::logHeapStatus("Zeroconf", "POST /spotify_handler entry");

        auto bodyStr = *requestReader->getBodyStringView();

        auto res = authenticator.authenticateZeroconfString(
            authInfo->deviceId, bodyStr);
        if (res) {
          BELL_LOG(info, "Zeroconf", "authenticated with spotify");
          authInfo->loginCredentials = *res;

          tao::json::value responseJson;
          responseJson["status"] = 101;
          responseJson["statusString"] = "OK";
          responseJson["spotifyError"] = 0;
          (void)responseWriter->writeResponseWithBody(
              200, {{"Content-Type", "application/json"}},
              tao::json::to_string(responseJson));

          // Wakes the caller's session loop to (re)build the Session with
          // these credentials, replacing whatever pairing (if any) is
          // already active.
          onNewCredentials();
        } else {
          BELL_LOG(error, "Zeroconf", "failed to authenticate with spotify");
          (void)responseWriter->writeResponseWithBody(500, {}, "");
        }

        cspot::logHeapStatus("Zeroconf", "POST /spotify_handler exit");
      });

  (void)httpServer->listen(port);
  auto service = bell::mdns::getDefaultManager()->advertise(
      authInfo->deviceName, "_spotify-connect._tcp", "", "", port,
      {{"VERSION", "1.0"}, {"CPath", "/spotify_handler"}, {"Stack", "SP"}});
  if (!service) {
    BELL_LOG(error, "Zeroconf", "mDNS advertise failed - device won't be "
                                "discoverable by the Spotify app");
    return nullptr;
  }
  return std::move(*service);
}
