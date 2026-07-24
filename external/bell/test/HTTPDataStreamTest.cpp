#include <doctest/doctest.h>
#include <trompeloeil.hpp>

// Code under test
#include "bell/http/DataStream.h"
#include "mocks/MockTransport.h"

TEST_CASE("bell::http::DataStream") {

  using trompeloeil::_;  // wild card for matching any value

  // A unique_ptr to our mock transport, which will be injected into the Client
  auto mockTransport = std::make_unique<MockTransport>();
  // We need a raw pointer to set expectations before the unique_ptr is moved
  // auto* rawMockTransport = mockTransport.get();

  // The Client instance under test, configured with our mock transport
  bell::http::Client client(std::move(mockTransport));

  SUBCASE("Properly reads content range") {
  }
}
