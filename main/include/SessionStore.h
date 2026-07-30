#pragma once

#include <string>

#include "AuthInfo.h"

namespace cspot {

// Persists an AuthInfo's JSON representation (device id + login credentials)
// to a plain file. Works unmodified on both a regular filesystem and ESP32's
// SPIFFS, which is mounted as a POSIX path.
class SessionStore {
 public:
  explicit SessionStore(std::string path);

  // Leaves authInfo untouched if the file doesn't exist or is empty.
  void load(AuthInfo& authInfo) const;

  void save(const AuthInfo& authInfo) const;

 private:
  std::string path;
};

}  // namespace cspot
