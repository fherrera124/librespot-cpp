#pragma once

#include <chrono>
#include <string>

#include "bell/Result.h"
#include "bell/http/Client.h"

#include "AuthInfo.h"

namespace cspot {
// Clock defined as template, allows for testing with a mocked clock externally
class CredentialsResolver {
 public:
  virtual ~CredentialsResolver() = default;

  // Enumeration of the endpoint types
  enum class AddressType {
    AccessPoint,
    Dealer,
    SpClient,
  };

  // Alias for a timepoint
  using sysclock_timepoint = std::chrono::time_point<std::chrono::system_clock>;

  /**
   * @brief Resolve the address of the access point, dealer, or spClient.
   *
   * @note This function will fetch the addresses from the Spotify API if they are not already cached, or are expired.
   *
   * @param type The type of address to resolve
   * @return std::string The resolved address
   */
  virtual bell::Result<std::string> getApAddress(
      AddressType type,
      sysclock_timepoint now = std::chrono::system_clock::now()) = 0;

  /**
   * @brief Retrieve the Spotify client token, using the "clienttoken.spotify.com" endpoint, caching the token for subsequent calls.
   *
   * @return std::string retrieved token
   */
  virtual bell::Result<std::string> getClientToken(
      sysclock_timepoint now = std::chrono::system_clock::now()) = 0;

  /**
  * @brief Fetches a new access key from the Spotify API, caching the key for subsequent calls.
  *
  * @returns std::string access key
  */
  virtual bell::Result<std::string> getAccessKey(
      sysclock_timepoint now = std::chrono::system_clock::now()) = 0;
};

/**
 * Creates an instance of the default credentials resolver
 */
std::unique_ptr<CredentialsResolver> createDefaultCredentialsResolver(
    std::shared_ptr<bell::HTTPClient> httpClient,
    std::shared_ptr<AuthInfo> authInfo);
}  // namespace cspot
