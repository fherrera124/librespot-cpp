#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "AuthInfo.h"
#include "Authenticator.h"
#include "bell/http/Server.h"
#include "bell/mdns/Manager.h"

namespace cspot {

// Runs the Spotify Connect pairing handshake: registers the
// /spotify_handler HTTP endpoints and advertises the device over mDNS so
// it's selectable in the Spotify app's device picker. Successful pairings
// are written directly to authInfo->loginCredentials and reported through
// onNewCredentials.
class ZeroconfServer {
 public:
  ZeroconfServer(std::shared_ptr<AuthInfo> authInfo,
                 std::shared_ptr<bell::http::Server> httpServer,
                 std::function<void()> onNewCredentials);

  // Registers the handlers, starts listening and advertises over mDNS.
  // Returns nullptr (and logs) if the mDNS advertise fails - the caller
  // still owns a listening httpServer, it just won't be discoverable.
  // The returned Advertiser must be kept alive by the caller: its
  // destructor tears down the mDNS announcement.
  std::unique_ptr<bell::mdns::Advertiser> start(uint16_t port = 2139);

  // Reported as activeUser in the getInfo response - empty means no
  // confirmed active session. Thread-safe: called from ConnectReceiver::
  // run()'s thread, read from the GET handler on httpServer's own thread.
  void setCurrentUser(std::string username);

 private:
  std::shared_ptr<AuthInfo> authInfo;
  std::shared_ptr<bell::http::Server> httpServer;
  Authenticator authenticator;
  std::function<void()> onNewCredentials;

  std::mutex currentUserMutex_;
  std::string currentUser_;
};

}  // namespace cspot
