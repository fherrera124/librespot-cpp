#pragma once

// Standard includes
#include <fstream>
#include <optional>

// Own includes
#include "bell/io/DataStream.h"

namespace bell::io {

/**
 * @brief A DataStream implementation backed by an owned std::ifstream.
 *
 * Supports seeking, querying size, and chunked reads.
 * Owns the ifstream, which will be closed automatically on destruction.
 */
class FileDataStream : public DataStream {
 public:
  /**
     * @brief Constructs a DataStream by taking ownership of a std::ifstream.
     * @param file An rvalue reference to an opened std::ifstream in binary mode.
     * @throws std::runtime_error if the stream is not open or not good().
     */
  explicit FileDataStream(std::ifstream&& file);

  ~FileDataStream() override = default;

  bool isSeekable() const override { return true; }
  bool isInfinite() const override { return false; }
  std::optional<size_t> size() const override { return fileSize; }
  size_t position() const override;
  bell::Result<> seek(size_t offset, SeekOrigin origin) override;
  bell::Result<size_t> read(std::byte* outputBuffer,
                            size_t outputBufferLen) override;

 private:
  mutable std::ifstream file;
  std::optional<size_t> fileSize;
};

}  // namespace bell::io
