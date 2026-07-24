#include "bell/io/CircularByteBuffer.h"

#include <algorithm>  // For std::min

using namespace bell::io;

CircularByteBuffer::CircularByteBuffer(size_t capacity)
    : storageCapacity(capacity), buffer(capacity) {}

size_t CircularByteBuffer::write(const std::byte* data, size_t dataLen) {
  if (data == nullptr || dataLen == 0) {
    return 0;
  }

  std::unique_lock<std::mutex> lock(accessMutex);

  // Wait until there is at least one byte of space.
  condFull.wait(lock, [this] { return currentSize < storageCapacity; });

  // Determine how many bytes we can actually write.
  size_t availableSpace = storageCapacity - currentSize;
  size_t bytesToCopy = std::min(dataLen, availableSpace);

  if (bytesToCopy == 0) {
    return 0;  // Should not happen due to wait, but as a safeguard.
  }

  // Copy data, handling wrap-around if necessary.
  // First part of the copy (from head to the end of the vector)
  size_t firstChunkSize = std::min(bytesToCopy, storageCapacity - headPos);
  memcpy(&buffer[headPos], data, firstChunkSize);

  // Second part of the copy (if it wraps around)
  if (firstChunkSize < bytesToCopy) {
    size_t secondChunkSize = bytesToCopy - firstChunkSize;
    memcpy(buffer.data(), data + firstChunkSize, secondChunkSize);
  }

  headPos = (headPos + bytesToCopy) % storageCapacity;
  currentSize += bytesToCopy;

  lock.unlock();           // Unlock before notifying to reduce contention.
  condEmpty.notify_all();  // Notify all waiting readers.

  return bytesToCopy;
}

size_t CircularByteBuffer::read(std::byte* buffer, size_t dataLen) {
  if (buffer == nullptr || dataLen == 0) {
    return 0;
  }

  std::unique_lock<std::mutex> lock(accessMutex);
  condEmpty.wait(lock, [this] { return currentSize > 0; });

  size_t bytesToCopy = std::min(dataLen, currentSize);

  if (bytesToCopy == 0) {
    return 0;
  }

  size_t firstChunkSize = std::min(bytesToCopy, storageCapacity - tailPos);
  memcpy(buffer, &this->buffer[tailPos], firstChunkSize);

  if (firstChunkSize < bytesToCopy) {
    size_t secondChunkSize = bytesToCopy - firstChunkSize;
    memcpy(buffer + firstChunkSize, &this->buffer[0], secondChunkSize);
  }

  tailPos = (tailPos + bytesToCopy) % storageCapacity;
  currentSize -= bytesToCopy;

  lock.unlock();
  condFull.notify_all();

  return bytesToCopy;
}

size_t CircularByteBuffer::size() const {
  std::scoped_lock lock(accessMutex);
  return currentSize;
}

size_t CircularByteBuffer::capacity() const {
  return storageCapacity;
}
