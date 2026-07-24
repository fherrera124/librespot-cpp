#include <doctest/doctest.h>

#include <unistd.h>
#include "bell/net/IpAddress.h"

TEST_CASE("bell::net::IpAddress tests") {
  SUBCASE("resolveDomain properly resolves domains") {
    auto result = bell::net::IpAddress::resolveDomain("localhost", SOCK_STREAM);

    // Localhost should resolve to either IPv4 or IPv6
    REQUIRE(result.has_value());

    REQUIRE((result->getType() != bell::net::IpAddress::Type::Unknown));
    REQUIRE(result->getSockAddrLen() > 0);

    // Requesting IPv4 should return IPv4
    result =
        bell::net::IpAddress::resolveDomain("localhost", SOCK_STREAM, AF_INET);
    REQUIRE(result.has_value());

    // Should be IPv4
    REQUIRE((result->getType() == bell::net::IpAddress::Type::IPv4));

    // Resolving an invalid domain should fail
    result = bell::net::IpAddress::resolveDomain("_invalid.domai", SOCK_STREAM);

    REQUIRE(!result);
  }

  SUBCASE("resolveDomain properly copies IP addresses") {
    auto result = bell::net::IpAddress::resolveDomain("127.0.0.1", SOCK_STREAM);

    REQUIRE(result.has_value());

    // Should be IPv4
    REQUIRE(result->getType() == bell::net::IpAddress::Type::IPv4);
    REQUIRE(result->getSockAddrLen() > 0);

    result = bell::net::IpAddress::resolveDomain("124.1.", SOCK_STREAM);
    REQUIRE(!result);

    // Resolves IPv6
    result = bell::net::IpAddress::resolveDomain("::1", SOCK_STREAM);

    REQUIRE(result.has_value());

    REQUIRE(result->getType() == bell::net::IpAddress::Type::IPv6);
  }
}
