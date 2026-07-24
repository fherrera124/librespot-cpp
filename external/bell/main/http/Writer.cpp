#include "bell/http/Writer.h"

#include <ios>
#include <unordered_map>
#include "bell/Result.h"

using namespace bell;
namespace {
// Map of status codes to their corresponding status messages
const std::unordered_map<int, std::string> statusCodes = {
    {200, "OK"},
    {404, "Not Found"},
    {500, "Internal Server Error"},
};
}  // namespace

http::Writer::Writer(Direction writerDirection, std::ostream* ostream)
    : writerDirection(writerDirection), ostream(ostream) {}

http::Writer::Writer(Direction writerDirection,
                     std::shared_ptr<std::ostream> ostreamPtr)
    : writerDirection(writerDirection),
      sharedOstream(std::move(ostreamPtr)),
      ostream(sharedOstream.get()) {}

std::optional<std::string> http::Writer::getStatusMessage() {
  if (!statusCode) {
    return std::nullopt;
  }

  auto it = statusCodes.find(*statusCode);
  if (it == statusCodes.end()) {
    return "Unknown";
  }
  return it->second;
}

bell::Result<> http::Writer::writeHeaders() {
  if (headersWritten) {
    return make_unexpected_errc(std::errc::operation_not_permitted);
  }

  // Will fill in standard headers if they are not already set
  enforceStandardHeaders();

  if (writerDirection == Direction::Request) {
    if (method && path) {
      *ostream << methodToString(*method) << " " << *path << " HTTP/1.1\r\n";
    } else {
      return make_unexpected_errc(std::errc::invalid_argument);
    }
  } else {
    if (statusCode) {
      *ostream << "HTTP/1.1 " << *statusCode << " "
               << getStatusMessage().value() << "\r\n";
    } else {
      return make_unexpected_errc(std::errc::invalid_argument);
    }
  }

  for (const auto& header : headers) {
    *ostream << header.first << ": " << header.second << "\r\n";
  }

  if (contentLength > 0) {
    *ostream << "content-length: " << contentLength << "\r\n";
  }

  *ostream << "\r\n";  // End of headers
  if (!ostream->flush() || (ostream->fail() && !ostream->eof())) {
    return make_unexpected_errc(std::errc::io_error);
  }

  headersWritten = true;

  return {};
}

void http::Writer::setHeader(const std::string& headerName,
                             const std::string& headerValue) {
  headers[headerName] = headerValue;
}

void http::Writer::setHeaders(const Headers& headers) {
  for (const auto& header : headers) {
    setHeader(header.first, header.second);
  }
}

void http::Writer::enforceStandardHeaders() {
  if (writerDirection == Direction::Request) {
    if (headers.find("host") == headers.end()) {
      headers["host"] =
          "localhost";  // Default host, should be overridden by the actual host
    }
    if (headers.find("user-agent") == headers.end()) {
      headers["user-agent"] = defaultUserAgent;
    }
  } else {
    // Standard response headers
    if (contentLength > 0 && headers.find("content-type") == headers.end()) {
      headers["content-type"] = "text/html";  // Default content type
    }
  }
}

bell::Result<> http::Writer::setContentLength(size_t contentLength) {
  if (!isValid(writerDirection)) {
    return make_unexpected_errc(std::errc::operation_not_supported);
  }

  this->contentLength = contentLength;

  return {};
}

bell::Result<> http::Writer::writeRequest(Method method,
                                          const std::string& path,
                                          const Headers& headers,
                                          size_t expectedContentLength) {
  if (!isValid(Direction::Request)) {
    return make_unexpected_errc(std::errc::operation_not_supported);
  }

  // Assign the request parameters
  auto res = setMethod(method);

  if (!res) {
    return res;
  }

  res = setPath(path);
  if (!res) {
    return res;
  }

  setHeaders(headers);
  res = setContentLength(expectedContentLength);
  if (!res) {
    return res;
  }

  return writeHeaders();
}

bell::Result<> http::Writer::writeResponse(int statusCode,
                                           const Headers& headers,
                                           size_t expectedContentLength) {
  if (!isValid(Direction::Response)) {
    return make_unexpected_errc(std::errc::operation_not_supported);
  }
  // Assign the request parameters
  auto res = setStatusCode(statusCode);
  if (!res) {
    return res;
  }

  setHeaders(headers);

  res = setContentLength(expectedContentLength);
  if (!res) {
    return res;
  }

  return writeHeaders();
}

bell::Result<> http::Writer::writeResponseWithBody(int statusCode,
                                                   const Headers& headers,
                                                   const std::string& body) {
  auto res = writeResponse(statusCode, headers, body.size());
  if (!res) {
    return res;
  }

  return writeBodyStringView(body);
}

bool http::Writer::hasWrittenHeaders() const {
  return headersWritten;
}

bool http::Writer::hasWrittenBody() const {
  return contentLengthWritten >= contentLength;
}

bool http::Writer::isValid(Direction expectedDirection) {
  if (headersWritten) {
    return false;
  }

  if (writerDirection != expectedDirection) {
    return false;
  }

  return true;
}

bell::Result<> http::Writer::setPath(const std::string& path) {
  if (!isValid(Direction::Request)) {
    return make_unexpected_errc(std::errc::operation_not_supported);
  }

  this->path = path;

  return {};
}

bell::Result<> http::Writer::setMethod(Method method) {
  if (!isValid(Direction::Request)) {
    return make_unexpected_errc(std::errc::operation_not_supported);
  }

  this->method = method;
  return {};
}

bell::Result<> http::Writer::setStatusCode(int statusCode) {
  if (!isValid(Direction::Response)) {
    return make_unexpected_errc(std::errc::operation_not_supported);
  }

  this->statusCode = statusCode;

  return {};
}

std::ostream* http::Writer::getStream() const {
  return ostream;
}

bell::Result<> http::Writer::writeBodyRaw(const std::byte* bytes,
                                          size_t bytesLen) {
  if (!headersWritten) {
    return make_unexpected_errc(std::errc::operation_not_permitted);
  }

  if (contentLengthWritten + bytesLen > contentLength) {
    return make_unexpected_errc(std::errc::io_error);
  }

  ostream->write(reinterpret_cast<const char*>(bytes),
                 static_cast<std::streamsize>(bytesLen));
  contentLengthWritten += bytesLen;

  if (!ostream->flush() || (ostream->fail() && !ostream->eof())) {
    return make_unexpected_errc(std::errc::io_error);
  }

  return {};
}

bell::Result<> http::Writer::writeBodyStringView(std::string_view body) {
  return writeBodyRaw(reinterpret_cast<const std::byte*>(body.data()),
                      body.size());
}
