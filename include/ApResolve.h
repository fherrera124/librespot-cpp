#pragma once

#include <string>  // for string
#include <vector>  // for vector

namespace cspot {
class ApResolve {
 public:
  ApResolve(std::string apOverride);

  /**
   * @brief Connects to spotify's servers and returns first valid ap address
   * @returns std::string Address in form of url:port
   */
  std::string fetchFirstApAddress();

  /**
   * @brief All candidate Dealer WebSocket endpoints (host:port), in the
   * order apresolve.spotify.com returned them - DealerClient tries each
   * before giving up, instead of getting stuck on a single bad host (same
   * "try every resolved address" pattern as PlainConnection::connect()'s
   * F17 fix; matches go-librespot's own dealer address rotation).
   */
  std::vector<std::string> fetchDealerAddresses();

  /**
   * @brief All candidate login accesspoints (host:port), same rationale as
   * fetchDealerAddresses() - Session::connectWithRandomAp() tries each
   * before giving up, instead of the single, non-retried AP
   * fetchFirstApAddress() (misleadingly named - it's not random, always
   * index 0) used to leave it stuck on.
   */
  std::vector<std::string> fetchApAddresses();

  /**
   * @brief Resolves the spclient HTTP endpoint (host:port), used for
   * connect-state PUTs. See docs/dealer_websocket_migration.md §4.2.
   */
  std::string fetchFirstSpclientAddress();

 private:
  std::vector<std::string> fetchAddressesOfType(const std::string& type);
  std::string fetchFirstAddressOfType(const std::string& type);

  std::string apOverride;
};
}  // namespace cspot
