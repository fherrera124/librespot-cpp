#include <atomic>
#include <fstream>

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

// Runs for the process's entire lifetime, independent of pairing/session
// state, so the device stays visible/selectable in the Spotify app's
// picker at all times. The returned Advertiser must be kept alive by the
// caller: its destructor tears down the mDNS announcement.
std::unique_ptr<bell::mdns::Advertiser> startZeroconfService(
    std::shared_ptr<cspot::AuthInfo> authInfo,
    std::shared_ptr<bell::http::Server> httpServer,
    cspot::Authenticator& authenticator, bell::Semaphore& authSemaphore,
    std::atomic<bool>& needsSessionRestart) {
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
      [&authenticator, &authSemaphore, authInfo, &needsSessionRestart](
          const std::unique_ptr<bell::http::Reader>& requestReader,
          const std::unique_ptr<bell::http::Writer>& responseWriter,
          const auto& routeParams) {
        std::cout << "Received post request" << std::endl;
        cspot::logHeapStatus("Zeroconf", "POST /spotify_handler entry");
        auto bodyStr = *requestReader->getBodyStringView();

        auto res = authenticator.authenticateZeroconfString(authInfo->deviceId,
                                                             bodyStr);
        if (res) {
          BELL_LOG(info, "Zeroconf", "authenticated with spotify");
          authInfo->loginCredentials = *res;
          BELL_LOG(info, "Zeroconf", "user: {}, bloblen={}", res->username,
                   res->authData.size());

          tao::json::value responseJson;
          responseJson["status"] = 101;
          responseJson["statusString"] = "OK";
          responseJson["spotifyError"] = 0;
          (void)responseWriter->writeResponseWithBody(
              200, {{"Content-Type", "application/json"}},
              tao::json::to_string(responseJson));

          // Wakes main()'s loop to (re)build the Session with these
          // credentials, replacing whatever pairing (if any) is already
          // active. give() only fires on success so the loop never wakes
          // to rebuild against stale/unchanged credentials.
          needsSessionRestart.store(true);
          authSemaphore.give();
        } else {
          BELL_LOG(error, "Zeroconf", "failed to authenticate with spotify");
          (void)responseWriter->writeResponseWithBody(500, {}, "");
        }

        cspot::logHeapStatus("Zeroconf", "POST /spotify_handler exit");
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

  auto httpServer = std::make_shared<bell::http::Server>();
  cspot::Authenticator authenticator;
  bell::Semaphore authSemaphore;
  std::atomic<bool> needsSessionRestart{false};
  auto mdnsService = startZeroconfService(authInfo, httpServer, authenticator,
                                          authSemaphore, needsSessionRestart);

  auto persistSession = [&authInfo]() {
    std::string sessionString = authInfo->toJson();
    std::ofstream outFile(sessionFilePath, std::ios::binary);
    if (outFile.is_open()) {
      outFile << sessionString;
      outFile.close();
    }
  };
  persistSession();

  // haveCredentialsToTry: skips the initial wait when a persisted session
  // was loaded above, so it's tried once before falling back to waiting
  // for a pairing.
  std::shared_ptr<cspot::Session> session;
  bool haveCredentialsToTry = authInfo->loginCredentials.has_value();
  while (true) {
    if (!haveCredentialsToTry) {
      authSemaphore.take();
      needsSessionRestart.store(false);

      if (!authInfo->loginCredentials.has_value() ||
          authInfo->loginCredentials->authData.empty()) {
        continue;
      }
    }
    haveCredentialsToTry = false;

    BELL_LOG(info, "Main",
             session ? "Rebuilding session after a fresh pairing"
                     : "Starting session");
    if (session) {
      auto oldInactiveRes = session->putInactive();
      if (!oldInactiveRes) {
        BELL_LOG(warn, "Main", "putInactive on old session failed: {}",
                 oldInactiveRes.error());
      }
    }
    session.reset();
    session = std::make_shared<cspot::Session>(authInfo);
    auto startRes = session->start();
    if (!startRes) {
      BELL_LOG(error, "Main",
               "Failed to start session: {} - waiting for another pairing "
               "attempt",
               startRes.error());
      continue;
    }
    persistSession();

    // Returns on a fresh pairing (needsSessionRestart) or a rejected login
    // (Session::credentialsRejected()) - AP/dealer transport failures are
    // retried internally and never surface here.
    session->runPoller(needsSessionRestart);

    if (session->credentialsRejected()) {
      BELL_LOG(error, "Main",
               "Credentials rejected by server - clearing and waiting for "
               "a new pairing");
      authInfo->loginCredentials.reset();
      persistSession();
    }
  }
  return 0;
}
