#pragma once

#include <algorithm>
#include <climits>
#include <random>
#include <string>
#include "Utils.h"
#include "authentication.pb.h"
#include "proto/AuthenticationPb.h"
#include "tao/json.hpp"

namespace {
// 20 random bytes, hex-encoded (40 chars) - matches go-librespot's device id
// generation exactly (daemon/app.go: crypto/rand.Read into a 20-byte buffer,
// then hex.EncodeToString), real per-installation entropy rather than a
// hash of a fixed compile-time string (the old deviceIdPrefix + hash(
// deviceName) scheme produced the *same* id for every build of this
// firmware, since deviceName was itself a hardcoded literal - zero entropy,
// not just "stable"). main()'s session-file load
// (AuthInfo::assignDataFromJson()) overwrites this with the persisted value
// on every run after the first, so this generator only actually matters
// once per device's lifetime.
std::string generateDeviceId() {
  static std::independent_bits_engine<std::default_random_engine, CHAR_BIT,
                                      unsigned char>
      randomEngine{std::default_random_engine(std::random_device{}())};
  static const char* hexDigits = "0123456789abcdef";
  std::string id;
  id.reserve(40);
  std::generate_n(std::back_inserter(id), 40,
                  [] { return hexDigits[randomEngine() % 16]; });
  return id;
}
}  // namespace

namespace cspot {
struct AuthInfo {
  AuthInfo() = default;
  AuthInfo(const std::string& deviceName)
      : deviceName(deviceName), deviceId(generateDeviceId()) {}
  std::string deviceName;
  std::string deviceId;
  std::string sessionId;

  std::optional<cspot_proto::LoginCredentials> loginCredentials;

  inline std::string toJson() const {
    tao::json::value json;
    json["deviceName"] = deviceName;
    json["deviceId"] = deviceId;
    if (loginCredentials.has_value()) {
      json["username"] = loginCredentials->username;
      std::cout << "Encoding blob of size " << loginCredentials->authData.size()
                << std::endl;
      json["blob"] = base64Encode(loginCredentials->authData.data(),
                                  loginCredentials->authData.size());
      json["authType"] = static_cast<int>(loginCredentials->type);
    }
    return tao::json::to_string(json);
  }

  inline void assignDataFromJson(const std::string& jsonString) {
    auto json = tao::json::from_string(jsonString);
    deviceName = json.at("deviceName").get_string();
    deviceId = json.at("deviceId").get_string();
    auto username = json.optional<std::string>("username");
    auto blob = json.optional<std::string>("blob");
    auto authType = json.optional<int>("authType");
    if (username.has_value() && blob.has_value() && authType.has_value()) {
      cspot_proto::LoginCredentials credentials;
      credentials.username = *username;
      credentials.type = static_cast<AuthenticationType>(*authType);
      auto decodedBlob = base64Decode(*blob);
      credentials.authData.insert(credentials.authData.end(),
                                  decodedBlob.begin(), decodedBlob.end());
      loginCredentials = credentials;
    }
  }
};
}  // namespace cspot
