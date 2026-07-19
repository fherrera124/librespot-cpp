#pragma once

#include <atomic>   // for atomic
#include <cstdint>  // for uint8_t, int32_t, int64_t
#include <memory>   // for shared_ptr
#include <string>   // for string
#include <vector>   // for vector

namespace cspot {
struct Context;

// User-session token provider for the Dealer WebSocket and spclient HTTP
// APIs, via Spotify's login5 flow (the mechanism real librespot/go-librespot
// clients use today - see docs/dealer_websocket_migration.md §4.1, which
// also records why the cheaper Mercury "keymaster" path was tried first and
// empirically discarded). Two HTTPS round-trips, both protobuf-bodied:
//   1. POST clienttoken.spotify.com/v1/clienttoken -> Client-Token header
//   2. POST login5.spotify.com/v3/login (stored AP credential) -> token,
//      solving a hashcash proof-of-work challenge in between if the server
//      demands one.
// Pure HTTPS - unlike the discarded keymaster probe, no dependency on the
// Mercury dispatch loop, so it can be called from any task.
class Login5Client {
 public:
  Login5Client(std::shared_ptr<cspot::Context> ctx);

  /**
  * @brief Checks if the held access token is expired (or none fetched yet)
  */
  bool isExpired();

  /**
  * @brief Returns a valid access token, blocking to fetch one if needed.
  * @returns token, or empty string if every attempt failed
  */
  std::string getToken();

  /**
  * @brief Forces a full refresh (client token reused if still valid)
  */
  void updateToken();

  /**
  * @brief Returns the Client-Token (spclient/login5 header), fetching one
  * if needed. Empty on failure.
  */
  std::string getClientToken();

 private:
  std::string fetchClientToken();
  bool login(const std::string& clientToken);
  bool solveHashcash(const std::vector<uint8_t>& loginContext,
                     const uint8_t* prefix, size_t prefixLen, int32_t length,
                     uint8_t suffixOut[16], int64_t& seconds, int32_t& nanos);

  std::shared_ptr<cspot::Context> ctx;

  std::atomic<bool> tokenPending = false;
  std::string token;
  long long int expiresAt = 0;

  std::string clientToken;
  long long int clientTokenExpiresAt = 0;
};
}  // namespace cspot
