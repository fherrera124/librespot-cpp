
#include "bell/http/DataStream.h"

#include <cassert>

#include "bell/Logger.h"
#include "bell/http/Client.h"
#include "bell/http/Common.h"

using namespace bell::http;

bell::Result<> DataStream::open(bell::HTTPMethod method, const std::string& url,
                                const Headers& headers) {
  lastReadChunk.resize(chunkSize);
  bytesInLastReadChunk = 0;
  chunkStartPosition = 0;
  currentPosition = 0;
  isSeekableFlag = false;
  activeResponse.reset();

  auto req = Request::create(method, url);
  if (!req) {
    return tl::make_unexpected(req.error());
  }

  req->operationTimeoutMs = 3000;
  req->headers = headers;
  this->httpRequest = *req;
  this->httpRequest.headers["Connection"] = "keep-alive";
  this->httpRequest.headers["Range"] = fmt::format("bytes=0-{}", chunkSize - 1);

  // Initial request
  auto response = httpClient->rawRequest(httpRequest);
  if (!response) {
    BELL_LOG(error, LOG_TAG, "HTTP request error: {}", response.error());
    return tl::make_unexpected(response.error());
  }

  totalSize = response->contentLength;
  activeResponse = *response;

  // Detect seekability via Content-Range
  if (activeResponse->headers.contains("Content-Range")) {
    auto rangeHeader = activeResponse->headers.at("Content-Range");
    auto slashPos = rangeHeader.find('/');
    if (slashPos != std::string::npos) {
      try {
        totalSize = std::stoll(rangeHeader.substr(slashPos + 1));
        isSeekableFlag = true;
      } catch (const std::invalid_argument& e) {
        BELL_LOG(error, LOG_TAG, "Failed to parse Content-Range header: {}",
                 e.what());
        return bell::make_unexpected_errc<>(std::errc::bad_message);
      }
    }
  } else if (totalSize.has_value()) {
    // If Content-Length exists but no Content-Range → finite, not seekable
    isSeekableFlag = false;
  }

  // Preload first chunk
  auto* stream = activeResponse->stream();
  stream->read(reinterpret_cast<char*>(lastReadChunk.data()), chunkSize);
  if (stream->fail() && !stream->eof()) {
    return bell::make_unexpected_errc<>(std::errc::io_error);
  }
  bytesInLastReadChunk = static_cast<size_t>(stream->gcount());
  chunkStartPosition = 0;

  return {};
}

bool DataStream::isSeekable() const {
  return isSeekableFlag;
}

bool DataStream::isInfinite() const {
  return !totalSize.has_value();
}

std::optional<size_t> DataStream::size() const {
  return totalSize;
}

size_t DataStream::position() const {
  return currentPosition;
}

bell::Result<> DataStream::seek(size_t offset, SeekOrigin origin) {
  (void)origin;  // TODO: support other origins
  if (!isSeekable()) {
    return bell::make_unexpected_errc<>(std::errc::invalid_argument);
  }
  if (offset >= totalSize.value_or(0)) {
    return bell::make_unexpected_errc<>(std::errc::invalid_seek);
  }

  currentPosition = offset;
  bytesInLastReadChunk = 0;
  chunkStartPosition = 0;

  return {};
}

bell::Result<size_t> DataStream::read(std::byte* outputBuffer,
                                      size_t outputBufferLen) {
  size_t totalCopied = 0;
  size_t toRead = outputBufferLen;

  while (toRead > 0) {
    // Copy from current chunk
    size_t availableInChunk = (bytesInLastReadChunk > chunkStartPosition)
                                  ? bytesInLastReadChunk - chunkStartPosition
                                  : 0;

    if (availableInChunk > 0) {
      size_t toCopy = std::min(toRead, availableInChunk);
      std::copy(lastReadChunk.data() + chunkStartPosition,
                lastReadChunk.data() + chunkStartPosition + toCopy,
                outputBuffer + totalCopied);

      chunkStartPosition += toCopy;
      currentPosition += toCopy;
      totalCopied += toCopy;
      toRead -= toCopy;

      if (toRead == 0) {
        break;
      }
    }

    // Need more data
    if (isSeekable()) {
      auto res = requestNextRange();
      if (!res) {
        return bell::make_unexpected_errc<size_t>(std::errc::io_error);
      }
    } else {
      if (!activeResponse) {
        break;
      }
      auto* stream = activeResponse->stream();
      stream->read(reinterpret_cast<char*>(lastReadChunk.data()), chunkSize);
      if (stream->fail() && !stream->eof()) {
        return bell::make_unexpected_errc<size_t>(std::errc::io_error);
      }
      bytesInLastReadChunk = static_cast<size_t>(stream->gcount());
      chunkStartPosition = 0;
      if (bytesInLastReadChunk == 0) {
        break;  // EOF
      }
    }
  }

  return totalCopied;
}

bell::Result<> DataStream::requestNextRange() {
  assert(isSeekable());

  size_t chunkReadSize =
      std::min(chunkSize, totalSize.value_or(SIZE_MAX) - currentPosition);

  if (chunkReadSize == 0) {
    return bell::make_unexpected_errc<>(std::errc::invalid_seek);
  }

  httpRequest.headers["Range"] = fmt::format(
      "bytes={}-{}", currentPosition, currentPosition + chunkReadSize - 1);

  // Reset the old response
  activeResponse.reset();
  BELL_LOG(debug, LOG_TAG, "Requesting range: {}",
           httpRequest.headers["Range"]);

  auto response = httpClient->rawRequest(httpRequest);
  if (!response) {
    BELL_LOG(error, LOG_TAG, "HTTP request error: {}", response.error());
    return tl::make_unexpected(response.error());
  }

  activeResponse = *response;

  auto* stream = activeResponse->stream();
  stream->read(reinterpret_cast<char*>(lastReadChunk.data()),
               *response->contentLength);

  if (stream->fail() && !stream->eof()) {
    return bell::make_unexpected_errc<>(std::errc::io_error);
  }

  bytesInLastReadChunk = static_cast<size_t>(stream->gcount());
  chunkStartPosition = 0;

  return {};
}
