#include <unistd.h>
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <iostream>
#include "CDNAudioStream.h"
#include "LoginBlob.h"
#include "Session.h"
#include "SessionContext.h"
#include "Utils.h"
#include "api/ApClient.h"
#include "api/ApConnection.h"
#include "audio/VorbisAudioStream.h"
#include "bell/http/Client.h"
#include "bell/http/Reader.h"
#include "bell/http/Server.h"
#include "bell/http/Writer.h"
#include "bell/mdns/Manager.h"
#include "bell/utils/Utils.h"

#include "crypto/DiffieHellman.h"
#include "proto/KeyexchangePb.h"
#include "tao/json/forward.hpp"

TEST_CASE("MiscSpotifyApi tests", "[MiscSpotifyApi]") {
    // auto startTime = std::chrono::high_resolution_clock::now();
  // std::string url = "https://apresolve.spotify.com";
  // auto res = bell::http::request(bell::HTTPMethod::GET, url);
  // auto body = res->getBodyStringView();
  // std::cout << *body << std::endl;
  // auto elapsed = std::chrono::high_resolution_clock::now() - startTime;
  // std::cout << "Elapsed time: " << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() << " ms" << std::endl;
  // return;
  // std::vector<uint8_t> fileId = {0x01, 0x02, 0x03, 0x04, 0x05};
  // std::string cdnUrl = "https://audio-fa-quic.spotifycdn.com/audio/8a167f50899816665fc2fbb858dd2bfa3223f135?1753799812_uSHtFc8tVUWWD4iiM4zEZFeVBjo1MO_KNWwecDQQdxc=";
  // // auto cdnStream = std::make_unique<cspot::CDNAudioStream>(cdnUrl, fileId);

  // return;
  // auto dh = cspot::DH();
  // std::cout << "DH public key: " << std::endl;
  // cspot::logDataBase64(dh.getPublicKey().data(), dh.getPublicKey().size());
  // // cspot::logDataBase64(dh.getPrivateKey().data(), dh.getPrivateKey().size());
  // test();
  // return;
  // auto credentials = std::make_shared<cspot::SessionCredentials>("dupa");

  // Check if "session.json" exists, if it does, load it
  std::ifstream sessionFile("session.json", std::ios::binary);
  if (sessionFile.is_open()) {
    std::string sessionBlob((std::istreambuf_iterator<char>(sessionFile)),
                            std::istreambuf_iterator<char>());
    sessionFile.close();
    auto jsonData = tao::json::from_string(sessionBlob);
    auto loginBlob = std::make_shared<cspot::LoginBlob>();
    auto res = loginBlob->restoreFromJSON(jsonData);
    if (!res) {
      std::cerr << "Failed to restore session from JSON: "
                << res.error().message() << std::endl;
      return;
    }
    std::cout << "Restored session from session.json" << std::endl;
    std::cout << "Device name: " << loginBlob->getDeviceName() << std::endl;
    std::cout << "Device ID: " << loginBlob->getDeviceId() << std::endl;
    std::cout << "Username: " << loginBlob->getUsername() << std::endl;

    auto session = std::make_unique<cspot::Session>(loginBlob);
    (void)session->start();

    while (true) {
      session->runPoller();
    }
  } else {

    // // // cspot::ApConnection apConnection(url);
    auto loginBlob = std::make_shared<cspot::LoginBlob>();
    loginBlob->setDeviceName("rice-tortilla");

    // return;
    auto httpServer = std::make_shared<bell::http::Server>();

    // Get info handler
    httpServer->registerGet(
        "/spotify_handler",
        [&loginBlob](const std::unique_ptr<bell::http::Reader>& requestReader,
                     const std::unique_ptr<bell::http::Writer>& responseWriter,
                     const auto& routeParams) {
          auto queryParams = *requestReader->getQueryParams();
          std::cout << "Received get request" << std::endl;

          if (queryParams.find("action") != queryParams.end() &&
              queryParams["action"] == "getInfo") {
            auto zeroConfString = loginBlob->buildZeroconfJSONResponse();
            (void)responseWriter->writeResponseWithBody(
                200, {{"Content-Type", "application/json"}}, zeroConfString);
          } else {
            (void)responseWriter->writeResponseWithBody(500, {},
                                                        "Invalid action");
          }
        });

    httpServer->registerPost(
        "/spotify_handler",
        [&loginBlob](const std::unique_ptr<bell::http::Reader>& requestReader,
                     const std::unique_ptr<bell::http::Writer>& responseWriter,
                     const auto& routeParams) {
          std::cout << "Received post request" << std::endl;
          auto bodyStr = *requestReader->getBodyStringView();

          auto zeroConfString = loginBlob->buildZeroconfJSONResponse();
          (void)responseWriter->writeResponseWithBody(
              200, {{"Content-Type", "application/json"}}, zeroConfString);

          (void)loginBlob->authenticateZeroconfString(bodyStr);
          std::string blobStr =
              tao::json::to_string(*loginBlob->getJSONForStorage());
          // open "session.json" and write the blob to it
          std::ofstream sessionFile("session.json", std::ios::binary);
          if (sessionFile.is_open()) {
            sessionFile.write(blobStr.data(), blobStr.size());
            sessionFile.close();
            std::cout << "Session saved to session.json" << std::endl;
            exit(1);
          }
        });

    (void)httpServer->listen(2139);
    auto service =  // Register mdns service, for spotify to find us
        bell::mdns::getDefaultManager()->advertise(
            loginBlob->getDeviceName(), "_spotify-connect._tcp", "", "", 2139,
            {{"VERSION", "1.0"},
             {"CPath", "/spotify_handler"},
             {"Stack", "SP"}});

    while (true) {
      // sessionContext->socketPoll.poll();
      bell::utils::sleepMs(1000);
    }
  }
}
