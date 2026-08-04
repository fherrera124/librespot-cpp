#include "audio/CDNRangeFetcher.h"

#include "bell/Logger.h"
#include "bell/http/Common.h"
#include "nonstd/expected.hpp"

using namespace cspot;

CDNRangeFetcher::CDNRangeFetcher(std::shared_ptr<bell::HTTPClient> httpClient)
    : httpClient(std::move(httpClient)) {}

bell::Result<RangeFetchResult> CDNRangeFetcher::fetch(
    const std::string& cdnUrl, const std::string& rangeHeaderValue) {
  auto req = bell::HTTPRequest::create(bell::http::Method::GET, cdnUrl);
  if (!req) {
    return nonstd::make_unexpected(req.error());
  }
  req->operationTimeoutMs = 3000;
  req->headers["Range"] = rangeHeaderValue;

  // Deliberately no "requesting range" log here - callers (CDNDataStream,
  // PrefetchWorker) each log a single line once the fetch finishes,
  // combining the range with how long it took. Logging it here too would
  // just double the line count for no extra information.
  auto response = httpClient->rawRequest(*req);
  if (!response) {
    // No log here either, same reasoning as above - callers already log
    // the failure with the range and their own context (chunk index,
    // elapsed time) once fetch() returns the propagated error.
    return nonstd::make_unexpected(response.error());
  }

  auto* stream = response->stream();
  RangeFetchResult result;
  result.data.resize(*response->contentLength);
  stream->read(reinterpret_cast<char*>(result.data.data()),
              *response->contentLength);

  if (stream->fail() && !stream->eof()) {
    return bell::make_unexpected_errc<RangeFetchResult>(std::errc::io_error);
  }

  if (!response->headers.contains("Content-Range")) {
    return bell::make_unexpected_errc<RangeFetchResult>(
        std::errc::bad_message);
  }

  auto rangeHeader = response->headers.at("Content-Range");
  // Expected format: bytes START-END/TOTAL
  size_t spacePos = rangeHeader.find(' ');
  size_t dashPos = rangeHeader.find('-');
  size_t slashPos = rangeHeader.find('/');
  try {
    if (spacePos != std::string::npos && dashPos != std::string::npos &&
        slashPos != std::string::npos) {
      result.contentRangeStart = static_cast<size_t>(std::stoull(
          rangeHeader.substr(spacePos + 1, dashPos - (spacePos + 1))));
      result.contentRangeTotal =
          static_cast<size_t>(std::stoull(rangeHeader.substr(slashPos + 1)));
    } else {
      BELL_LOG(error, LOG_TAG, "Invalid Content-Range header: {}", rangeHeader);
      return bell::make_unexpected_errc<RangeFetchResult>(
          std::errc::bad_message);
    }
  } catch (const std::exception& e) {
    BELL_LOG(error, LOG_TAG, "Failed parsing Content-Range: {} ({})",
             rangeHeader, e.what());
    return bell::make_unexpected_errc<RangeFetchResult>(
        std::errc::bad_message);
  }

  // Trim to what was actually read (may be less than the requested
  // Content-Length on a transient short read/EOF).
  result.data.resize(static_cast<size_t>(stream->gcount()));

  return result;
}
