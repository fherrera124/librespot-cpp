#include "bell/http/Reader.h"

#include "bell/Logger.h"
#include "bell/Result.h"
#include "bell/http/Common.h"
#include "bell/io/MemoryStream.h"
#include "bell/net/URIParser.h"
#include "tl/expected.hpp"

namespace {
const char* LOG_TAG = "HTTPReader";
}

using namespace bell;

http::Reader::Reader(Direction readerDirection, std::istream* istream,
                     std::vector<char>* externalBuffer)
    : readerDirection(readerDirection),
      istream(istream),
      bufferPtr(externalBuffer) {
  if (bufferPtr == nullptr) {
    // External buffer not provided, use internal buffer
    bufferPtr = &internalBuffer;
  } else {
    bufferPtr->clear();

    usingExternalBuffer = true;
  }
}

http::Reader::Reader(Direction readerDirection,
                     std::shared_ptr<std::istream> istreamPtr)
    : readerDirection(readerDirection),
      sharedIstream(std::move(istreamPtr)),
      istream(sharedIstream.get()),
      bufferPtr(&internalBuffer) {
  // Use the internal buffer for reading
  bufferPtr->clear();
}

bell::Result<> http::Reader::readHeaders() {
  if (headersValid) {
    return make_unexpected_errc(std::errc::operation_not_permitted);
  }

  int minorVersion = 0;
  size_t numHeaders = 0;

  // Response specific
  const char* statusMessagePtr = nullptr;
  size_t statusMessageLen = 0;
  int parsedStatusCode = 0;

  // Request specific
  const char* pathPtr = nullptr;
  size_t pathLen = 0;
  const char* methodPtr = nullptr;
  size_t methodLen = 0;

  int lastPhrResult =
      0;  // Last result from phr_parse_request/phr_parse_response

  char lastChar = 0;
  size_t lastLineStart = 0;

  if (!usingExternalBuffer) {
    bufferPtr = &internalBuffer;
  }

  // Consume the stream byte by byte, so we dont read into the body
  while (lastPhrResult <= 0 && istream->get(lastChar)) {
    if (bufferPtr->size() > maxRequestLen) {
      BELL_LOG(error, LOG_TAG,
               "readHeaders: exceeded maxRequestLen ({} bytes), so far: {}",
               maxRequestLen,
               std::string_view(bufferPtr->data(), bufferPtr->size()));
      return make_unexpected_errc(std::errc::io_error);
    }

    bufferPtr->push_back(lastChar);

    // Full line read, process it
    if (bufferPtr->size() > 2 && bufferPtr->back() == '\n' &&
        bufferPtr->at(bufferPtr->size() - 2) == '\r') {

      // Reserve space for the headers
      phrHeaders.push_back({});
      numHeaders = phrHeaders.size();

      if (readerDirection == Direction::Request) {
        lastPhrResult =
            phr_parse_request(bufferPtr->data(), bufferPtr->size(), &methodPtr,
                              &methodLen, &pathPtr, &pathLen, &minorVersion,
                              phrHeaders.data(), &numHeaders, lastLineStart);
      } else {
        // Handle the response
        lastPhrResult = phr_parse_response(
            bufferPtr->data(), bufferPtr->size(), &minorVersion,
            &parsedStatusCode, &statusMessagePtr, &statusMessageLen,
            phrHeaders.data(), &numHeaders, lastLineStart);
      }

      bool isLastLine =
          lastLineStart > 0 && lastLineStart == bufferPtr->size() - 2;

      // Throw on phr error, or if the parser is not done yet and we're at the end
      if (lastPhrResult == -1 || (isLastLine && lastPhrResult <= 0)) {
        BELL_LOG(error, LOG_TAG,
                 "readHeaders: phr_parse_{} rejected/incomplete "
                 "(result={}, isLastLine={}), {} bytes so far: {}",
                 readerDirection == Direction::Request ? "request" : "response",
                 lastPhrResult, isLastLine, bufferPtr->size(),
                 std::string_view(bufferPtr->data(), bufferPtr->size()));
        return make_unexpected_errc(std::errc::io_error);
      }

      lastLineStart = bufferPtr->size();
    }
  }

  if (lastPhrResult <= 0) {
    // Stream ended (clean EOF or a real read error already logged by
    // SocketBuffer) before a full status line + headers were ever seen.
    BELL_LOG(error, LOG_TAG,
             "readHeaders: stream ended before headers were complete "
             "(eof={}, fail={}, bad={}), {} bytes so far: {}",
             istream->eof(), istream->fail(), istream->bad(),
             bufferPtr->size(),
             std::string_view(bufferPtr->data(), bufferPtr->size()));
    return make_unexpected_errc(std::errc::io_error);
  }

  headersValid = true;  // Mark headers as read

  // Assign the content length
  auto contentLengthHeader = getHeader("Content-Length");
  if (!contentLengthHeader.empty()) {
    contentLength = std::stoi(std::string(contentLengthHeader));
  } else {
    contentLength = 0;
  }

  if (readerDirection == Direction::Response) {
    statusCode = parsedStatusCode;
    statusMessage = std::string(statusMessagePtr, statusMessageLen);

    if (statusCode < 100 || statusCode >= 600) {
      return make_unexpected_errc(std::errc::protocol_not_supported);
    }
  } else {
    path = std::string_view(pathPtr, pathLen);
    method = parseMethod({methodPtr, methodLen});

    if (minorVersion == 1 && getHeader("Host").empty()) {
      return make_unexpected_errc(std::errc::protocol_not_supported);
    }

    auto res = parseQueryParams();
    if (!res) {
      return res;
    }
  }

  return {};
}

std::istream* http::Reader::getStream() const {
  return istream;
}

size_t http::Reader::getContentLength() const {
  return contentLength.value();
}

bell::Result<std::unordered_map<std::string, std::string>>
http::Reader::getQueryParams() const {
  if (!isValid(Direction::Request)) {
    return tl::make_unexpected(
        std::make_error_code(std::errc::operation_not_permitted));
  }

  return queryParams;
}

std::string_view http::Reader::getHeader(const std::string& headerName) const {
  for (const auto& header : phrHeaders) {
    if (header.name_len == headerName.size() &&
        std::equal(headerName.begin(), headerName.end(), header.name,
                   [](char a, char b) {
                     // Case insensitive comparison
                     return std::tolower(a) == std::tolower(b);
                   })) {
      return {header.value, header.value_len};
    }
  }

  return {};
}

http::Headers http::Reader::getAllHeaders() const {
  Headers headers{};
  for (const auto& header : phrHeaders) {
    headers.insert({std::string(header.name, header.name_len),
                    std::string(header.value, header.value_len)});
  }
  return headers;
}

bell::Result<std::string_view> http::Reader::getBodyStringView() {
  if (!usingExternalBuffer) {
    bufferPtr = &internalBuffer;
  }

  if (readContentLength == 0) {
    auto res = readBody();
    if (!res) {
      return tl::make_unexpected(res.error());
    }
  }

  if (!usingExternalBuffer) {
    bufferPtr = &internalBuffer;
  }

  return std::string_view{
      bufferPtr->data() + bufferPtr->size() - readContentLength,
      readContentLength};
}

bell::Result<std::vector<std::byte>> http::Reader::getBodyBytes() {
  if (!usingExternalBuffer) {
    bufferPtr = &internalBuffer;
  }

  if (readContentLength == 0) {
    auto res = readBody();
    if (!res) {
      return tl::make_unexpected(res.error());
    }
  }

  return std::vector<std::byte>{
      reinterpret_cast<std::byte*>(bufferPtr->data() + bufferPtr->size() -
                                   readContentLength),
      reinterpret_cast<std::byte*>(bufferPtr->data() + bufferPtr->size()),
  };
}

bell::Result<const std::byte*> http::Reader::getBodyBytesPtr() {
  if (!usingExternalBuffer) {
    bufferPtr = &internalBuffer;
  }

  if (readContentLength == 0) {
    auto res = readBody();
    if (!res) {
      return tl::make_unexpected(res.error());
    }
  }

  return reinterpret_cast<const std::byte*>(
      bufferPtr->data() + bufferPtr->size() - readContentLength);
}

bell::Result<> http::Reader::parseQueryParams() {
  if (!isValid(Direction::Request)) {
    return make_unexpected_errc(std::errc::operation_not_permitted);
  }

  if (!path.has_value()) {
    return make_unexpected_errc(std::errc::operation_not_permitted);
  }

  auto queryStart = path->find('?');
  if (queryStart != std::string::npos) {
    io::IMemoryStream ss(
        reinterpret_cast<const std::byte*>(path->data() + queryStart + 1),
        path->size() - queryStart - 1);
    std::string pair;

    while (std::getline(ss, pair, '&')) {
      size_t pos = pair.find('=');
      if (pos != std::string::npos) {
        std::string key = net::decodeURLEncoded(pair.substr(0, pos));
        std::string value = net::decodeURLEncoded(pair.substr(pos + 1));
        queryParams[key] = value;
      }
    }

    // Remove query parameters from the path
    path = path->substr(0, queryStart);
  }

  return {};
}

bell::Result<size_t> http::Reader::getBodyBytesLength() {
  if (readContentLength == 0) {
    auto res = readBody();
    if (!res) {
      return tl::make_unexpected(res.error());
    }
  }

  return readContentLength;
}

bell::Result<> http::Reader::readBody() {
  if (!usingExternalBuffer) {
    bufferPtr = &internalBuffer;
  }

  if (!isValid(readerDirection)) {
    return make_unexpected_errc(std::errc::operation_not_permitted);
  }

  if (contentLength == 0 || readContentLength == contentLength) {
    return {};  // Nothing to read
  }

  // Ensure that the response buffer has enough space to read the content
  bufferPtr->resize(bufferPtr->size() + contentLength.value() -
                    readContentLength);

  // Read the content
  istream->read(
      bufferPtr->data() + bufferPtr->size() - contentLength.value(),
      static_cast<std::streamsize>(contentLength.value() - readContentLength));

  if (istream->fail() && !istream->eof()) {
    return make_unexpected_errc(std::errc::io_error);
  }

  // Update the read content length
  readContentLength += istream->gcount();

  return {};
}

bool http::Reader::isValid(Direction expectedDirection) const {
  if (!headersValid) {
    return false;
  }

  if (readerDirection != expectedDirection) {
    return false;
  }

  return true;
}

bell::Result<int> http::Reader::getStatusCode() const {
  if (!isValid(Direction::Response)) {
    return tl::make_unexpected(
        std::make_error_code(std::errc::operation_not_permitted));
  }

  if (!statusCode.has_value()) {
    return tl::make_unexpected(std::make_error_code(std::errc::no_message));
  }

  return statusCode.value();
}

bell::Result<http::Method> http::Reader::getMethod() const {
  if (!isValid(Direction::Request)) {
    return tl::make_unexpected(
        std::make_error_code(std::errc::operation_not_permitted));
  }
  if (!method.has_value()) {
    return tl::make_unexpected(std::make_error_code(std::errc::no_message));
  }

  return method.value();
}

bell::Result<std::string_view> http::Reader::getStatusMessage() const {
  if (!isValid(Direction::Response)) {
    return tl::make_unexpected(
        std::make_error_code(std::errc::operation_not_permitted));
  }
  if (!statusMessage.has_value()) {
    return tl::make_unexpected(std::make_error_code(std::errc::no_message));
  }

  return statusMessage.value();
}

bell::Result<std::string_view> http::Reader::getPath() const {
  if (!isValid(Direction::Request)) {
    return tl::make_unexpected(
        std::make_error_code(std::errc::operation_not_permitted));
  }
  if (!path.has_value()) {
    return tl::make_unexpected(std::make_error_code(std::errc::no_message));
  }

  return path.value();
}
