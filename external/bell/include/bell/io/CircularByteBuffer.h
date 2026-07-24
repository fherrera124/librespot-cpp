#pragma once

// Standard library includes
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace bell::io {
class CircularByteBuffer {
 public:
  /**
   * @brief Constructs a ByteCircularBuffer with a given capacity.
   * @param capacity The maximum number of bytes the buffer can hold.
   */
  explicit CircularByteBuffer(size_t capacity);

  // Delete copy constructor and assignment operator
  CircularByteBuffer(const CircularByteBuffer&) = delete;
  CircularByteBuffer& operator=(const CircularByteBuffer&) = delete;

  ~CircularByteBuffer() = default;

  /**
   * @brief Writes data to the buffer.
   *
   * This function will block if the buffer is full. Once space becomes available,
   * it will write as many bytes as possible.
   *
   * @param data A pointer to the data to be written.
   * @param dataLen The number of bytes to write.
   * @return The number of bytes actually written to the buffer.
   */
  size_t write(const std::byte* data, size_t dataLen);

  /**
   * @brief Reads data from the buffer.
   *
   * This function will block if the buffer is empty. Once data becomes available,
   * it will read as many bytes as possible, up to the provided `bytes_to_read`.
   *
   * @param buffer A pointer to the buffer where data will be stored.
   * @param dataLen The number of bytes to read.
   * @return The number of bytes actually read from the buffer.
   */
  size_t read(std::byte* buffer, size_t dataLen);

  /**
   * @brief Gets the current number of bytes available to read in the buffer.
   * @return The number of bytes.
   */
  size_t size() const;

  /**
   * @brief Gets the maximum capacity of the buffer.
   * @return The capacity in bytes.
   */
  size_t capacity() const;

 private:
  mutable std::mutex accessMutex;
  std::condition_variable condFull;
  std::condition_variable condEmpty;

  const size_t storageCapacity;
  std::vector<std::byte> buffer;
  size_t headPos{};      // Write position
  size_t tailPos{};      // Read position
  size_t currentSize{};  // Number of bytes currently in the buffer
};
}  // namespace bell::io
