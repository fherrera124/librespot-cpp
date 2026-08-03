
#include "audio/CDNDataStream.h"

#include <algorithm>
#include <chrono>

#include "bell/Logger.h"
#include "bell/http/Common.h"
#include "bell/io/DataStream.h"

using namespace cspot;

namespace {
const size_t chunkSize = 32 * 1024L;  // 32KB chunks

// Small safety margin over whatever depth ReadAheadPolicy actually asks
// for - claim()'s eviction means an undersized window would just degrade
// to less effective prefetching, never a correctness issue, so this
// doesn't need to track the runtime-configurable depth exactly.
const size_t kChunkCacheCapacity = 4;

// How long requestRange() waits for a chunk PrefetchWorker is already
// fetching before giving up and doing its own independent fetch anyway.
// Bounded well under AudioSinkI2S's ~1.5s PCM ring buffer cushion so that
// even the worst case (wait times out, then a fresh fetch is still slow)
// doesn't reliably blow through it - real CDN range fetches observed on
// hardware run ~300ms typically, with occasional spikes past 3s.
const std::chrono::milliseconds kInFlightWaitTimeoutMs{1000};

// RAII: cancels a ChunkCache claim on any early return out of
// requestRange() unless publish() (which disarms it) is reached first -
// several failure exits below all need this, and repeating an explicit
// cancel() at each one is exactly the kind of thing a future added
// return would forget, permanently stranding a slot as "Fetching".
struct ChunkClaimGuard {
  ChunkCache* cache = nullptr;
  std::optional<size_t> idx;
  bool disarmed = false;

  ~ChunkClaimGuard() {
    if (idx && !disarmed) {
      cache->cancel(*idx);
    }
  }
};

// Every Spotify CDN audio file (confirmed for plain OGG_VORBIS_160, not
// just Opus, against librespot-cpp's CDNAudioFile - same IV above, same
// mechanism) is prefixed with a fixed-size proprietary header (loudness
// normalization gain/peak floats live at offsets 144/148 in there) before
// the real Ogg container begins - raw byte 0 of the HTTP response is NOT
// byte 0 of the Ogg stream. Reproduced on real hardware: every attempt to
// open a real track failed identically with "Not a Vorbis stream" - the
// Ogg/AES layer below (requestRange/AesCtrCipher) stays entirely in RAW
// (wire) coordinates and is otherwise correct; this offset is applied
// only at the public seek()/size()/position() boundary so OggContainer
// and everything above it sees byte 0 as the real start of the Ogg data.
const size_t kSpotifyHeaderSize = 167;
}  // namespace

CDNDataStream::CDNDataStream(std::shared_ptr<bell::HTTPClient> httpClient,
                             std::shared_ptr<PrefetchWorker> prefetchWorker)
    : rangeFetcher(std::move(httpClient)),
      prefetchWorker(std::move(prefetchWorker)),
      chunkCache(std::make_shared<ChunkCache>(kChunkCacheCapacity)) {}

CDNDataStream::~CDNDataStream() = default;

bell::Result<> CDNDataStream::open(const std::string& cdnUrl,
                                   const std::vector<std::byte>& decryptKey) {
  this->cdnUrl = std::make_shared<const std::string>(cdnUrl);

  // Reset sizes & state
  totalSize.reset();
  originalTotalSizeRaw = 0;
  tailRemainderBytes = 0;
  bytesInLastReadChunk = 0;
  chunkStartPosition = 0;
  pendingDiscardBack = 0;
  currentPosition = 0;
  resetPrefetchPhase();

  // Re-init (in case of reopen) - std::optional::emplace() destroys the
  // previous AesCtrCipher (if any) before constructing the new one.
  aesCipher.emplace(decryptKey);
  if (!aesCipher->hasValidKey()) {
    BELL_LOG(error, LOG_TAG, "Failed to set AES key");
    return bell::make_unexpected_errc<>(std::errc::bad_message);
  }
  // Separate instance for PrefetchWorker's own thread - never shared with
  // the one above (see AesCtrCipher's header comment on why).
  prefetchAesCipher = std::make_shared<AesCtrCipher>(decryptKey);
  if (!prefetchAesCipher->hasValidKey()) {
    BELL_LOG(error, LOG_TAG, "Failed to set AES key (prefetch cipher)");
    return bell::make_unexpected_errc<>(std::errc::bad_message);
  }

  lastReadChunk.resize(chunkSize);
  // Initialize reuse metadata
  bufferAlignedStart = bufferAlignedEnd = bufferVisibleStart =
      bufferVisibleEnd = 0;

  return {};
}

bool CDNDataStream::isSeekable() const {
  return true;
}

bool CDNDataStream::isInfinite() const {
  return false;
}

std::optional<size_t> CDNDataStream::size() const {
  // totalSize (member) is raw/wire size (includes the header) - expose
  // the logical, header-excluded size to callers (OggContainer etc).
  if (!totalSize) {
    return std::nullopt;
  }
  return (*totalSize > kSpotifyHeaderSize) ? (*totalSize - kSpotifyHeaderSize)
                                           : 0;
}

size_t CDNDataStream::position() const {
  // currentPosition (member) is raw/wire position - expose the logical,
  // header-excluded position to callers.
  return (currentPosition > kSpotifyHeaderSize)
             ? (currentPosition - kSpotifyHeaderSize)
             : 0;
}

bell::Result<> CDNDataStream::seek(size_t offset, SeekOrigin origin) {
  // Compute target position first (do not clobber state yet)
  size_t targetPos = currentPosition;
  if (origin == SeekOrigin::Begin) {
    // 'offset' is a logical (header-excluded) position from the caller;
    // requestRange()/AesCtrCipher below operate purely in raw/wire
    // coordinates, so translate here at the public boundary.
    targetPos = offset + kSpotifyHeaderSize;
  } else if (origin == SeekOrigin::Current) {
    targetPos = currentPosition + offset;
  } else {  // End
    if (!totalSize) {
      // Need a tail request first to discover size. Perform a probe using requestRange.
      return requestRange(offset, chunkSize, SeekOrigin::End);
    }
    if (offset > *totalSize) {
      return bell::make_unexpected_errc<>(std::errc::invalid_seek);
    }
    targetPos = *totalSize - offset;
  }

  // If we already have a decrypted buffer that fully covers the desired (single) seek position,
  // we can reuse it instead of issuing a new HTTP range request.
  // Current buffer user-visible coverage: [bufferVisibleStart, bufferVisibleEnd)
  if (bytesInLastReadChunk > 0 && targetPos >= bufferVisibleStart &&
      targetPos < bufferVisibleEnd) {

    // Derive the index (in lastReadChunk) where bufferVisibleStart resides.
    // chunkStartPosition currently points at the index of currentPosition.
    // Therefore: baseIndex = chunkStartPosition - (currentPosition - bufferVisibleStart).
    if (currentPosition >= bufferVisibleStart) {
      size_t consumedFromVisibleStart = currentPosition - bufferVisibleStart;
      size_t baseIndex = (chunkStartPosition >= consumedFromVisibleStart)
                             ? (chunkStartPosition - consumedFromVisibleStart)
                             : 0;  // safety (should not underflow)

      size_t offsetInsideVisible = targetPos - bufferVisibleStart;
      size_t newChunkStart = baseIndex + offsetInsideVisible;

      if (newChunkStart < bytesInLastReadChunk) {
        chunkStartPosition = newChunkStart;
        currentPosition = targetPos;
        // Successful in-buffer seek reuse
        return {};
      }
    }
  }

  // Miss: a new range is needed, possibly starting a new prefetch phase
  // (see requestRange()'s comment) - reset chunk state.
  bytesInLastReadChunk = 0;
  chunkStartPosition = 0;
  pendingDiscardBack = 0;
  currentPosition = targetPos;
  resetPrefetchPhase();

  return requestRange(currentPosition, chunkSize, SeekOrigin::Begin);
}

bell::Result<size_t> CDNDataStream::read(std::byte* outputBuffer,
                                         size_t outputBufferLen) {
  size_t totalCopied = 0;
  size_t toRead = outputBufferLen;

  while (toRead > 0) {
    // If total size known and at/end, break
    if (totalSize && currentPosition >= *totalSize) {
      break;
    }

    // Copy from current chunk
    size_t availableInChunk = (bytesInLastReadChunk > chunkStartPosition)
                                  ? (bytesInLastReadChunk - chunkStartPosition)
                                  : 0;

    if (availableInChunk > 0) {
      size_t toCopy = std::min(toRead, availableInChunk);
      std::copy(lastReadChunk.data() + chunkStartPosition,
                lastReadChunk.data() + chunkStartPosition + toCopy,
                outputBuffer + totalCopied);

      chunkStartPosition += toCopy;
      currentPosition += toCopy;
      totalCopied += toCopy;
      toRead -= toCopy;

      if (toRead == 0) {
        break;
      }
      continue;
    }

    // Need more data
    size_t desired = chunkSize;
    if (totalSize) {
      size_t remaining = *totalSize - currentPosition;
      if (remaining == 0)
        break;
      desired = std::min(desired, remaining);
    }

    auto res = requestRange(currentPosition, desired,
                            bell::io::DataStream::SeekOrigin::Begin);
    if (!res) {
      return bell::make_unexpected_errc<size_t>(std::errc::io_error);
    }
  }

  return totalCopied;
}

bell::Result<std::vector<std::byte>> CDNDataStream::readRawHeaderBytes(
    size_t maxBytes) {
  // requestRange()/AesCtrCipher operate in raw/wire coordinates (see this
  // class's own comment on kSpotifyHeaderSize) - offset 0 here is genuinely
  // wire byte 0, not the logical (header-excluded) byte 0 every other
  // caller of this class means.
  auto res = requestRange(0, maxBytes, SeekOrigin::Begin);
  if (!res) {
    return tl::make_unexpected(res.error());
  }

  size_t available = (bytesInLastReadChunk > chunkStartPosition)
                         ? (bytesInLastReadChunk - chunkStartPosition)
                         : 0;
  size_t toCopy = std::min(maxBytes, available);
  return std::vector<std::byte>(
      lastReadChunk.begin() + chunkStartPosition,
      lastReadChunk.begin() + chunkStartPosition + toCopy);
}

void CDNDataStream::resetPrefetchPhase() {
  phaseAnchor.reset();
  chunkCache->reset();
}

std::optional<size_t> CDNDataStream::chunkIndexInPhase(
    size_t desiredStart) const {
  if (!phaseAnchor || desiredStart < *phaseAnchor) {
    return std::nullopt;
  }
  size_t delta = desiredStart - *phaseAnchor;
  if (delta % chunkSize != 0) {
    return std::nullopt;
  }
  return delta / chunkSize;
}

bool CDNDataStream::adoptCachedChunk(const std::vector<std::byte>& data,
                                    const RangeRequestPlan& plan) {
  if (data.size() != plan.requestSize) {
    return false;
  }

  lastReadChunk = data;
  chunkStartPosition = plan.skipPrefix;
  bytesInLastReadChunk = plan.requestSize - plan.skipSuffix;
  currentPosition = plan.desiredStart;
  bufferAlignedStart = plan.requestStart;
  bufferAlignedEnd = plan.requestStart + plan.requestSize;
  bufferVisibleStart = plan.desiredStart;
  bufferVisibleEnd = plan.desiredStart + plan.desiredLength;
  return true;
}

void CDNDataStream::advancePrefetchWindow(size_t chunkIndex) {
  chunkCache->advanceWindow(chunkIndex);
  // .value() (not *phaseAnchor): every current call site only reaches
  // here after chunkIndexInPhase() already confirmed phaseAnchor is set,
  // but that guarantee lives outside this function - a future call site
  // that skips it should get a loud, debuggable throw here instead of
  // silently dereferencing an empty optional.
  prefetchWorker->requestPrefetch(
      PrefetchWorker::Session{chunkCache, cdnUrl, prefetchAesCipher,
                              chunkSize, phaseAnchor.value(),
                              originalTotalSizeRaw},
      chunkIndex);
}

std::pair<bool, std::optional<size_t>> CDNDataStream::tryServeFromCache(
    size_t desiredStart, size_t desiredLen) {
  // A "phase" is a run of chunkSize-sized fetches whose desiredStart
  // values form an exact arithmetic sequence phaseAnchor,
  // phaseAnchor+chunkSize, ... - true for this class's own sequential
  // access pattern (read() only asks for more once currentPosition
  // reaches the end of what it has), and PrefetchWorker computes byte
  // ranges for chunk n the same way via planRange(phaseAnchor +
  // n*chunkSize, chunkSize), so a cache hit here is always byte-for-byte
  // what a synchronous fetch would have produced.
  if (!phaseAnchor) {
    phaseAnchor = desiredStart;
  }

  auto idx = chunkIndexInPhase(desiredStart);
  if (!idx) {
    // desiredStart isn't reachable from phaseAnchor by whole chunkSize
    // steps - not expected from this class's own call sites, but if it
    // ever happens, start a fresh phase rather than risk mixing grids.
    phaseAnchor = desiredStart;
    chunkCache->reset();
    return {false, std::nullopt};
  }

  // Computed once, regardless of which branch below ends up needing it -
  // desiredStart/desiredLen don't change between them.
  auto cachePlan = planRange(desiredStart, desiredLen);
  auto outcome = chunkCache->claim(*idx);

  if (outcome == ChunkCache::ClaimOutcome::AlreadyReady) {
    if (auto cached = chunkCache->tryGet(*idx)) {
      if (adoptCachedChunk(*cached, cachePlan)) {
        advancePrefetchWindow(*idx);
        return {true, std::nullopt};
      }
    }
    // Size mismatch (shouldn't happen given PrefetchWorker's own check) -
    // fall through to a normal synchronous fetch instead of trusting a
    // possibly-corrupt cached buffer.
    return {false, std::nullopt};
  }

  if (outcome == ChunkCache::ClaimOutcome::AlreadyFetching) {
    // PrefetchWorker (or, rarely, a concurrent caller) is already fetching
    // this exact chunk - wait for it instead of duplicating a CDN round-
    // trip. Observed on real hardware: without this, requestRange() would
    // routinely re-request a range the worker was already mid-fetch on,
    // doubling network cost on a link where a single 32KB range can
    // already take 1-3+ seconds.
    auto waitStart = std::chrono::steady_clock::now();
    if (auto data = chunkCache->waitFor(*idx, kInFlightWaitTimeoutMs)) {
      if (adoptCachedChunk(*data, cachePlan)) {
        BELL_LOG(debug, LOG_TAG,
                 "Waited {} ms for in-flight prefetch of chunk at offset "
                 "{} - avoided a duplicate fetch",
                 std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - waitStart)
                     .count(),
                 desiredStart);
        advancePrefetchWindow(*idx);
        return {true, std::nullopt};
      }
    }
    BELL_LOG(debug, LOG_TAG,
             "In-flight prefetch of chunk at offset {} didn't finish "
             "within {} ms - falling back to an independent fetch",
             desiredStart, kInFlightWaitTimeoutMs.count());
    // Timed out, failed, or size mismatch - fall through to an
    // independent synchronous fetch, same as before this fix existed.
    return {false, std::nullopt};
  }

  if (outcome == ChunkCache::ClaimOutcome::MustFetch) {
    return {false, *idx};
  }

  // WindowFull: proceed unclaimed, same as before this fix.
  return {false, std::nullopt};
}

void CDNDataStream::publishAndAdvance(const RangeRequestPlan& plan,
                                      size_t alignedOffsetInBuffer,
                                      bool ownsClaimedSlot) {
  auto idx = chunkIndexInPhase(plan.desiredStart);
  if (!idx) {
    return;
  }
  if (ownsClaimedSlot) {
    chunkCache->publish(
        *idx, std::vector<std::byte>(
                 lastReadChunk.begin() + alignedOffsetInBuffer,
                 lastReadChunk.begin() + alignedOffsetInBuffer +
                     plan.requestSize));
  }
  advancePrefetchWindow(*idx);
}

bell::Result<> CDNDataStream::requestRange(size_t offset, size_t length,
                                           SeekOrigin origin) {
  // Determine HTTP Range header
  size_t desiredStart = offset;
  size_t desiredLen = length;

  bool tailRequest = (origin == bell::io::DataStream::SeekOrigin::End);

  if (origin == bell::io::DataStream::SeekOrigin::Current) {
    desiredStart = currentPosition + offset;
  }

  if (totalSize && origin != bell::io::DataStream::SeekOrigin::End) {
    if (desiredStart >= *totalSize) {
      return bell::make_unexpected_errc<>(std::errc::invalid_seek);
    }
    // Clamp desiredLen within remaining
    desiredLen = std::min(desiredLen, *totalSize - desiredStart);
  }

  // Fast path: this is exactly the "give me the next chunkSize-sized
  // window" shape PrefetchWorker fills in ahead of the read cursor - see
  // tryServeFromCache()'s own comment for what a "phase" is and why a
  // cache hit here is always byte-for-byte what a synchronous fetch would
  // have produced. Anything else (header probe, tail probe, the
  // undersized final chunk near EOF) skips this and falls straight to the
  // synchronous fetch below, exactly as before read-ahead existed.
  ChunkClaimGuard claimGuard;
  if (!tailRequest && desiredLen == chunkSize) {
    auto [served, claimedIdx] = tryServeFromCache(desiredStart, desiredLen);
    if (served) {
      return {};
    }
    if (claimedIdx) {
      // We now own this slot - claimGuard cancels it on any early return
      // below; the success path further down publishes it.
      claimGuard.cache = chunkCache.get();
      claimGuard.idx = *claimedIdx;
    }
  }

  // If it's a tail request (size unknown possibly), request a padded aligned tail.
  RangeRequestPlan plan;
  std::string rangeHeaderValue;
  if (tailRequest) {
    // Request enough bytes from end to cover alignment + desired length
    size_t provisionalAlignedTail =
        alignUp16(length + 16);  // +16 ensures room for prefix alignment
    rangeHeaderValue = fmt::format("bytes=-{}", provisionalAlignedTail);
  } else {
    plan = planRange(desiredStart, desiredLen);
    rangeHeaderValue = fmt::format("bytes={}-{}", plan.requestStart,
                                   plan.requestStart + plan.requestSize - 1);
  }

  auto startTime = std::chrono::steady_clock::now();
  auto fetchRes = rangeFetcher.fetch(*cdnUrl, rangeHeaderValue);
  auto elapsed = std::chrono::steady_clock::now() - startTime;
  totalRequestTimeMs +=
      std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

  if (!fetchRes) {
    BELL_LOG(error, LOG_TAG, "Fetch of {} failed: {}", rangeHeaderValue,
             fetchRes.error());
    return tl::make_unexpected(fetchRes.error());
  }
  BELL_LOG(
      debug, LOG_TAG, "Fetched {} in {} ms (total {} ms)", rangeHeaderValue,
      std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
      totalRequestTimeMs);

  size_t startVal = fetchRes->contentRangeStart;
  size_t totalRaw = fetchRes->contentRangeTotal;
  lastReadChunk = std::move(fetchRes->data);

  // Update total size info
  originalTotalSizeRaw = totalRaw;
  tailRemainderBytes = originalTotalSizeRaw % 16;
  size_t trimmed = originalTotalSizeRaw - tailRemainderBytes;
  totalSize = trimmed;

  size_t alignedOffsetInBuffer;
  if (tailRequest) {
    // Determine desired logical start for this tail read
    size_t desiredTailStart = (trimmed > length) ? (trimmed - length) : 0;

    // Build plan now that size is known.
    plan = planRange(desiredTailStart,
                     std::min(length, trimmed - desiredTailStart));

    // We requested 'bytes=-provisionalAlignedTail', actual startVal may be 0 or (totalRaw - provisionalAlignedTail)
    // Need to ensure we have enough prefix for alignment
    if (startVal > plan.requestStart) {
      // Not enough prefix fetched; re-issue with the now-known-correct
      // explicit aligned range (requestRange's own non-tail branch builds
      // the right Range header from plan.desiredStart/desiredLength).
      return requestRange(plan.desiredStart, plan.desiredLength,
                          bell::io::DataStream::SeekOrigin::Begin);
    }

    alignedOffsetInBuffer = plan.requestStart - startVal;
  } else {
    alignedOffsetInBuffer = plan.requestStart - startVal;
  }

  // The server may have returned fewer bytes than the aligned range we
  // asked for (short read/early EOF) - indexing past what actually came
  // back would be an out-of-bounds access on lastReadChunk.
  if (alignedOffsetInBuffer + plan.requestSize > lastReadChunk.size()) {
    BELL_LOG(error, LOG_TAG,
             "Short range response: got {} bytes, needed {} from offset {}",
             lastReadChunk.size(), plan.requestSize, alignedOffsetInBuffer);
    return bell::make_unexpected_errc<>(std::errc::io_error);
  }

  auto decryptRes =
      aesCipher->decrypt(lastReadChunk.data() + alignedOffsetInBuffer,
                         plan.requestSize, plan.requestStart);
  if (!decryptRes) {
    BELL_LOG(error, LOG_TAG, "Failed to decrypt data: {}",
             decryptRes.error());
    return decryptRes;
  }

  chunkStartPosition = alignedOffsetInBuffer + plan.skipPrefix;
  bytesInLastReadChunk =
      alignedOffsetInBuffer + plan.requestSize - plan.skipSuffix;
  currentPosition = plan.desiredStart;
  // Update reuse metadata
  bufferAlignedStart = plan.requestStart;
  bufferAlignedEnd = plan.requestStart + plan.requestSize;
  bufferVisibleStart = plan.desiredStart;
  bufferVisibleEnd = plan.desiredStart + plan.desiredLength;

  // Ensure indices are sane
  if (bytesInLastReadChunk < chunkStartPosition) {
    BELL_LOG(error, LOG_TAG, "Internal alignment error: start {} end {}",
             chunkStartPosition, bytesInLastReadChunk);
    return bell::make_unexpected_errc<>(std::errc::bad_message);
  }

  // Successful synchronous fetch of a cacheable chunk - now that
  // totalSize (and, if this was the very first one, phaseAnchor) are
  // known, let the prefetch worker look further ahead from here, and
  // publish what we fetched if we own the claim on it (the MustFetch case
  // above) - lets PrefetchWorker (or a future caller) see it as ready
  // instead of independently re-fetching what we just got ourselves.
  if (!tailRequest && desiredLen == chunkSize) {
    publishAndAdvance(plan, alignedOffsetInBuffer, claimGuard.idx.has_value());
    claimGuard.disarmed = true;
  }

  return {};
}
