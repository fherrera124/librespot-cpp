#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "api/SpClient.h"
#include "tao/json.hpp"

namespace cspot::playplay_service {

inline std::string hex(const std::vector<std::byte>& bytes) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  result.reserve(bytes.size() * 2);
  for (std::byte byte : bytes) {
    const auto value = std::to_integer<unsigned>(byte);
    result.push_back(digits[value >> 4]);
    result.push_back(digits[value & 15]);
  }
  return result;
}

inline std::optional<std::string> requestBody(const PlayPlayLicense& license) {
  if (license.obfuscatedKey.size() != 16 || license.b4Seq.size() != 4) {
    return std::nullopt;
  }
  return tao::json::to_string(tao::json::value{
      {"obfuscated_key", hex(license.obfuscatedKey)},
      {"b4_seq", hex(license.b4Seq)}});
}

inline int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

inline std::optional<std::vector<std::byte>> parseResponse(
    std::string_view body) {
  try {
    const auto json = tao::json::from_string(body);
    if (!json.is_object()) return std::nullopt;
    const auto* success = json.find("success");
    const auto* aesKey = json.find("aes_key");
    if (!success || !success->is_boolean() || !success->get_boolean() ||
        !aesKey || !aesKey->is_string()) {
      return std::nullopt;
    }
    const auto& keyHex = aesKey->get_string();
    if (keyHex.size() != 32) return std::nullopt;
    std::vector<std::byte> key;
    key.reserve(16);
    for (size_t i = 0; i < keyHex.size(); i += 2) {
      const int hi = hexDigit(keyHex[i]);
      const int lo = hexDigit(keyHex[i + 1]);
      if (hi < 0 || lo < 0) return std::nullopt;
      key.push_back(static_cast<std::byte>((hi << 4) | lo));
    }
    return key;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

}  // namespace cspot::playplay_service
