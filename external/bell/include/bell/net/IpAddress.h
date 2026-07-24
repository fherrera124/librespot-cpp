#pragma once

// System includes
#include <sys/socket.h>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>

// Library includes
#include "tl/expected.hpp"

// Own includes
#include "bell/Result.h"

namespace bell::net {
class IpAddress {
 public:
  enum class Type { IPv4, IPv6, Unknown };

  // Default constructor
  IpAddress();

  // Create an address from a sockaddr structure. Should fill in the type and storage fields.
  IpAddress(const sockaddr* addr,
            std::optional<std::string> originalHost = std::nullopt);

  // Returns the address in a string format. For IPv4, this is the dotted decimal format. For IPv6, this is the colon-separated format.
  bell::Result<std::string> toString(bool includePort = true) const;

  // Returns the address type
  Type getType() const;

  // Returns the original host string, as passed to the resolveDomain() or fromString() functions
  std::optional<std::string> getOriginalHost() const;

  // Returns the port of the address, if set
  std::optional<uint16_t> getPort() const;

  // Returns the sockaddr structure pointer, for use with socket functions
  const sockaddr* getSockAddrPtrConst() const;

  // Returns the sockaddr structure pointer, for use with socket functions
  sockaddr* getSockAddrPtr();

  // Returns the size of the sockaddr structure
  socklen_t getSockAddrLen() const;

  // Sets the optional port for the address.
  void setPort(uint16_t port);

  // Returns the inet family of the address
  int getFamily() const;

  // Implement compare operator
  bool operator==(const IpAddress& other) const {
    if (addressType != other.addressType)
      return false;
    if (port != other.port)
      return false;
    if (addrLen != other.addrLen)
      return false;
    return memcmp(&storage, &other.storage, sizeof(storage)) == 0;
  }

  /**
   * @brief Create an address from the string representation of an IP address.
   *
   * @param addrStr The string representation of the IP address. This can be either an IPv4 or IPv6 address.
   *
   * @remark This function does not perform any DNS resolution. Use resolveDomain() for that.
   * @return optional<Address> The address structure if the string is a valid IP address, or std::nullopt if it is not.
   */
  static std::optional<IpAddress> fromString(const std::string& addrStr);

  /**
   * @brief Resolve the provided hostname to an IP address. In case the hostname is already an IP address, it is directly stored in the Address structure.
   */
  static bell::Result<IpAddress> resolveDomain(const std::string& hostname,
                                               int sockType,
                                               int family = AF_UNSPEC);

 private:
  Type addressType = Type::Unknown;
  sockaddr_storage storage{};
  socklen_t addrLen = 0;
  std::optional<uint16_t> port;
  std::optional<std::string> originalHost;
};
}  // namespace bell::net

namespace bell {
using IpAddress = net::IpAddress;
}
