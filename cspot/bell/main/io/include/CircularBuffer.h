#pragma once

#include <condition_variable>  // for condition_variable
#include <cstdint>  // for uint8_t
#include <cstring>  // for size_t
#include <memory>   // for unique_ptr
#include <mutex>    // for mutex
#include <vector>   // for vector

#include "WrappedSemaphore.h"  // for WrappedSemaphore

namespace bell {
class CircularBuffer {
 public:
  CircularBuffer(size_t dataCapacity);

  std::unique_ptr<bell::WrappedSemaphore> dataSemaphore;

  size_t size() const { return dataSize; }

  size_t capacity() const { return dataCapacity; }

  size_t write(const uint8_t* data, size_t bytes);
  size_t read(uint8_t* data, size_t bytes);
  void emptyBuffer();
  void emptyExcept(size_t size);

  // Blocks (real condition_variable wait, not polling) until at least one
  // byte is available or timeoutMs elapses. Returns bytes actually read
  // (possibly 0 on timeout).
  size_t readBlocking(uint8_t* data, size_t bytes, uint32_t timeoutMs);
  // Blocks until at least one byte of space is free or timeoutMs elapses,
  // then writes whatever fits. Returns bytes actually written (possibly 0
  // on timeout with the buffer still full).
  size_t writeBlocking(const uint8_t* data, size_t bytes, uint32_t timeoutMs);

 private:
  size_t writeLocked(const uint8_t* data, size_t bytes);
  size_t readLocked(uint8_t* data, size_t bytes);

  std::mutex bufferMutex;
  std::condition_variable dataAvailableCV;
  std::condition_variable spaceAvailableCV;
  size_t begIndex = 0;
  size_t endIndex = 0;
  size_t dataSize = 0;
  size_t dataCapacity = 0;
  std::vector<uint8_t> buffer;
};
}  // namespace bell
