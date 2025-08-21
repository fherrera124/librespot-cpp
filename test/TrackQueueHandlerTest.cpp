#include <doctest/doctest.h>
#include <trompeloeil.hpp>

#include "mocks/MockSpClient.h"

TEST_CASE("ContextTrackProvider tests") {
  using trompeloeil::_;  // wild card for matching any value

  auto mockSpClient = std::make_shared<MockSpClient>();
}
