#pragma once

// Standard library includes
#include <cstddef>
#include <optional>

// Own includes
#include "bell/Result.h"

namespace bell::io {

/**
 * @brief Abstract interface representing a readable stream of bytes.
 *
 * This interface abstracts over both finite and infinite streams of data,
 *
 * Streams are always read in terms of raw bytes, with no implicit encoding.
 * Implementations are expected to handle any necessary internal buffering,
 * especially for expensive or remote sources (e.g. chunked HTTP requests).
 */
class DataStream {
 public:
  virtual ~DataStream() = default;

  /**
   * @brief Enumeration for seek origin.
   */
  enum class SeekOrigin {
    Begin,    // From the beginning of the stream
    Current,  // From the current position
    End       // From the end of the stream
  };

  /**
   * @brief Indicates whether the stream supports random access via seek().
   *
   * @return true if seek() can be called to move to an arbitrary position.
   *         false if the stream is sequential-only.
   *
   * @note Infinite streams are typically not seekable.
   */
  virtual bool isSeekable() const = 0;

  /**
   * @brief Indicates whether the stream is potentially infinite.
   *
   * An infinite stream is one that has no defined end (EOF may never occur),
   * such as a live broadcast. Finite streams have a definite size.
   *
   * @return true if stream length is unbounded or unknown; false otherwise.
   */
  virtual bool isInfinite() const = 0;

  /**
   * @brief Returns the total size of the stream in bytes, if known.
   *
   * @return Size in bytes, or std::nullopt if size is unknown or stream is infinite.
   */
  virtual std::optional<size_t> size() const = 0;

  /**
   * @brief Returns the current read position within the stream.
   *
   * For seekable streams, this is the byte offset from the start.
   * For non-seekable streams, this is the total number of bytes read so far.
   *
   * @return Current position in bytes.
   */
  virtual size_t position() const = 0;

  /**
   * @brief Moves the read position to the specified byte offset.
   *
   * For seekable streams, the next read() will return data starting from
   * the given offset. Implementations may perform this eagerly (fetching
   * data immediately) or lazily (fetching on the next read()).
   *
   * For non-seekable streams, this returns an error Result.
   *
   * @param offset Absolute byte offset from the start of the stream.
   * @param origin Seek origin (default: Begin).
   * @return Success if seek was possible; error otherwise.
   */
  virtual bell::Result<> seek(size_t offset,
                              SeekOrigin origin = SeekOrigin::Begin) = 0;

  /**
   * @brief Reads data from the stream into the provided buffer.
   *
   * @param outputBuffer Pointer to destination buffer.
   * @param outputBufferLen Maximum number of bytes to read.
   * @return Number of bytes actually read, or an error Result.
   *
   * @note Returns 0 only at end-of-stream for finite streams.
   *       Infinite streams may block until data is available.
   */
  virtual bell::Result<size_t> read(std::byte* outputBuffer,
                                    size_t outputBufferLen) = 0;
};

}  // namespace bell::io
