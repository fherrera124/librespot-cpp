#include <doctest/doctest.h>

#include <unistd.h>

#include <sys/socket.h>
#include "bell/net/URIParser.h"

TEST_CASE("bell::io::URIParser tests") {
  SUBCASE("Valid URIs") {
    std::string uri = "http://www.example.com";
    // Parse first URI
    auto result = bell::net::parseURI(uri);
    REQUIRE(result.has_value());
    REQUIRE(result->port.has_value() == false);
    REQUIRE(result->query.has_value() == false);

    REQUIRE(result->scheme.value() == "http");
    REQUIRE(result->host.value() == "www.example.com");
    REQUIRE(result->path.value() == "/");

    uri = "https://www.example.com:443/path/to/resource?query=value";
    // Parse second URI
    result = bell::net::parseURI(uri);
    REQUIRE(result.has_value());
    REQUIRE(result->port.value() == 443);

    REQUIRE(result->scheme.value() == "https");
    REQUIRE(result->host.value() == "www.example.com");
    REQUIRE(result->path.value() == "/path/to/resource");
    REQUIRE(result->query.value() == "query=value");

    // Parse third URI
    result = bell::net::parseURI("ftp://ftp.example.com:21/resources");
    REQUIRE(result.has_value());
    REQUIRE(result->port.value() == 21);
    REQUIRE(result->query.has_value() == false);

    REQUIRE(result->scheme.value() == "ftp");
    REQUIRE(result->host.value() == "ftp.example.com");
    REQUIRE(result->path.value() == "/resources");
  }

  SUBCASE("Invalid URIs") {
    auto result = bell::net::parseURI("://www.example.com");  // Missing scheme
    REQUIRE(result.has_value() == false);

    result = bell::net::parseURI("http://:80");  // Missing host
    REQUIRE(result.has_value() == false);

    result = bell::net::parseURI("http://www.example.com:abc");  // Invalid port
    REQUIRE(result.has_value() == false);

    result = bell::net::parseURI("http://www.example.com?");  // Empty query
    REQUIRE(result.has_value() == false);
  }
}

TEST_CASE("bell::uri::encode tests") {
  SUBCASE("Unreserved Characters") {
    std::string input =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.~";
    std::string expected =
        input;  // Unreserved characters should not be encoded
    REQUIRE(bell::net::encodeURLEncoded(input) == expected);
  }

  SUBCASE("Reserved Characters") {
    std::string input = "!*'();:@&=+$,/?#[]";
    std::string expected =
        "%21%2A%27%28%29%3B%3A%40%26%3D%2B%24%2C%2F%3F%23%5B%5D";
    REQUIRE(bell::net::encodeURLEncoded(input) == expected);
  }

  SUBCASE("Mixed Characters") {
    std::string input = "Hello, World!";
    std::string expected = "Hello%2C%20World%21";
    REQUIRE(bell::net::encodeURLEncoded(input) == expected);
  }

  SUBCASE("Non-ASCII Characters") {
    std::string input = "こんにちは";  // "Hello" in Japanese
    // Percent-encode the UTF-8 bytes
    std::string expected = "%E3%81%93%E3%82%93%E3%81%AB%E3%81%A1%E3%81%AF";
    REQUIRE(bell::net::encodeURLEncoded(input) == expected);
  }

  SUBCASE("Empty String") {
    std::string input;
    std::string expected;
    REQUIRE(bell::net::encodeURLEncoded(input) == expected);
  }
}

TEST_CASE("bell::uri::decode tests") {
  SUBCASE("Encoded Unreserved Characters") {
    std::string input =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.~";
    std::string expected = input;
    // Even if unreserved characters are percent-encoded, they should bell::uri::decode back to original
    std::string encoded = bell::net::encodeURLEncoded(input);
    REQUIRE(bell::net::decodeURLEncoded(encoded) == expected);
  }

  SUBCASE("Encoded Reserved Characters") {
    std::string input = "!*'();:@&=+$,/?#[]";
    std::string encoded =
        "%21%2A%27%28%29%3B%3A%40%26%3D%2B%24%2C%2F%3F%23%5B%5D";
    REQUIRE(bell::net::decodeURLEncoded(encoded) == input);
  }

  SUBCASE("Mixed Characters") {
    std::string input = "Hello%2C%20World%21";
    std::string expected = "Hello, World!";
    REQUIRE(bell::net::decodeURLEncoded(input) == expected);
  }

  SUBCASE("Non-ASCII Characters") {
    std::string input = "%E3%81%93%E3%82%93%E3%81%AB%E3%81%A1%E3%81%AF";
    std::string expected = "こんにちは";  // "Hello" in Japanese
    REQUIRE(bell::net::decodeURLEncoded(input) == expected);
  }

  SUBCASE("Empty String") {
    std::string input;
    std::string expected;
    REQUIRE(bell::net::decodeURLEncoded(input) == expected);
  }

  SUBCASE("Invalid Encoded Percent") {
    std::string input = "Invalid%2GInput";
    // Depending on your implementation, decide how to handle invalid sequences
    // For this test, we'll expect the '%' and following characters to be left as is
    std::string expected = "Invalid%2GInput";
    REQUIRE(bell::net::decodeURLEncoded(input) == expected);
  }

  SUBCASE("Incomplete Percent Encoding") {
    std::string input1 = "Incomplete%";
    std::string expected1 = "Incomplete%";
    REQUIRE(bell::net::decodeURLEncoded(input1) == expected1);

    std::string input2 = "Incomplete%2";
    std::string expected2 = "Incomplete%2";
    REQUIRE(bell::net::decodeURLEncoded(input2) == expected2);
  }

  SUBCASE("Plus Character") {
    std::string input = "A+plus+B";  // '+' should remain unchanged
    std::string expected = "A+plus+B";
    REQUIRE(bell::net::decodeURLEncoded(input) == expected);
  }

  SUBCASE("Encode-bell::uri::decode Round Trip") {
    std::string original =
        "Test String with special characters !*'();:@&=+$,/?#[]";
    std::string encoded = bell::net::encodeURLEncoded(original);
    std::string decoded = bell::net::decodeURLEncoded(encoded);
    REQUIRE(decoded == original);
  }

  SUBCASE("Upper and Lowercase Hex Digits") {
    std::string inputUpper = "%7E";
    std::string inputLower = "%7e";
    std::string expected = "~";
    REQUIRE(bell::net::decodeURLEncoded(inputUpper) == expected);
    REQUIRE(bell::net::decodeURLEncoded(inputLower) == expected);
  }
}
