#pragma once

#include <string>
#include "proto/AuthenticationPb.h"

namespace cspot {
struct AuthInfo {
  std::string deviceName;
  std::string deviceId;
  std::string sessionId;

  std::optional<cspot_proto::LoginCredentials> loginCredentials;
};
}  // namespace cspot
