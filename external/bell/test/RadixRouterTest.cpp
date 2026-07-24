#include <doctest/doctest.h>

// Code under test
#include "bell/http/RadixRouter.h"

using MockHandlerType = int;

TEST_CASE("bell::http::RadixRouter tests") {
  bell::http::RadixRouter<MockHandlerType> router;

  SUBCASE("Inserts and finds exact match routes") {
    int mockHandlerVal = 5;
    router.insert(bell::http::Method::GET, "/users", mockHandlerVal);

    auto result = router.find(bell::http::Method::GET, "/users");
    REQUIRE(result.has_value());
    REQUIRE(result->first == mockHandlerVal);
  }

  SUBCASE("Handles routes with parameters") {
    int mockHandlerVal = 5;
    router.insert(bell::http::Method::GET, "/users/:id", mockHandlerVal);

    auto result = router.find(bell::http::Method::GET, "/users/123");
    REQUIRE(result.has_value());
    REQUIRE(result->second["id"] == "123");
    REQUIRE(result->first == mockHandlerVal);
  }

  SUBCASE("Handles catch-all routes") {
    int mockHandlerVal = 5;
    router.insert(bell::http::Method::GET, "/users/*", mockHandlerVal);

    auto result = router.find(bell::http::Method::GET, "/users/123");
    REQUIRE(result.has_value());
    REQUIRE(result->second["**"] == "123");
    REQUIRE(result->first == mockHandlerVal);
  }

  SUBCASE("Handles routes with multiple methods") {
    int mockGetHandlerVal = 5;
    int mockPostHandlerVal = 6;

    router.insert(bell::http::Method::GET, "/users", mockGetHandlerVal);
    router.insert(bell::http::Method::POST, "/users", mockPostHandlerVal);

    auto result = router.find(bell::http::Method::GET, "/users");
    REQUIRE(result.has_value());
    REQUIRE(result->first == mockGetHandlerVal);

    result = router.find(bell::http::Method::POST, "/users");
    REQUIRE(result.has_value());
    REQUIRE(result->first == mockPostHandlerVal);

    // Doesnt find a route for PUT
    result = router.find(bell::http::Method::PUT, "/users");
    REQUIRE_FALSE(result.has_value());
  }
}
