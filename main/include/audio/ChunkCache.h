#pragma once

// Standard includes
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

namespace cspot {

/**
 * @brief Bounded, index-addressable cache of downloaded chunk bytes, used
 *        to coordinate a foreground reader with a background read-ahead
 *        fetcher.
 *
 * Pure in-memory data structure - no networking, no threads of its own.
 * Chunks are identified by an opaque index (the caller decides what a
 * "chunk" maps to, e.g. floor(byteOffset / chunkSize)). Holds at most
 * `capacity` distinct indices at a time (Fetching + Ready combined) -
 * advanceWindow() is how a caller tells it to drop everything behind the
 * current read cursor to make room for chunks further ahead.
 *
 * One mutex/condition_variable guards the whole (small) window rather
 * than one per chunk - unlike a design meant to track dozens of chunks in
 * flight, a window this size doesn't need finer-grained locking.
 */
class ChunkCache {
 public:
  enum class ClaimOutcome {
    MustFetch,       // caller now owns fetching this chunk
    AlreadyReady,     // already resident and ready - call tryGet()
    AlreadyFetching,  // someone else is fetching it - call waitFor()
    WindowFull,       // at capacity, nothing evictable - caller should back off
  };

  explicit ChunkCache(size_t capacity);
  ~ChunkCache();

  ChunkCache(const ChunkCache&) = delete;
  ChunkCache& operator=(const ChunkCache&) = delete;

  // Non-blocking. Returns the chunk's bytes if resident and ready, nullptr
  // otherwise (absent or still being fetched).
  std::shared_ptr<const std::vector<std::byte>> tryGet(size_t chunkIndex) const;

  // Attempts to become responsible for fetching chunkIndex. On MustFetch,
  // the caller must eventually call publish() or cancel() for this same
  // index - failing to do so leaves it permanently "Fetching" (and, if
  // the window is at capacity, ineligible for eviction).
  ClaimOutcome claim(size_t chunkIndex);

  // Blocks (up to 'timeout'; zero/default = wait indefinitely) while
  // chunkIndex is being fetched. Returns the bytes once ready, or nullptr
  // if it becomes absent (fetch failed/cancelled, or evicted) or the wait
  // times out.
  std::shared_ptr<const std::vector<std::byte>> waitFor(
      size_t chunkIndex,
      std::chrono::milliseconds timeout = std::chrono::milliseconds::zero());

  // Fetching -> Ready. No-op if chunkIndex isn't currently owned as
  // Fetching (e.g. it was evicted by advanceWindow()/reset() while the
  // fetch was in flight) - the fetched bytes are simply discarded.
  void publish(size_t chunkIndex, std::vector<std::byte> data);

  // Fetching -> absent (the fetch failed). No-op if not currently
  // Fetching. Wakes any waitFor() callers so they can retry via claim().
  void cancel(size_t chunkIndex);

  // Drops every resident chunk with index < minChunkIndex - keeps the
  // window's memory bounded as the read cursor advances. Safe to call
  // even if some of those are still Fetching (see publish()/cancel()'s
  // own no-op-if-evicted behavior above).
  void advanceWindow(size_t minChunkIndex);

  // Drops everything unconditionally (seek outside the window, track
  // change, shutdown).
  void reset();

 private:
  enum class SlotState { Fetching, Ready };

  struct Slot {
    size_t chunkIndex = 0;
    SlotState state = SlotState::Fetching;
    std::shared_ptr<const std::vector<std::byte>> data;
  };

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<Slot> slots_;
  size_t capacity_;

  // Both require mutex_ already held by the caller.
  Slot* findSlot(size_t chunkIndex);
  const Slot* findSlot(size_t chunkIndex) const;
};

}  // namespace cspot
