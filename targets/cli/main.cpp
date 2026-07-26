#include <fstream>
#include <istream>

#include "AuthInfo.h"
#include "Authenticator.h"
#include "Session.h"
#include "Utils.h"
#include "bell/Logger.h"
#include "bell/http/Client.h"
#include "bell/http/Server.h"
#include "bell/mdns/Manager.h"
#include "bell/utils/Semaphore.h"
#include "proto/AuthenticationPb.h"

namespace {
const char* sessionFilePath = "session.json";
}

// Always started from main() regardless of whether a saved session already
// exists - the Spotify app's device picker discovers devices on the LAN via
// this same "_spotify-connect._tcp" mDNS advertisement + HTTP endpoint no
// matter our auth state. This used to only run while there was no saved
// session yet, which meant an already-paired device never advertised again
// on any later run and stayed invisible in the picker (confirmed on real
// hardware, targets/esp32's own build of this same code). httpServer,
// authenticator and authSemaphore are main()'s own locals, passed in here
// by reference/pointer - main() never returns before the process exits, so
// they outlive every use the registered handlers make of them, including a
// re-pairing POST that arrives long after this function itself returns.
//
// The returned Advertiser must be kept alive by the caller for as long as
// the device should stay discoverable: EspressifMDNAdvertiser's destructor
// calls mdns_service_remove() (stopAdvertising()), so letting it fall out
// of scope right after this function returns - as a local `auto service =
// ...` here used to - tears the announcement back down within
// microseconds of registering it. mdns_service_add() itself still reports
// success, so this was invisible from the return value alone: confirmed
// via `avahi-browse -a` on a real network showing every other
// _spotify-connect._tcp device (a Sangean radio) but never this one.
std::unique_ptr<bell::mdns::Advertiser> startZeroconfService(
    std::shared_ptr<cspot::AuthInfo> authInfo,
    std::shared_ptr<bell::http::Server> httpServer,
    cspot::Authenticator& authenticator, bell::Semaphore& authSemaphore) {
  httpServer->registerGet(
      "/spotify_handler",
      [&authenticator, authInfo](
          const std::unique_ptr<bell::http::Reader>& requestReader,
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
      [&authenticator, &authSemaphore, authInfo](
          const std::unique_ptr<bell::http::Reader>& requestReader,
          const std::unique_ptr<bell::http::Writer>& responseWriter,
          const auto& routeParams) {
        std::cout << "Received post request" << std::endl;
        cspot::logHeapStatus("Zeroconf", "POST /spotify_handler entry");
        auto bodyStr = *requestReader->getBodyStringView();
        tao::json::value responseJson;
        responseJson["status"] = 101;
        responseJson["statusString"] = "OK";
        responseJson["spotifyError"] = 0;

        auto responseString = tao::json::to_string(responseJson);
        (void)responseWriter->writeResponseWithBody(
            200, {{"Content-Type", "application/json"}}, responseString);

        auto res = authenticator.authenticateZeroconfString(authInfo->deviceId,
                                                             bodyStr);
        if (res) {
          BELL_LOG(info, "Zeroconf", "authenticated with spotify");
          authInfo->loginCredentials = *res;
          BELL_LOG(info, "Zeroconf", "user: {}, bloblen={}", res->username,
                   res->authData.size());
        } else {
          BELL_LOG(error, "Zeroconf", "failed to authenticate with spotify");
        }

        cspot::logHeapStatus("Zeroconf", "POST /spotify_handler exit");
        authSemaphore.give();
      });

  (void)httpServer->listen(2139);
  auto service =  // Register mdns service, for spotify to find us
      bell::mdns::getDefaultManager()->advertise(
          authInfo->deviceName, "_spotify-connect._tcp", "", "", 2139,
          {{"VERSION", "1.0"}, {"CPath", "/spotify_handler"}, {"Stack", "SP"}});
  if (!service) {
    BELL_LOG(error, "Zeroconf", "mDNS advertise failed - device won't be "
                                "discoverable by the Spotify app");
    return nullptr;
  }
  return std::move(*service);
};

int main(int argc, char** argv) {
  bell::registerDefaultLogger();

  auto authInfo = std::make_shared<cspot::AuthInfo>("Cspot player");

  std::ifstream sessionFile(sessionFilePath, std::ios::binary);
  if (sessionFile.is_open()) {
    std::string sessionString((std::istreambuf_iterator<char>(sessionFile)),
                              std::istreambuf_iterator<char>());
    sessionFile.close();
    if (!sessionString.empty()) {
      authInfo->assignDataFromJson(sessionString);
    }
  }
  authInfo->logDeviceIdOrigin();

  // These are main()'s own locals (not nested inside an if/else scope) so
  // they stay alive for the process's entire lifetime, matching the
  // handlers' own lifetime - see startZeroconfService()'s comment.
  auto httpServer = std::make_shared<bell::http::Server>();
  cspot::Authenticator authenticator;
  bell::Semaphore authSemaphore;
  // Must also outlive this scope, same reasoning as the three above - see
  // startZeroconfService()'s comment on why letting this fall out of scope
  // silently tears the mDNS announcement back down.
  auto mdnsService =
      startZeroconfService(authInfo, httpServer, authenticator, authSemaphore);

  if (!authInfo->loginCredentials.has_value()) {
    authSemaphore.take();
    if (!authInfo->loginCredentials.has_value() ||
        authInfo->loginCredentials->authData.empty()) {
      BELL_LOG(error, "Main", "No login credentials, exiting");
      return 1;
    }

    // Write session to file
    std::string sessionString = authInfo->toJson();
    std::ofstream outFile(sessionFilePath, std::ios::binary);
    if (outFile.is_open()) {
      outFile << sessionString;
      outFile.close();
    }
  }

  auto session = std::make_shared<cspot::Session>(authInfo);
  auto startRes = session->start();

  if (!startRes) {
    BELL_LOG(error, "Main", "Failed to start session: {}", startRes.error());
    return 1;
  }

  while (true) {
    session->runPoller();
  }
  return 0;
}
