#pragma once

#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "BellTask.h"
#include "TrackQueue.h"  // for QueuedTrack's full definition

namespace bell {
class WrappedSemaphore;
};

namespace cspot {
struct Context;
class AccessKeyFetcher;

// Drives QueuedTrack's network state machine (processTrack()'s per-state
// dispatch, moved here from TrackQueue) on its own background task. Never
// touches TrackQueue's preloadedTracks/tracksMutex directly - it asks for
// what it needs through two constructor-injected callbacks, the same way
// it already depends on AccessKeyFetcher's behavior without ever touching
// AccessKeyFetcher's internals. See TrackQueue.h's own comment on why the
// split stops here (TrackQueue keeps owning preloadedTracks) instead of
// handing the deque itself across the class boundary.
class TrackLoader : public bell::Task {
 public:
  using SnapshotFn = std::function<std::deque<std::shared_ptr<QueuedTrack>>()>;
  using TopUpFn = std::function<void()>;

  // processSemaphore: shared with TrackQueue, not owned exclusively here -
  // TrackQueue's insertNext()/replaceUpcoming() also give() it (to wake
  // this task early after a queue edit, instead of waiting out
  // runTask()'s own 100ms poll), same as before the split. A
  // WrappedSemaphore is itself a thread-safe handoff primitive (unlike a
  // plain mutex), so sharing it across this boundary doesn't reintroduce
  // the anti-pattern QueuedTrack's mutex parameter had - see TrackQueue.h.
  TrackLoader(std::shared_ptr<cspot::Context> ctx,
              std::shared_ptr<cspot::AccessKeyFetcher> accessKeyFetcher,
              std::shared_ptr<bell::WrappedSemaphore> processSemaphore,
              SnapshotFn snapshotPreloaded, TopUpFn tryTopUpLookahead);
  ~TrackLoader();

  void runTask() override;
  void stopTask();

 private:
  std::shared_ptr<cspot::Context> ctx;
  std::shared_ptr<cspot::AccessKeyFetcher> accessKeyFetcher;
  std::shared_ptr<bell::WrappedSemaphore> processSemaphore;
  SnapshotFn snapshotPreloaded;
  TopUpFn tryTopUpLookahead;
  std::mutex runningMutex;

  // PB scratch buffers - reused across tracks by stepLoadMetadata()'s
  // pb_release()/pbDecode() (same pattern as before the split).
  Track pbTrack;
  Episode pbEpisode;

  std::string accessKey;
  bool isRunning = false;

  void processTrack(std::shared_ptr<QueuedTrack> track);
};
}  // namespace cspot
