#include "audio/ChunkCache.h"

#include <algorithm>
#include <chrono>

using namespace cspot;

ChunkCache::ChunkCache(size_t capacity) : capacity_(capacity) {}

ChunkCache::~ChunkCache() = default;

ChunkCache::Slot* ChunkCache::findSlot(size_t chunkIndex) {
  for (auto& slot : slots_) {
    if (slot.chunkIndex == chunkIndex) {
      return &slot;
    }
  }
  return nullptr;
}

const ChunkCache::Slot* ChunkCache::findSlot(size_t chunkIndex) const {
  for (const auto& slot : slots_) {
    if (slot.chunkIndex == chunkIndex) {
      return &slot;
    }
  }
  return nullptr;
}

std::shared_ptr<const std::vector<std::byte>> ChunkCache::tryGet(
    size_t chunkIndex) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const Slot* slot = findSlot(chunkIndex);
  return (slot && slot->state == SlotState::Ready) ? slot->data : nullptr;
}

ChunkCache::ClaimOutcome ChunkCache::claim(size_t chunkIndex) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (Slot* existing = findSlot(chunkIndex)) {
    return existing->state == SlotState::Ready ? ClaimOutcome::AlreadyReady
                                               : ClaimOutcome::AlreadyFetching;
  }

  if (slots_.size() >= capacity_) {
    // Evict the smallest-index *ready* slot to make room - never evict a
    // slot that's still Fetching, that would orphan an in-flight fetch.
    auto victim = slots_.end();
    for (auto it = slots_.begin(); it != slots_.end(); ++it) {
      if (it->state != SlotState::Ready) {
        continue;
      }
      if (victim == slots_.end() || it->chunkIndex < victim->chunkIndex) {
        victim = it;
      }
    }
    if (victim == slots_.end()) {
      return ClaimOutcome::WindowFull;
    }
    slots_.erase(victim);
  }

  slots_.push_back(Slot{chunkIndex, SlotState::Fetching, nullptr});
  return ClaimOutcome::MustFetch;
}

std::shared_ptr<const std::vector<std::byte>> ChunkCache::waitFor(
    size_t chunkIndex, std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(mutex_);

  auto notFetching = [this, chunkIndex]() {
    Slot* slot = findSlot(chunkIndex);
    return slot == nullptr || slot->state == SlotState::Ready;
  };

  if (timeout <= std::chrono::milliseconds::zero()) {
    cv_.wait(lock, notFetching);
  } else {
    cv_.wait_for(lock, timeout, notFetching);
  }

  Slot* slot = findSlot(chunkIndex);
  return (slot && slot->state == SlotState::Ready) ? slot->data : nullptr;
}

void ChunkCache::publish(size_t chunkIndex, std::vector<std::byte> data) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    Slot* slot = findSlot(chunkIndex);
    if (!slot) {
      return;  // evicted while the fetch was in flight - discard
    }
    slot->data =
        std::make_shared<const std::vector<std::byte>>(std::move(data));
    slot->state = SlotState::Ready;
  }
  cv_.notify_all();
}

void ChunkCache::cancel(size_t chunkIndex) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    Slot* slot = findSlot(chunkIndex);
    if (!slot || slot->state == SlotState::Ready) {
      return;  // nothing to cancel
    }
    slots_.erase(std::remove_if(slots_.begin(), slots_.end(),
                                [chunkIndex](const Slot& s) {
                                  return s.chunkIndex == chunkIndex;
                                }),
                slots_.end());
  }
  cv_.notify_all();
}

void ChunkCache::advanceWindow(size_t minChunkIndex) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    slots_.erase(std::remove_if(slots_.begin(), slots_.end(),
                                [minChunkIndex](const Slot& s) {
                                  return s.chunkIndex < minChunkIndex;
                                }),
                slots_.end());
  }
  cv_.notify_all();  // wake any waitFor() on an index that just got evicted
}

void ChunkCache::reset() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    slots_.clear();
  }
  cv_.notify_all();
}
