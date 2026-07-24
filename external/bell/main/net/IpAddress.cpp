#include "bell/net/IpAddress.h"

// Socket-related includes
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>

#include <fmt/format.h>
#include <system_error>

#include "bell/Result.h"

using namespace bell::net;

IpAddress::IpAddress() {
  std::memset(&storage, 0, sizeof(storage));
}

IpAddress::IpAddress(const sockaddr* addr,
                     std::optional<std::string> originalHost)
    : originalHost(std::move(originalHost)) {
  // Copy the address and determine the address type and length
  if (addr->sa_family == AF_INET) {
    addressType = Type::IPv4;
    std::memcpy(&storage, addr, sizeof(sockaddr_in));
    addrLen = sizeof(sockaddr_in);

    // Extract the port, if available
    const auto* addr_in = reinterpret_cast<const sockaddr_in*>(addr);
    port = ntohs(addr_in->sin_port);
  } else if (addr->sa_family == AF_INET6) {
    addressType = Type::IPv6;
    std::memcpy(&storage, addr, sizeof(sockaddr_in6));
    addrLen = sizeof(sockaddr_in6);

    const auto* addr_in = reinterpret_cast<const sockaddr_in6*>(addr);
    port = ntohs(addr_in->sin6_port);
  }

  if (port == 0) {
    // No port was set, so clear it
    port = std::nullopt;
  }
}

bell::Result<std::string> IpAddress::toString(bool includePort) const {
  // Reserve enough space for largest (IPv6) address
  std::string result(INET6_ADDRSTRLEN, '\0');

  if (addressType == Type::IPv4) {
    const auto* addr_in = reinterpret_cast<const sockaddr_in*>(&storage);
    if (inet_ntop(AF_INET, &addr_in->sin_addr, result.data(), result.size())) {
      result.erase(result.find('\0'));
      if (includePort && port.has_value()) {
        result += ":" + std::to_string(port.value());
      }
    }
  } else if (addressType == Type::IPv6) {
    const auto* addr_in6 = reinterpret_cast<const sockaddr_in6*>(&storage);
    if (inet_ntop(AF_INET6, &addr_in6->sin6_addr, result.data(),
                  result.size())) {
      result.erase(result.find('\0'));
      if (includePort && port.has_value()) {
        result += ":" + std::to_string(port.value());
      }
    }
  } else {
    // Unknown address type
    return make_unexpected_errc<std::string>(
        std::errc::address_family_not_supported);
  }

  return result;
}

IpAddress::Type IpAddress::getType() const {
  return addressType;
}

const sockaddr* IpAddress::getSockAddrPtrConst() const {
  return reinterpret_cast<const sockaddr*>(&storage);
}

sockaddr* IpAddress::getSockAddrPtr() {
  return reinterpret_cast<sockaddr*>(&storage);
}

socklen_t IpAddress::getSockAddrLen() const {
  return addrLen;
}

void IpAddress::setPort(uint16_t port) {
  // In case port is 0, we set it to the default port for the address type
  uint16_t actualPort = port > 0 ? htons(port) : port;

  // Set the port in the address structure
  if (addressType == Type::IPv4) {
    auto* addr_in = reinterpret_cast<sockaddr_in*>(&storage);
    addr_in->sin_port = actualPort;
  } else if (addressType == Type::IPv6) {
    auto* addr_in6 = reinterpret_cast<sockaddr_in6*>(&storage);
    addr_in6->sin6_port = actualPort;
  }
}

int IpAddress::getFamily() const {
  if (addressType == Type::IPv4) {
    return AF_INET;
  }
  if (addressType == Type::IPv6) {
    return AF_INET6;
  }

  throw AF_UNSPEC;
}

std::optional<IpAddress> IpAddress::fromString(const std::string& addrStr) {
  struct sockaddr_in ipv4Addr {};
  auto* sockAddrv4 = reinterpret_cast<sockaddr*>(&ipv4Addr);

  struct sockaddr_in6 ipv6Addr {};
  auto* sockAddrv6 = reinterpret_cast<sockaddr*>(&ipv6Addr);

  // Try to parse IPv4 address
  if (inet_pton(AF_INET, addrStr.c_str(), &(ipv4Addr.sin_addr)) == 1) {
    sockAddrv4->sa_family = AF_INET;
    return IpAddress(sockAddrv4, addrStr);
  }

  // Try to parse IPv6 address
  if (inet_pton(AF_INET6, addrStr.c_str(), &(ipv6Addr.sin6_addr)) == 1) {
    sockAddrv6->sa_family = AF_INET6;
    return IpAddress(sockAddrv6, addrStr);
  }

  return std::nullopt;
}

std::optional<uint16_t> IpAddress::getPort() const {
  if (addressType == Type::IPv4) {
    const auto* addr_in = reinterpret_cast<const sockaddr_in*>(&storage);
    return ntohs(addr_in->sin_port);
  }

  if (addressType == Type::IPv6) {
    const auto* addr_in = reinterpret_cast<const sockaddr_in6*>(&storage);
    return ntohs(addr_in->sin6_port);
  }

  return std::nullopt;
}

std::optional<std::string> IpAddress::getOriginalHost() const {
  return originalHost;
}

bell::Result<IpAddress> IpAddress::resolveDomain(const std::string& hostname,
                                                 int sockType, int family) {
  if (hostname.empty()) {
    // Addr any on empty hostname
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    return {reinterpret_cast<sockaddr*>(&addr)};
  }

  auto possibleAddress = fromString(hostname);

  // First, check if hostname is already an IP address (IPv4 or IPv6)
  if (possibleAddress.has_value()) {
    return possibleAddress.value();
  }

  // Otherwise, treat it as a domain name and resolve it
  struct addrinfo hints {};
  struct addrinfo* res = nullptr;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = family;      // Allow both IPv4 and IPv6
  hints.ai_socktype = sockType;  // TCP (SOCK_STREAM) or UDP (SOCK_DGRAM)

  int result = getaddrinfo(hostname.c_str(), nullptr, &hints, &res);
  if (result != 0) {
    return tl::make_unexpected(std::error_code(result, std::system_category()));
  }

  // We'll use the first valid result
  IpAddress resolved(res->ai_addr, hostname);

  // Clean up
  freeaddrinfo(res);

  return resolved;
}
