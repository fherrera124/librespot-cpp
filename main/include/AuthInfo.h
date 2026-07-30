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

  // False until assignDataFromJson() overwrites deviceId with a value
  // loaded from a persisted session - lets logDeviceIdOrigin() below tell
  // a freshly-generated device id apart from a reused/persisted one.
  bool deviceIdWasPersisted = false;

  inline std::string toJson() const {
    tao::json::value json;
    json["deviceName"] = deviceName;
    json["deviceId"] = deviceId;
    if (loginCredentials.has_value()) {
      auto encodedBlob = base64Encode(loginCredentials->authData.data(),
                                      loginCredentials->authData.size());
      if (encodedBlob) {
        json["username"] = loginCredentials->username;
        json["blob"] = *encodedBlob;
        json["authType"] = static_cast<int>(loginCredentials->type);
      } else {
        BELL_LOG(error, "AuthInfo",
                 "Failed to base64-encode credentials blob for persistence");
      }
    }
    return tao::json::to_string(json);
  }

  inline void assignDataFromJson(const std::string& jsonString) {
    auto json = tao::json::from_string(jsonString);
    deviceName = json.at("deviceName").get_string();
    deviceId = json.at("deviceId").get_string();
    deviceIdWasPersisted = true;
    auto username = json.optional<std::string>("username");
    auto blob = json.optional<std::string>("blob");
    auto authType = json.optional<int>("authType");
    if (username.has_value() && blob.has_value() && authType.has_value()) {
      auto decodedBlob = base64Decode(*blob);
      if (!decodedBlob) {
        BELL_LOG(error, "AuthInfo",
                 "Failed to base64-decode persisted credentials blob - "
                 "ignoring");
        return;
      }
      cspot_proto::LoginCredentials credentials;
      credentials.username = *username;
      credentials.type = static_cast<AuthenticationType>(*authType);
      credentials.authData.insert(credentials.authData.end(),
                                  decodedBlob->begin(), decodedBlob->end());
      loginCredentials = credentials;
    }
  }

  inline void logDeviceIdOrigin() const {
    if (deviceIdWasPersisted) {
      BELL_LOG(info, "AuthInfo", "Using persisted device id: {}", deviceId);
    } else {
      BELL_LOG(info, "AuthInfo", "Generated new device id: {}", deviceId);
    }
  }
};
}  // namespace cspot
