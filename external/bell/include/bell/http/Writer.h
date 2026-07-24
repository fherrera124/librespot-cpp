#pragma once

// Standard includes
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

// Own includes
#include "bell/Result.h"
#include "bell/http/Common.h"

namespace bell::http {
class Writer {
 public:
  // Default constructor, initializes to invalid state
  Writer() = default;

  /**
   * @brief http::Writer constructor, initializes the writer with the given stream. No data is written to the stream until writeHeaders() is called.
   *
   * @param readerDirection Type of the writer, either Request or Response
   * @param ostream Pointer to the output stream, which must be valid until the writer is destroyed
   */
  Writer(Direction writerDirection, std::ostream* ostream);

  /**
   * @brief Ownership taking constructor. No data is written to the stream until writeHeaders() is called.
   *
   * @param readerDirection Type of the writer, either Request or Response
   * @param ostream Pointer to the output stream, which must be valid until the writer is destroyed
   */
  Writer(Direction writerDirection, std::shared_ptr<std::ostream> ostreamPtr);

  /**
   * @brief Raw headers write method
   *
   * Will send all the configured headers to the stream, along with either the request or response line. Call the writeRequest / writeResponse methods for easier use.
   */
  bell::Result<> writeHeaders();

  /**
   * @brief Writes an HTTP request line and headers to the stream.
   *
   * Constructs and writes an HTTP request line followed by headers.
   * The request line consists of the HTTP method, path, and protocol version.
   *
   * @param method HTTP method to use for the request (e.g., GET, POST).
   * @param path The path for the HTTP request.
   * @param headers Optional additional headers for the request.
   * @param expectedContentLength The expected content length for request body.
   */
  bell::Result<> writeRequest(Method method, const std::string& path,
                              const Headers& headers = {},
                              size_t expectedContentLength = 0);

  /**
   * @brief Writes an HTTP response line and headers to the stream.
   *
   * Constructs and writes an HTTP response line followed by headers.
   * The response line consists of the HTTP version, status code, and status message.
   *
   * @param statusCode HTTP status code for the response (e.g., 200, 404).
   * @param headers Optional additional headers for the response.
   * @param expectedContentLength The expected content length for response body.
   */
  bell::Result<> writeResponse(int statusCode, const Headers& headers = {},
                               size_t expectedContentLength = 0);

  /**
   * @brief Writes an HTTP response with a body to the stream.
   *
   * Constructs and writes an HTTP response line, headers, and a body.
   * Automatically sets content-related headers based on the length of the body.
   *
   * @param statusCode HTTP status code for the response.
   * @param headers Optional additional headers for the response.
   * @param body The body content to be included in the response.
   */
  bell::Result<> writeResponseWithBody(int statusCode,
                                       const Headers& headers = {},
                                       const std::string& body = "");

  /**
   * @brief Sets an individual HTTP header.
   *
   * Adds or overwrites the specified header in the internal header map.
   *
   * @param headerName The name of the header to set.
   * @param headerValue The value of the header to set.
   */
  void setHeader(const std::string& headerName, const std::string& headerValue);

  /**
   * @brief Sets multiple HTTP headers.
   *
   * Adds or overwrites the specified headers in the internal header map.
   *
   * @param headers A collection of headers to set.
   */
  void setHeaders(const Headers& headers);

  /**
   * @brief Sets the content length for the HTTP body.
   *
   * Specifies the expected length of the content to be written or received.
   *
   * @param contentLength The length of the content.
   */
  bell::Result<> setContentLength(size_t contentLength);

  /**
   * @brief Sets the HTTP status code for a response.
   *
   * Defines the status code that will be used in the response line.
   *
   * @param statusCode The HTTP status code to set (e.g., 200, 404).
   */
  bell::Result<> setStatusCode(int statusCode);

  /**
   * @brief Sets the HTTP request path.
   *
   * Defines the path that will be used in the request line.
   *
   * @param path The path for the HTTP request.
   */
  bell::Result<> setPath(const std::string& path);

  /**
   * @brief Sets the HTTP method for a request.
   *
   * Specifies the method that will be used in the request line.
   *
   * @param method The HTTP method to set (e.g., GET, POST).
   */
  bell::Result<> setMethod(Method method);

  /**
   * @brief Writes a body to the stream using a string view.
   *
   * Writes the provided body content directly to the output stream.
   *
   * @param body The body content to write to the stream.
   */
  bell::Result<> writeBodyStringView(std::string_view body);

  /**
   * @brief Writes raw bytes as a body to the stream.
   *
   * Writes the specified number of bytes from a byte array to the output stream.
   *
   * @param bytes Pointer to the byte array.
   * @param bytesLen The number of bytes to write from the array.
   */
  bell::Result<> writeBodyRaw(const std::byte* bytes, size_t bytesLen);

  /**
   * @brief Returns true if the headers have been written to the stream.
   */
  bool hasWrittenHeaders() const;

  /**
   * @brief Returns true if the body has been written to the stream, or if no body is expected.
   */
  bool hasWrittenBody() const;

  /**
   * @brief Returns the stream used by the writer
   *
   * @return std::istream* Pointer to the stream used by the reader. Will be valid until the reader is destroyed.
   */
  std::ostream* getStream() const;

 private:
  Direction writerDirection = Direction::Invalid;
  std::shared_ptr<std::ostream> sharedOstream;
  std::ostream* ostream{};

  const char* defaultUserAgent = "bell/1.0";

  bool headersWritten = false;

  // Case-insensitive compare for headers map
  struct CaseInsensitiveCompare {
    bool operator()(const std::string& a, const std::string& b) const {
      return std::lexicographical_compare(
          a.begin(), a.end(), b.begin(), b.end(),
          [](char a, char b) { return std::tolower(a) < std::tolower(b); });
    }
  };

  std::map<std::string, std::string, CaseInsensitiveCompare> headers;
  size_t contentLength = 0;
  size_t contentLengthWritten = 0;

  // Request specific
  std::optional<Method> method;
  std::optional<std::string> path;

  // Response specific
  std::optional<int> statusCode;

  std::optional<std::string> getStatusMessage();
  void enforceStandardHeaders();
  bool isValid(Direction expectedDirection);
};
}  // namespace bell::http

namespace bell {
using HTTPWriter = http::Writer;
}
