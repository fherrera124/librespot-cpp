#pragma once

// Standard includes
#include <memory>
#include <string>
#include <vector>

// Library includes
#include "bell/Result.h"
#include "bell/http/Client.h"

namespace cspot {

// Result of a single HTTP range fetch: the bytes actually read from the
// response body, plus the START/TOTAL parsed out of the server's
// "Content-Range: bytes START-END/TOTAL" response header.
struct RangeFetchResult {
  std::vector<std::byte> data;
  size_t contentRangeStart = 0;
  size_t contentRangeTotal = 0;
};

/**
 * @brief Executes a single blocking HTTP GET with a Range header against a
 *        CDN URL and parses the resulting Content-Range.
 *
 * No state is retained between calls (every call builds its own Request/
 * Response locally) - the same instance, or several independent instances
 * sharing the same bell::HTTPClient, can be called concurrently from
 * different threads. bell::http::ConnectionPool (owned by the HTTPClient)
 * is what hands each concurrent call its own socket.
 */
class CDNRangeFetcher {
 public:
  explicit CDNRangeFetcher(std::shared_ptr<bell::HTTPClient> httpClient);

  // rangeHeaderValue is the full value to send as the "Range" header, e.g.
  // "bytes=0-32767" or a suffix request "bytes=-32768" - callers decide
  // alignment/suffix-vs-explicit, this class only executes the request.
  bell::Result<RangeFetchResult> fetch(const std::string& cdnUrl,
                                       const std::string& rangeHeaderValue);

 private:
  const char* LOG_TAG = "CDNRangeFetcher";
  std::shared_ptr<bell::HTTPClient> httpClient;
};

}  // namespace cspot
