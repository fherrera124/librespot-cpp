#include <doctest/doctest.h>
#include <trompeloeil.hpp>

// Code under test
#include "bell/http/Client.h"
#include "mocks/MockTransport.h"

TEST_CASE("bell::http::Client tests") {
  using trompeloeil::_;  // wild card for matching any value

  // A unique_ptr to our mock transport, which will be injected into the Client
  auto mockTransport = std::make_unique<MockTransport>();
  // We need a raw pointer to set expectations before the unique_ptr is moved
  auto* rawMockTransport = mockTransport.get();

  // The Client instance under test, configured with our mock transport
  bell::http::Client client(std::move(mockTransport));

  SUBCASE("Successful GET request") {
    const std::string url = "http://example.com/api/resource";
    const std::string responseBody = "{\"id\": 1, \"name\": \"Test\"}";

    // Set expectation: The 'execute' method must be called exactly once.
    REQUIRE_CALL(*rawMockTransport, execute(_))
        .WITH([](const bell::http::Request& req) {
          return req.method == bell::HTTPMethod::GET &&
                 req.uri.path == "/api/resource" &&
                 req.uri.host == "example.com";
        })
        .RETURN(createMockResponse(200, responseBody));

    // Perform the action
    auto response = client.get(url);

    // Check the results
    REQUIRE(response);
    CHECK(response->statusCode == 200);
    auto textResult = response->text();
    REQUIRE(textResult);
    CHECK(textResult == responseBody);
  }

  SUBCASE("Successful POST request with custom headers") {
    const std::string url = "https://api.server.net/v1/data";
    const std::string requestBodyStr = "{\"data\":\"payload\"}";
    std::vector<std::byte> requestBody(
        reinterpret_cast<const std::byte*>(requestBodyStr.data()),
        reinterpret_cast<const std::byte*>(requestBodyStr.data()) +
            requestBodyStr.size());
    const std::string responseBody = "{\"status\":\"created\"}";

    // Set expectation for the execute method call
    REQUIRE_CALL(*rawMockTransport, execute(_))
        .WITH([](const bell::http::Request& req) {
          return req.method == bell::HTTPMethod::POST &&
                 req.uri.path == "/v1/data" &&
                 req.headers.count("Authorization") == 1 &&
                 req.headers.at("Authorization") == "Bearer my-token";
        })
        .RETURN(createMockResponse(201, responseBody));

    // Perform the action with custom headers
    bell::HTTPHeaders headers = {{"Authorization", "Bearer my-token"}};
    auto result = client.post(url, headers, requestBody);

    // Check the results
    REQUIRE(result);
    CHECK(result->statusCode == 201);
  }

  SUBCASE("Request with an invalid URL") {
    // We expect that an invalid URL will cause Request::create to fail,
    // so the transport's 'execute' method should NEVER be called.
    const std::string invalidUrl = "dupa-dupa-duupa";

    // Perform the action
    auto result = client.get(invalidUrl);

    // Check that the operation failed as expected
    REQUIRE(!result);
  }
}
