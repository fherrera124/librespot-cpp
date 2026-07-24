#pragma once

// Standard includes
#include <optional>

// Own includes
#include "bell/http/Client.h"
#include "bell/http/Common.h"
#include "bell/io/DataStream.h"

namespace bell::http {
class DataStream : public io::DataStream {
 public:
  explicit DataStream(std::shared_ptr<Client> httpClient,
                      size_t chunkSize = 4 * 1024L)
      : httpClient(std::move(httpClient)), chunkSize(chunkSize) {}

  ~DataStream() override = default;

  bell::Result<> open(bell::HTTPMethod method, const std::string& url,
                      const Headers& headers);

  bool isSeekable() const override;
  bool isInfinite() const override;
  std::optional<size_t> size() const override;
  size_t position() const override;
  bell::Result<> seek(size_t offset, SeekOrigin origin) override;
  bell::Result<size_t> read(std::byte* outputBuffer,
                            size_t outputBufferLen) override;

 private:
  const char* LOG_TAG = "HTTPDataStream";

  // HTTP client handle
  std::shared_ptr<Client> httpClient;

  bool isSeekableFlag = false;

  std::optional<size_t> totalSize;

  // Amount of bytes to request per chunk
  size_t chunkSize;

  // Buffer to store the data
  std::vector<std::byte> lastReadChunk;
  size_t bytesInLastReadChunk = 0;
  size_t chunkStartPosition = 0;

  size_t currentPosition = 0;

  int64_t totalRequestTimeMs = 0;

  // Keeps params of the HTTP request, reused for range-based files
  Request httpRequest;
  std::optional<Response> activeResponse;

  // Requests a new range of data from the server, filling up the lastReadChunk buffer.
  // This is called only for seekable streams
  bell::Result<> requestNextRange();
};
}  // namespace bell::http

namespace bell {
using HTTPDataStream = http::DataStream;
}
