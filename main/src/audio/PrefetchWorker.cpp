#include "audio/PrefetchWorker.h"

#include <chrono>

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

PrefetchWorker::PrefetchWorker(std::shared_ptr<bell::HTTPClient> httpClient,
                               std::shared_ptr<ReadAheadPolicy> policy)
    : bell::Task("PrefetchWorker", kTaskStackSize, kTaskPriority),
      policy(std::move(policy)),
      rangeFetcher(std::move(httpClient)) {
  startTask();
}

PrefetchWorker::~PrefetchWorker() {
  // stopTask() flips taskRunning but has no way to wake cv_.wait() below -
  // do that ourselves first, or a taskLoop() parked with nothing to
  // prefetch would block this destructor forever. See the header's own
  // comment on shuttingDown.
  {
    std::lock_guard<std::mutex> lock(mutex_);
    shuttingDown = true;
  }
  cv_.notify_all();
  stopTask();
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

  for (size_t idx : policy->chunksToPrefetch(chunkIndex, totalChunks)) {
    if (session.chunkCache->claim(idx) != ChunkCache::ClaimOutcome::MustFetch) {
      continue;  // already ready, already in flight, or window full
    }

    size_t desiredStart = session.phaseAnchor + idx * session.chunkSize;
    auto plan = planRange(desiredStart, session.chunkSize);
    size_t rangeEnd = plan.requestStart + plan.requestSize - 1;

    // The last chunk of a track is usually shorter than a full chunkSize -
    // fetching it would just come back short and get cancelled below
    // anyway (see the short-response check further down). totalWireSize
    // is already known by the time any prefetching starts (set from the
    // very first fetch in CDNDataStream::open()), so this is knowable
    // ahead of time - no point spending a network round-trip on a result
    // we can already predict. The synchronous foreground path handles
    // this exact boundary correctly on its own (read()'s own remaining-
    // bytes clamp), so nothing is lost by skipping it here.
    if (plan.requestStart + plan.requestSize > session.totalWireSize) {
      BELL_LOG(debug, LOG_TAG,
               "Skipping prefetch of chunk {} (bytes={}-{}) - known to be "
               "the partial final chunk",
               idx, plan.requestStart, rangeEnd);
      session.chunkCache->cancel(idx);
      continue;
    }

    auto startTime = std::chrono::steady_clock::now();
    auto fetchRes = rangeFetcher.fetch(
        *session.cdnUrl, fmt::format("bytes={}-{}", plan.requestStart, rangeEnd));
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - startTime)
                        .count();

    if (!fetchRes) {
      BELL_LOG(debug, LOG_TAG,
               "Prefetch of chunk {} (bytes={}-{}) failed after {} ms: {}",
               idx, plan.requestStart, rangeEnd, elapsedMs, fetchRes.error());
      session.chunkCache->cancel(idx);
      continue;
    }
    if (fetchRes->data.size() < plan.requestSize) {
      BELL_LOG(debug, LOG_TAG,
               "Prefetch of chunk {} (bytes={}-{}) got a short response "
               "({} of {} bytes) after {} ms",
               idx, plan.requestStart, rangeEnd, fetchRes->data.size(),
               plan.requestSize, elapsedMs);
      session.chunkCache->cancel(idx);
      continue;
    }
    BELL_LOG(debug, LOG_TAG, "Prefetched chunk {} (bytes={}-{}) in {} ms", idx,
             plan.requestStart, rangeEnd, elapsedMs);

    auto decryptRes = session.aesCipher->decrypt(
        fetchRes->data.data(), plan.requestSize, plan.requestStart);
    if (!decryptRes) {
      BELL_LOG(debug, LOG_TAG, "Prefetch decrypt failed for chunk {}: {}", idx,
               decryptRes.error());
      session.chunkCache->cancel(idx);
      continue;
    }

    session.chunkCache->publish(idx, std::move(fetchRes->data));
  }
}
