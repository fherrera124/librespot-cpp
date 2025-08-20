#include <doctest/doctest.h>
#include <trompeloeil.hpp>

#include "api/CredentialsResolver.h"
#include "mocks/MockHTTPTransport.h"
#include "tao/json.hpp"

TEST_CASE("CredentialsResolver tests") {
  using trompeloeil::_;  // wild card for matching any value

  auto mockTransport = std::make_unique<MockHTTPTransport>();
  auto* rawMockTransport = mockTransport.get();

  auto mockAuthInfo = std::make_shared<cspot::AuthInfo>();
  auto resolver = cspot::createDefaultCredentialsResolver(
      std::make_shared<bell::HTTPClient>(std::move(mockTransport)),
      mockAuthInfo);

  SUBCASE("getApAddress") {
    const std::string url =
        "https://apresolve.spotify.com/"
        "?type=spclient&type=dealer-g2&type=accesspoint";

    const std::string ap1 = "ap1.spotify.com:4070";
    const std::string dealer1 = "dealer1.spotify.com:443";
    const std::string spclient1 = "spclient1.spotify.com:443";

    tao::json::value responseJSON = {
        {"accesspoint", tao::json::value::array({ap1})},
        {"dealer-g2", tao::json::value::array({dealer1})},
        {"spclient", tao::json::value::array({spclient1})}};
    const std::string responseBody = tao::json::to_string(responseJSON);

    SUBCASE("should fetch and cache addresses on the first call") {
      REQUIRE_CALL(*rawMockTransport, execute(_))
          .WITH([](const bell::http::Request& req) {
            return req.method == bell::HTTPMethod::GET &&
                   req.uri.host == "apresolve.spotify.com";
          })
          .RETURN(createMockResponse(200, responseBody, "application/json"));

      // Call for the first type, this should trigger the fetch
      auto apRes = resolver->getApAddress(
          cspot::CredentialsResolver::AddressType::AccessPoint);
      CHECK(apRes.has_value());
      CHECK(*apRes == ap1);

      // Subsequent calls should hit the cache (no new HTTP call)
      auto dealerRes = resolver->getApAddress(
          cspot::CredentialsResolver::AddressType::Dealer);
      CHECK(dealerRes.has_value());
      CHECK(*dealerRes == dealer1);
    }
  }
}
