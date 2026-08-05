#include "audio/PrefetchWorker.h"

#include <chrono>

#include "audio/ChunkFetcher.h"
#include "audio/RangeAlignment.h"
#include "bell/Logger.h"
#include "bell/http/Common.h"

using namespace cspot;

namespace {
// Same as StreamPlayer/FileProvider/EventLoop/ConnectStateHandler - this
// task's call depth (HTTP request build, TLS I/O, mbedTLS AES-CTR,
// fmt::format) matches theirs, not AudioSinkI2S's much shallower plain
// I2S DMA writes.
const int kTaskStackSize = 32 * 1024;
const int kTaskPriority = 5;
}  // namespace

PrefetchWorker::PrefetchWorker(std::shared_ptr<bell::HTTPClient> httpClient)
    : bell::Task("PrefetchWorker", kTaskStackSize, kTaskPriority),
      rangeFetcher(std::move(httpClient)) {
  startTask();
}

PrefetchWorker::~PrefetchWorker() {
  stopTask();
}

// Called by stopTask() only after it has already set taskRunning = false -
// relies on that ordering so a woken taskLoop() always observes
// taskRunning == false on its very next check of runTask()'s loop
// condition, even though its own cv_.wait() predicate is already
// satisfied by shuttingDown and so never truly blocks again.
void PrefetchWorker::wakeTask() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    shuttingDown = true;
  }
  cv_.notify_all();
}

void PrefetchWorker::requestPrefetch(Session session, size_t currentChunkIndex) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pendingSession = std::move(session);
    pendingChunkIndex = currentChunkIndex;
  }
  cv_.notify_one();
}

void PrefetchWorker::taskLoop() {
  Session session;
  size_t chunkIndex;
  {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return pendingSession.has_value() || shuttingDown; });
    if (shuttingDown) {
      return;
    }
    session = std::move(*pendingSession);
    pendingSession.reset();
    chunkIndex = pendingChunkIndex;
  }

  std::optional<size_t> totalChunks;
  if (session.totalWireSize > session.phaseAnchor) {
    totalChunks = (session.totalWireSize - session.phaseAnchor +
                  session.chunkSize - 1) /
                 session.chunkSize;
  }

  for (size_t idx : chunksToPrefetch(chunkIndex, session.depth, totalChunks)) {
    if (session.chunkCache->claim(idx) != ChunkCache::ClaimOutcome::MustFetch) {
      continue;  // already ready, already in flight, or window full
    }

    size_t desiredStart = session.phaseAnchor + idx * session.chunkSize;
    auto plan = planRange(desiredStart, session.chunkSize);

    // The last chunk of a track is usually shorter than a full chunkSize -
    // fetching it would just come back short (see fetchDecryptAndPublish
    // Chunk()'s own short-response handling). totalWireSize is already
    // known by the time any prefetching starts (set from the very first
    // fetch in CDNDataStream::open()), so this is knowable ahead of time -
    // no point spending a network round-trip on a result we can already
    // predict. The synchronous foreground path handles this exact
    // boundary correctly on its own (read()'s own remaining-bytes clamp),
    // so nothing is lost by skipping it here.
    if (plan.requestStart + plan.requestSize > session.totalWireSize) {
      BELL_LOG(debug, LOG_TAG,
               "Skipping prefetch of chunk {} (bytes={}-{}) - known to be "
               "the partial final chunk",
               idx, plan.requestStart, plan.requestStart + plan.requestSize - 1);
      session.chunkCache->cancel(idx);
      continue;
    }

    // Any failure is already logged (with detail) and the claim already
    // cancelled inside fetchDecryptAndPublishChunk() - nothing else to do
    // here either way, prefetch is always best-effort.
    (void)fetchDecryptAndPublishChunk(rangeFetcher, *session.aesCipher,
                                      *session.chunkCache, *session.cdnUrl,
                                      idx, plan);
  }
}
