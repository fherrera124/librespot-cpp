#include <doctest/doctest.h>

#include <cstddef>
#include <string>
#include <vector>

#include "api/PlayPlayService.h"
#include "tao/json.hpp"

TEST_CASE("PlayPlay service request carries both binary fields as hex") {
  cspot::PlayPlayLicense license;
  license.obfuscatedKey = std::vector<std::byte>(16, std::byte{0xab});
  license.b4Seq = {std::byte{0x00}, std::byte{0x01}, std::byte{0xfe},
                   std::byte{0xff}};

  auto body = cspot::playplay_service::requestBody(license);
  REQUIRE(body);
  auto json = tao::json::from_string(*body);
  CHECK(json.at("obfuscated_key").get_string() ==
        "abababababababababababababababab");
  CHECK(json.at("b4_seq").get_string() == "0001feff");

  license.b4Seq.pop_back();
  CHECK_FALSE(cspot::playplay_service::requestBody(license));
}

TEST_CASE("PlayPlay service response requires successful JSON and AES16 hex") {
  const std::string key = "00112233445566778899aabbccddeeff";
  auto parsed = cspot::playplay_service::parseResponse(
      "{\"success\":true,\"aes_key\":\"" + key + "\"}");
  REQUIRE(parsed);
  REQUIRE(parsed->size() == 16);
  CHECK((*parsed)[0] == std::byte{0x00});
  CHECK((*parsed)[15] == std::byte{0xff});

  const std::vector<std::string> invalid = {
      "{\"success\":false,\"aes_key\":\"" + key + "\"}",
      "{\"success\":1,\"aes_key\":\"" + key + "\"}",
      "{\"aes_key\":\"" + key + "\"}",
      "{\"success\":true,\"aes_key\":\"001122\"}",
      "{\"success\":true,\"aes_key\":\"00112233445566778899aabbccddeezz\"}",
      "{\"success\":true,\"aes_key\":32}",
      "{\"success\":true,\"aes_key\":\"" + key + "\"",
      "not JSON"};
  for (const auto& body : invalid) {
    CHECK_FALSE(cspot::playplay_service::parseResponse(body));
  }
}
