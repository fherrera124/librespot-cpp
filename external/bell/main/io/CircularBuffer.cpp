#include "CircularBuffer.h"

#include <algorithm>  // for min
#include <chrono>     // for milliseconds

using namespace bell;

CircularBuffer::CircularBuffer(size_t dataCapacity) {
  this->dataCapacity = dataCapacity;
  buffer = std::vector<uint8_t>(dataCapacity);
  this->dataSemaphore = std::make_unique<bell::WrappedSemaphore>(5);
};

size_t CircularBuffer::writeLocked(const uint8_t* data, size_t bytes) {
  if (bytes == 0)
    return 0;

  size_t bytesToWrite = std::min(bytes, dataCapacity - dataSize);
  // Write in a single step
  if (bytesToWrite <= dataCapacity - endIndex) {
    memcpy(buffer.data() + endIndex, data, bytesToWrite);
    endIndex += bytesToWrite;
    if (endIndex == dataCapacity)
      endIndex = 0;
  }

  // Write in two steps
  else {
    size_t firstChunkSize = dataCapacity - endIndex;
    memcpy(buffer.data() + endIndex, data, firstChunkSize);
    size_t secondChunkSize = bytesToWrite - firstChunkSize;
    memcpy(buffer.data(), data + firstChunkSize, secondChunkSize);
    endIndex = secondChunkSize;
  }

  dataSize += bytesToWrite;
  return bytesToWrite;
}

size_t CircularBuffer::write(const uint8_t* data, size_t bytes) {
  std::unique_lock<std::mutex> lock(bufferMutex);
  size_t written = writeLocked(data, bytes);
  lock.unlock();
  if (written > 0) {
    dataAvailableCV.notify_one();
  }

  // this->dataSemaphore->give();
  return written;
}

void CircularBuffer::emptyBuffer() {
  std::lock_guard<std::mutex> guard(bufferMutex);
  begIndex = 0;
  dataSize = 0;
  endIndex = 0;
}

void CircularBuffer::emptyExcept(size_t sizeToSet) {
  std::lock_guard<std::mutex> guard(bufferMutex);
  if (sizeToSet > dataSize)
    sizeToSet = dataSize;
  dataSize = sizeToSet;
  endIndex = begIndex + sizeToSet;
  if (endIndex > dataCapacity) {
    endIndex -= dataCapacity;
  }
}

size_t CircularBuffer::readLocked(uint8_t* data, size_t bytes) {
  if (bytes == 0)
    return 0;

  size_t bytesToRead = std::min(bytes, dataSize);

  // Read in a single step
  if (bytesToRead <= dataCapacity - begIndex) {
    memcpy(data, buffer.data() + begIndex, bytesToRead);
    begIndex += bytesToRead;
    if (begIndex == dataCapacity)
      begIndex = 0;
  }
  // Read in two steps
  else {
    size_t firstChunkSize = dataCapacity - begIndex;
    memcpy(data, buffer.data() + begIndex, firstChunkSize);
    size_t secondChunkSize = bytesToRead - firstChunkSize;
    memcpy(data + firstChunkSize, buffer.data(), secondChunkSize);
    begIndex = secondChunkSize;
  }

  dataSize -= bytesToRead;
  return bytesToRead;
}

size_t CircularBuffer::read(uint8_t* data, size_t bytes) {
  std::unique_lock<std::mutex> lock(bufferMutex);
  size_t bytesRead = readLocked(data, bytes);
  lock.unlock();
  if (bytesRead > 0) {
    spaceAvailableCV.notify_one();
  }
  return bytesRead;
}

size_t CircularBuffer::writeBlocking(const uint8_t* data, size_t bytes,
                                     uint32_t timeoutMs) {
  std::unique_lock<std::mutex> lock(bufferMutex);
  if (dataSize == dataCapacity) {
    spaceAvailableCV.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                              [this] { return dataSize < dataCapacity; });
  }
  size_t written = writeLocked(data, bytes);
  lock.unlock();
  if (written > 0) {
    dataAvailableCV.notify_one();
  }
  return written;
}

size_t CircularBuffer::readBlocking(uint8_t* data, size_t bytes,
                                    uint32_t timeoutMs) {
  std::unique_lock<std::mutex> lock(bufferMutex);
  if (dataSize == 0) {
    dataAvailableCV.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                             [this] { return dataSize > 0; });
  }
  size_t bytesRead = readLocked(data, bytes);
  lock.unlock();
  if (bytesRead > 0) {
    spaceAvailableCV.notify_one();
  }
  return bytesRead;
}
