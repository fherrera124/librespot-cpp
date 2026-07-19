#pragma once

#include <chrono>
#include <stdexcept>
#include <string>
#include <utility>

#include "BellUtils.h"  // for BELL_SLEEP_MS
#include "Logger.h"     // for CSPOT_LOG

namespace cspot {

// Thrown from inside HttpRetry::run()'s attempt callback to stop
// immediately instead of consuming the rest of the retry budget - use it
// for failures a retry would never fix (e.g. a permanent 4xx, a request
// that's malformed by construction). Any other std::exception is treated
// as transient and retried while attempts remain, same split as
// go-librespot's backoff.Permanent(err) (spclient.go).
class PermanentHttpFailure : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// A 429 with a server-suggested cooldown - a specialization of
// PermanentHttpFailure (never retried within HttpRetry's own bounded
// budget; that would just make the rate limit worse), but a caller that
// schedules its own next attempt independently (e.g. ConnectStateHandler's
// PUT coalescing loop) should catch this specifically to read retryAfter
// and hold off that long before trying again. Mirrors go-librespot's
// RateLimitedError (spclient.go).
class RateLimitedError : public PermanentHttpFailure {
 public:
  RateLimitedError(std::chrono::seconds retryAfter, const std::string& what)
      : PermanentHttpFailure(what), retryAfter(retryAfter) {}

  std::chrono::seconds retryAfter;
};

// Bounded retry with a constant backoff between attempts - consolidates
// what used to be hand-rolled per call site (ContextResolver::resolve(),
// TrackQueue's CDN-url step) into one place, mirroring go-librespot's
// backoff.WithMaxRetries(backoff.NewConstantBackOff(...)) pattern
// (spclient.go, audio/chunked-reader.go). Only fits a single blocking
// call-and-parse - TrackQueue's preload steps stay on their own
// state-machine-driven retry (see TrackQueue.cpp) since those need to
// yield between attempts instead of sleeping the calling thread, so they
// don't stall sibling tracks being preloaded on the same task.
class HttpRetry {
 public:
  HttpRetry(int maxAttempts, std::chrono::milliseconds backoff,
            std::string opName)
      : maxAttempts(maxAttempts),
        backoff(backoff),
        opName(std::move(opName)) {}

  // attempt() should perform exactly one try and either return normally or
  // throw. Sleeps `backoff` between attempts, never after the last one.
  template <typename Fn>
  auto run(Fn&& attempt) -> decltype(attempt()) {
    for (int attemptNum = 1;; attemptNum++) {
      try {
        return attempt();
      } catch (const PermanentHttpFailure& e) {
        CSPOT_LOG(error, "%s: %s (not retrying)", opName.c_str(), e.what());
        throw;
      } catch (const std::exception& e) {
        if (attemptNum >= maxAttempts) {
          CSPOT_LOG(error, "%s: giving up after %d attempts: %s",
                   opName.c_str(), maxAttempts, e.what());
          throw;
        }
        CSPOT_LOG(error, "%s: %s, retrying (%d/%d)", opName.c_str(),
                 e.what(), attemptNum, maxAttempts - 1);
        BELL_SLEEP_MS(backoff.count());
      }
    }
  }

 private:
  int maxAttempts;
  std::chrono::milliseconds backoff;
  std::string opName;
};

}  // namespace cspot
