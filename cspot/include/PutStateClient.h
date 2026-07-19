#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "HTTPClient.h"
#include "protobuf/connectstate.pb.h"

namespace cspot {

// Sends connect-state PUT requests to spclient: host resolution/caching,
// connection reuse, bounded retry, and 429 rate-limit tracking. Knows
// nothing about what a PutStateRequest means - callers fill it in.
class PutStateClient {
 public:
  // hostResolver: returns a fresh spclient host (default: ApResolve).
  // onHostResolved: called with the host whenever freshly resolved - lets
  // the caller seed anything else that wants the same host (e.g.
  // ContextResolver).
  // useTls: false only for host tests, against a local plain-HTTP server.
  explicit PutStateClient(
      std::function<std::string()> hostResolver = defaultHostResolver,
      std::function<void(const std::string&)> onHostResolved = nullptr,
      bool useTls = true);

  // Encodes and PUTs `request` to /connect-state/v1/devices/{deviceId}.
  // Bounded retry (2 attempts, 1s apart): 4xx is permanent, a dropped
  // connection or 5xx is transient. Returns true on HTTP 200.
  bool put(connectstate_PutStateRequest& request, const std::string& deviceId,
          const std::string& accessToken, const std::string& clientToken,
          const std::string& connectionId);

  // PUTs .../inactive?notify=false (empty body). Returns true on 2xx.
  bool putInactive(const std::string& deviceId, const std::string& accessToken,
                   const std::string& clientToken,
                   const std::string& connectionId);

  // Set from a 429's Retry-After - callers wanting to back off before the
  // next put()/putInactive() should check this.
  std::chrono::steady_clock::time_point rateLimitedUntil() const;

  // ApResolve("").fetchFirstSpclientAddress() - public so callers can name
  // it explicitly when overriding onHostResolved/useTls but not hostResolver.
  static std::string defaultHostResolver();

 private:
  std::function<std::string()> hostResolver;
  std::function<void(const std::string&)> onHostResolved;
  bool useTls;

  std::mutex putMutex;
  std::string spclientHost;
  std::unique_ptr<bell::HTTPClient::Response> putConnection;
  std::atomic<std::chrono::steady_clock::time_point> rateLimitedUntilTime{
      std::chrono::steady_clock::time_point{}};
};

}  // namespace cspot
