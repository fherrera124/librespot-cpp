#include <doctest/doctest.h>

#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "api/SpClient.h"
#include "bell/http/Reader.h"
#include "proto/ConnectPb.h"

namespace {
class FixedCredentials : public cspot::CredentialsResolver {
 public:
  bell::Result<std::string> getApAddress(AddressType, sysclock_timepoint) override {
    return std::string("example.invalid");
  }
  bell::Result<std::string> getClientToken(sysclock_timepoint) override {
    return std::string("test-client-token");
  }
  bell::Result<std::string> getAccessKey(sysclock_timepoint) override {
    return std::string("test-access-key");
  }
};

// Responses borrow this stream; the transport outlives each request. The
// external reader buffer lets the test detect accidental body buffering.
class ResponseTransport : public bell::http::Transport {
 public:
  explicit ResponseTransport(const std::string& wire) : input(wire) {}

  bell::Result<bell::HTTPResponse> execute(const bell::HTTPRequest&) override {
    bell::http::Reader reader(bell::http::Direction::Response, &input,
                              &readerBuffer);
    auto res = reader.readHeaders();
    if (!res) {
      return nonstd::make_unexpected(res.error());
    }
    return bell::HTTPResponse(std::move(reader));
  }

  std::istringstream input;
  std::vector<char> readerBuffer;
};

struct ClientFixture {
  explicit ClientFixture(const std::string& wire) {
    auto transport = std::make_unique<ResponseTransport>(wire);
    response = transport.get();
    client = cspot::createDefaultSpClient(
        std::make_shared<bell::HTTPClient>(std::move(transport)),
        std::make_shared<FixedCredentials>());
  }

  ResponseTransport* response = nullptr;
  std::unique_ptr<cspot::SpClient> client;
};

std::string responseHeaders(int status, size_t length) {
  return "HTTP/1.1 " + std::to_string(status) +
         " Test\r\nContent-Length: " + std::to_string(length) + "\r\n\r\n";
}
}  // namespace

TEST_CASE("State PUT drains large responses without retaining their bodies") {
  // Not a multiple of the drain buffer: exercises the final partial block.
  const std::string body(64 * 1024 + 17, 'x');
  const std::string first = responseHeaders(200, body.size()) + body;
  ClientFixture fixture(first + responseHeaders(200, 4) + "next");

  REQUIRE(fixture.client->putConnectStateRaw({}, "device", "session"));
  CHECK(fixture.response->input.tellg() ==
        static_cast<std::streamoff>(first.size()));
  CHECK(fixture.response->readerBuffer.capacity() < 1024);

  // A second response must start at its own status line, not in the first
  // response's body. No body bytes may be left unread or consumed ahead.
  REQUIRE(fixture.client->putConnectStateRaw({}, "device", "session"));
  CHECK(fixture.response->readerBuffer.capacity() < 1024);
}

TEST_CASE("State PUT accepts empty and close-delimited response bodies") {
  std::string wire;
  SUBCASE("empty Content-Length") {
    wire = responseHeaders(200, 0);
  }
  SUBCASE("body ends at connection EOF") {
    wire = "HTTP/1.1 200 Test\r\nConnection: close\r\n\r\n" +
           std::string(4097, 'x');
  }
  ClientFixture fixture(wire);
  CHECK(fixture.client->putConnectStateRaw({}, "device", "session"));
  CHECK(fixture.response->readerBuffer.capacity() < 1024);
}

TEST_CASE("Both PUT endpoints drain error responses before returning failure") {
  const std::string body(4097, 'x');
  const std::string wire = responseHeaders(400, body.size()) + body;
  ClientFixture fixture(wire);
  SUBCASE("state") {
    auto res = fixture.client->putConnectStateRaw({}, "device", "session");
    REQUIRE_FALSE(res);
    CHECK(res.error() == std::errc::bad_message);
  }
  SUBCASE("inactive") {
    auto res = fixture.client->putInactive("device", "session");
    REQUIRE_FALSE(res);
    CHECK(res.error() == std::errc::bad_message);
  }
  CHECK(fixture.response->input.tellg() ==
        static_cast<std::streamoff>(wire.size()));
  CHECK(fixture.response->readerBuffer.capacity() < 1024);
}

TEST_CASE("Both PUT endpoints report truncated response bodies") {
  // Include an HTTP error too: body read errors must not be mistaken for
  // successfully draining that response and checking only its status.
  int status = 200;
  SUBCASE("success status") { status = 200; }
  SUBCASE("error status") { status = 400; }
  const std::string wire = responseHeaders(status, 2048) + std::string(513, 'x');
  ClientFixture state(wire);
  ClientFixture inactive(wire);
  auto stateRes = state.client->putConnectStateRaw({}, "device", "session");
  auto inactiveRes = inactive.client->putInactive("device", "session");
  REQUIRE_FALSE(stateRes);
  REQUIRE_FALSE(inactiveRes);
  CHECK(stateRes.error() == bell::http::Errc::IncompleteMessage);
  CHECK(inactiveRes.error() == bell::http::Errc::IncompleteMessage);
}

TEST_CASE("Inactive PUT accepts an empty 204 response") {
  ClientFixture fixture(responseHeaders(204, 0));
  CHECK(fixture.client->putInactive("device", "session"));
}

TEST_CASE("Protobuf repeated strings preserve empty, long and binary values") {
  cspot_proto::Suppressions input;
  input.providers = {"test-provider", "", std::string(4096, 'x'),
                     std::string("a\0b", 3), "last"};
  std::vector<std::byte> encoded;
  REQUIRE(nanopb_helper::encodeToVector(input, encoded));

  cspot_proto::Suppressions output;
  REQUIRE(nanopb_helper::decodeFromVector(output, encoded));
  CHECK(output.providers == input.providers);
}

TEST_CASE("Protobuf repeated strings reject a truncated element") {
  cspot_proto::Suppressions input;
  input.providers = {"complete", std::string(64, 'x')};
  std::vector<std::byte> encoded;
  REQUIRE(nanopb_helper::encodeToVector(input, encoded));
  encoded.pop_back();

  cspot_proto::Suppressions output;
  CHECK_FALSE(nanopb_helper::decodeFromVector(output, encoded));
  REQUIRE(output.providers.size() == 1);
  CHECK(output.providers.front() == "complete");
}

TEST_CASE("Protobuf repeated strings roundtrip inside a cluster update") {
  cspot_proto::ClusterUpdate input{};
  input.cluster.playerState.hasValue = true;
  input.cluster.playerState.value.suppressions.providers = {"queue", "context"};
  std::vector<std::byte> encoded;
  REQUIRE(nanopb_helper::encodeToVector(input, encoded));

  cspot_proto::ClusterUpdate output{};
  REQUIRE(nanopb_helper::decodeFromVector(output, encoded));
  REQUIRE(output.cluster.playerState.hasValue);
  CHECK(output.cluster.playerState.value.suppressions.providers ==
        input.cluster.playerState.value.suppressions.providers);
}
