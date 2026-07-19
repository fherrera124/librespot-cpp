#include "PutStateClient.h"

#include "ApResolve.h"
#include "HttpRetry.h"     // for HttpRetry, PermanentHttpFailure, RateLimitedError
#include "Logger.h"        // for CSPOT_LOG
#include "NanoPBHelper.h"  // for pbEncode

using namespace cspot;

namespace {
// Spotify always sends Retry-After in delta-seconds, never an HTTP-date
// (developer.spotify.com/documentation/web-api/concepts/rate-limits) - only
// that form is parsed. Missing/malformed falls back to a default.
std::chrono::seconds parseRetryAfter(std::string_view value) {
  constexpr std::chrono::seconds DEFAULT_RETRY_AFTER{10};
  if (value.empty()) {
    return DEFAULT_RETRY_AFTER;
  }
  try {
    int secs = std::stoi(std::string(value));
    return secs > 0 ? std::chrono::seconds(secs) : DEFAULT_RETRY_AFTER;
  } catch (const std::exception&) {
    return DEFAULT_RETRY_AFTER;
  }
}
}  // namespace

std::string PutStateClient::defaultHostResolver() {
  return ApResolve("").fetchFirstSpclientAddress();
}

PutStateClient::PutStateClient(std::function<std::string()> hostResolver,
                               std::function<void(const std::string&)> onHostResolved,
                               bool useTls)
    : hostResolver(std::move(hostResolver)),
      onHostResolved(std::move(onHostResolved)),
      useTls(useTls) {}

std::chrono::steady_clock::time_point PutStateClient::rateLimitedUntil() const {
  return rateLimitedUntilTime.load();
}

bool PutStateClient::put(connectstate_PutStateRequest& request,
                         const std::string& deviceId,
                         const std::string& accessToken,
                         const std::string& clientToken,
                         const std::string& connectionId) {
  // TEMP DIAGNOSTIC (playlist-switch flicker investigation, 2026-07-18):
  // dump exactly what this PUT's PlayerState carries, to rule out stale
  // track/position data leaving the device. Remove once resolved.
  CSPOT_LOG(info,
           "PUT DIAG: reason=%d is_active=%d is_playing=%d is_paused=%d "
           "is_buffering=%d has_track=%d track.uri=%s pos_as_of_ts=%lld",
           (int)request.put_state_reason, (int)request.is_active,
           (int)request.device.player_state.is_playing,
           (int)request.device.player_state.is_paused,
           (int)request.device.player_state.is_buffering,
           (int)request.device.player_state.has_track,
           request.device.player_state.has_track
               ? request.device.player_state.track.uri
               : "-",
           (long long)request.device.player_state.position_as_of_timestamp);

  std::vector<uint8_t> body;
  try {
    body = pbEncode(connectstate_PutStateRequest_fields, &request);
  } catch (const std::exception& e) {
    CSPOT_LOG(error, "connect-state encode failed: %s", e.what());
    return false;
  }

  std::scoped_lock lock(putMutex);

  // Bounded retry (2 attempts, 1s apart): 4xx is permanent (no retry), a
  // dropped connection or 5xx is transient. spclientHost/putConnection are
  // reset only on a genuine transport exception, never on a mere non-200.
  try {
    return HttpRetry(2, std::chrono::milliseconds(1000), "connect-state PUT")
        .run([&]() -> bool {
          if (spclientHost.empty()) {
            spclientHost = hostResolver();
            if (onHostResolved) {
              onHostResolved(spclientHost);
            }
          }
          auto url = (useTls ? "https://" : "http://") + spclientHost +
                     "/connect-state/v1/devices/" + deviceId;

          bell::HTTPClient::Headers headers = {
              {"Authorization", "Bearer " + accessToken},
              {"Client-Token", clientToken},
              {"X-Spotify-Connection-Id", connectionId},
              {"Content-Type", "application/x-protobuf"}};

          try {
            if (putConnection == nullptr) {
              putConnection = bell::HTTPClient::put(url, headers, body);
            } else {
              putConnection->put(url, headers, body);
            }
          } catch (const std::exception& e) {
            putConnection.reset();
            spclientHost.clear();
            throw std::runtime_error(std::string("request failed: ") +
                                     e.what());
          }

          int status = putConnection->statusCode();
          std::string responseBody(putConnection->body());
          if (status == 200) {
            CSPOT_LOG(info, "connect-state PUT ok (reason %d)",
                     (int)request.put_state_reason);
            return true;
          }

          std::string reason =
              "status " + std::to_string(status) + ": " + responseBody;
          if (status == 429) {
            throw RateLimitedError(
                parseRetryAfter(putConnection->header("retry-after")),
                reason);
          }
          if (status >= 400 && status < 500) {
            throw PermanentHttpFailure(reason);
          }
          throw std::runtime_error(reason);
        });
  } catch (const RateLimitedError& e) {
    rateLimitedUntilTime = std::chrono::steady_clock::now() + e.retryAfter;
    CSPOT_LOG(error, "connect-state PUT rate-limited, backing off %llds",
             (long long)e.retryAfter.count());
    return false;
  } catch (const std::exception&) {
    return false;  // HttpRetry already logged the final giving-up message
  }
}

bool PutStateClient::putInactive(const std::string& deviceId,
                                 const std::string& accessToken,
                                 const std::string& clientToken,
                                 const std::string& connectionId) {
  std::scoped_lock lock(putMutex);
  try {
    if (spclientHost.empty()) {
      spclientHost = hostResolver();
      if (onHostResolved) {
        onHostResolved(spclientHost);
      }
    }
    // notify=false, matching what go-librespot's stopPlayback() passes.
    auto url = (useTls ? "https://" : "http://") + spclientHost +
               "/connect-state/v1/devices/" + deviceId + "/inactive?notify=false";
    bell::HTTPClient::Headers headers = {
        {"Authorization", "Bearer " + accessToken},
        {"Client-Token", clientToken},
        {"X-Spotify-Connection-Id", connectionId}};

    if (putConnection == nullptr) {
      putConnection = bell::HTTPClient::put(url, headers, {});
    } else {
      putConnection->put(url, headers, {});
    }

    int status = putConnection->statusCode();
    (void)putConnection->body();  // drain - never logged here
    // 204 expected; tolerate any 2xx.
    if (status >= 200 && status < 300) {
      CSPOT_LOG(info, "connect-state inactive PUT ok (%d)", status);
      return true;
    }
    CSPOT_LOG(error, "connect-state inactive PUT failed, status %d", status);
    return false;
  } catch (const std::exception& e) {
    putConnection.reset();
    spclientHost.clear();
    CSPOT_LOG(error, "connect-state inactive PUT failed: %s", e.what());
    return false;
  }
}
