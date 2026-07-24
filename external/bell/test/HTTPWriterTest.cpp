#include <doctest/doctest.h>

#include <iostream>
#include <sstream>

// Code under test
#include "bell/http/Common.h"
#include "bell/http/Writer.h"

TEST_CASE("bell::http::Writer tests") {
  SUBCASE("Writes a simple GET request") {
    std::stringstream ss;
    bell::http::Writer writer(bell::http::Direction::Request, &ss);

    // Should not allow a response-specific method
    REQUIRE(!writer.setStatusCode(200));

    // Write the request using the helper method
    REQUIRE(writer.writeRequest(bell::http::Method::GET, "/index.html"));

    std::string expected =
        "GET /index.html HTTP/1.1\r\nhost: localhost\r\nuser-agent: "
        "bell/1.0\r\n\r\n";
    REQUIRE(ss.str() == expected);

    // Should not be able to write another request
    REQUIRE(!writer.writeHeaders());

    // Should not be able to change path after writing headers
    REQUIRE(!writer.setPath("asdfasdf"));

    // Will not allow writing a body with the unspecified content length
    REQUIRE(!writer.writeBodyStringView("Hello, world!"));
  }

  SUBCASE("Writes a simple POST request") {
    std::stringstream ss;
    bell::http::Writer writer(bell::http::Direction::Request, &ss);

    std::string body = "Hello, world!";

    // Write the request using the helper method
    REQUIRE(writer.writeRequest(bell::http::Method::POST, "/post",
                        {{"content-type", "text/plain"}}, body.size()));

    REQUIRE(writer.writeBodyStringView(body));

    std::string expected =
        "POST /post HTTP/1.1\r\ncontent-type: text/plain\r\nhost: "
        "localhost\r\nuser-agent: bell/1.0\r\ncontent-length: 13\r\n\r\nHello, "
        "world!";
    REQUIRE(ss.str() == expected);
  }

  SUBCASE("Writes a POST request with a long header") {
    std::stringstream ss;
    bell::http::Writer writer(bell::http::Direction::Request, &ss);

    std::string body = "Hello, world!";

    // Write the request using the helper method
    REQUIRE(writer.writeRequest(
        bell::http::Method::POST, "/post",
        {{"content-type", "text/plain"},
         {"client-token",
          "qtmgeqnwjxxksgxzjnviiijcmfbvdvcgrrqakcuaeczenjenksrtevlpubwoaznhvjaq"
          "kmfjidccvzhvolnusudvggbrcnvkoldzunkacxuznnuyljcnxchxjdnrqrtncytfbywc"
          "zptqcpgrblmdwcebdlmlmpkdizshyqyhtbnuzwvpmffmmzrssqwtzjluqsxvhdvtmnut"
          "hiehztmmalpqesbbosisugmgsznzwjqlzxlotvzprobunfdpvhnugmzqxwsbfkwfouk"
          "d"}},
        body.size()));

    REQUIRE(writer.writeBodyStringView(body));

    std::string expected =
        "POST /post HTTP/1.1\r\nclient-token: "
        "qtmgeqnwjxxksgxzjnviiijcmfbvdvcgrrqakcuaeczenjenksrtevlpubwoaznhvjaqkm"
        "fjidccvzhvolnusudvggbrcnvkoldzunkacxuznnuyljcnxchxjdnrqrtncytfbywczptq"
        "cpgrblmdwcebdlmlmpkdizshyqyhtbnuzwvpmffmmzrssqwtzjluqsxvhdvtmnuthiehzt"
        "mmalpqesbbosisugmgsznzwjqlzxlotvzprobunfdpvhnugmzqxwsbfkwfoukd\r\ncont"
        "ent-type: text/plain\r\nhost: "
        "localhost\r\nuser-agent: bell/1.0\r\ncontent-length: 13\r\n\r\nHello, "
        "world!";
    REQUIRE(ss.str() == expected);
  }

  SUBCASE("Writes a simple GET response") {
    std::stringstream ss;
    bell::http::Writer writer(bell::http::Direction::Response, &ss);

    // Should not allow a request-specific method
    REQUIRE(!writer.setMethod(bell::http::Method::GET));

    // Write the request using the helper method
    REQUIRE(writer.writeResponse(200));

    std::string expected = "HTTP/1.1 200 OK\r\n\r\n";
    REQUIRE(ss.str() == expected);

    // Should not be able to write another response
    REQUIRE(!writer.writeHeaders());

    // Should not be able to change the status code after writing headers
    REQUIRE(!writer.setStatusCode(404));

    // Will not allow writing a body with the unspecified content length
    REQUIRE(!writer.writeBodyStringView("Hello, world!"));
  }

  SUBCASE("Writes a simple POST response") {
    std::stringstream ss;
    bell::http::Writer writer(bell::http::Direction::Response, &ss);

    // Should not allow a request-specific method
    REQUIRE(!writer.setMethod(bell::http::Method::POST));

    // Write the request using the helper method
    (void)writer.writeResponseWithBody(500, {}, "Hello, world!");

    std::string expected =
        "HTTP/1.1 500 Internal Server Error\r\ncontent-type: "
        "text/html\r\ncontent-length: 13\r\n\r\nHello, world!";
    REQUIRE(ss.str() == expected);

    // Should not be able to write another response
    REQUIRE(!writer.writeHeaders());
  }
}
