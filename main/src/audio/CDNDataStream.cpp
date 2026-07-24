
#include "audio/CDNDataStream.h"

#include <chrono>
#include <iostream>

#include "bell/Logger.h"
#include "bell/http/Client.h"
#include "bell/http/Common.h"
#include "bell/io/DataStream.h"

using namespace cspot;

namespace {
size_t chunkSize = 32 * 1024L;  // 32KB chunks

// Base IV for AES decryption, incremented per block
const std::array<uint8_t, 16> aesIVBase = {
    0x72, 0xe0, 0x67, 0xfb, 0xdd, 0xcb, 0xcf, 0x77,
    0xeb, 0xe8, 0xbc, 0x64, 0x3f, 0x63, 0x0d, 0x93,
};

// Every Spotify CDN audio file (confirmed for plain OGG_VORBIS_160, not
// just Opus, against librespot-cpp's CDNAudioFile - same IV above, same
// mechanism) is prefixed with a fixed-size proprietary header (loudness
// normalization gain/peak floats live at offsets 144/148 in there) before
// the real Ogg container begins - raw byte 0 of the HTTP response is NOT
// byte 0 of the Ogg stream. Reproduced on real hardware: every attempt to
// open a real track failed identically with "Not a Vorbis stream" - the
// Ogg/AES layer below (requestRange/decryptData) stays entirely in RAW
// (wire) coordinates and is otherwise correct; this offset is applied
// only at the public seek()/size()/position() boundary so OggContainer
// and everything above it sees byte 0 as the real start of the Ogg data.
const size_t kSpotifyHeaderSize = 167;
}  // namespace

CDNDataStream::CDNDataStream(std::shared_ptr<bell::HTTPClient> httpClient)
    : httpClient(std::move(httpClient)) {

  // Initialize the AES context and IV
  mbedtls_aes_init(&aesCtx);
  mbedtls_mpi_init(&aesIV);
}

CDNDataStream::~CDNDataStream() {
  activeResponse.reset();
  // Free the AES context and IV
  mbedtls_aes_free(&aesCtx);
  mbedtls_mpi_free(&aesIV);
}

bell::Result<> CDNDataStream::open(const std::string& cdnUrl,
                                   const std::vector<std::byte>& decryptKey) {

  // Store key (optional reuse)
  this->decryptKey = decryptKey;

  // Reset sizes & state
  totalSize.reset();
  originalTotalSizeRaw = 0;
  tailRemainderBytes = 0;
  bytesInLastReadChunk = 0;
  chunkStartPosition = 0;
  pendingDiscardBack = 0;
  currentPosition = 0;
  ivPosition = 0;
  activeResponse.reset();

  // Re-init AES contexts (in case of reopen)
  mbedtls_aes_free(&aesCtx);
  mbedtls_aes_init(&aesCtx);
  mbedtls_mpi_free(&aesIV);
  mbedtls_mpi_init(&aesIV);

  // Set the AES key for decryption
  if (mbedtls_aes_setkey_enc(
          &aesCtx, reinterpret_cast<const uint8_t*>(decryptKey.data()),
          decryptKey.size() * 8) != 0) {
    BELL_LOG(error, LOG_TAG, "Failed to set AES key");
    return bell::make_unexpected_errc<>(std::errc::bad_message);
  }

  // Set the IV to the base value
  int mbedtlsRes =
      mbedtls_mpi_read_binary(&aesIV, aesIVBase.data(), aesIVBase.size());

  if (mbedtlsRes != 0) {
    BELL_LOG(error, LOG_TAG, "Failed to initialize AES IV, mbedtls error: {}",
             mbedtlsRes);
    return bell::make_unexpected_errc<>(std::errc::bad_message);
  }

  lastReadChunk.resize(chunkSize);
  // Initialize reuse metadata
  bufferAlignedStart = bufferAlignedEnd = bufferVisibleStart =
      bufferVisibleEnd = 0;

  auto req = bell::HTTPRequest::create(bell::http::Method::GET, cdnUrl);
  if (!req) {
    return tl::make_unexpected(req.error());
  }

  req->operationTimeoutMs = 3000;
  req->headers = {};
  this->httpRequest = *req;

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
    // requestRange()/decryptData() below operate purely in raw/wire
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

  // Miss: need a new range; reset chunk state
  bytesInLastReadChunk = 0;
  chunkStartPosition = 0;
  pendingDiscardBack = 0;
  currentPosition = targetPos;

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

CDNDataStream::RangeRequestPlan CDNDataStream::planRange(size_t desiredStart,
                                                         size_t desiredLength) {
  RangeRequestPlan plan;
  plan.desiredStart = desiredStart;
  plan.desiredLength = desiredLength;

  plan.requestStart = alignDown16(desiredStart);
  plan.skipPrefix = desiredStart - plan.requestStart;

  size_t totalVisible = plan.skipPrefix + desiredLength;
  plan.requestSize = alignUp16(totalVisible);
  plan.skipSuffix = plan.requestSize - totalVisible;

  return plan;
}

bool CDNDataStream::canReuse(size_t offset, size_t length) const {
  return (bytesInLastReadChunk > 0) && (offset >= bufferVisibleStart) &&
         (offset + length) <= bufferVisibleEnd;
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

  // If it's a tail request (size unknown possibly), request a padded aligned tail.
  RangeRequestPlan plan;
  size_t provisionalAlignedTail = 0;
  if (tailRequest) {
    // Request enough bytes from end to cover alignment + desired length
    provisionalAlignedTail =
        alignUp16(length + 16);  // +16 ensures room for prefix alignment
    httpRequest.headers["Range"] =
        fmt::format("bytes=-{}", provisionalAlignedTail);
  } else {
    plan = planRange(desiredStart, desiredLen);
    httpRequest.headers["Range"] =
        fmt::format("bytes={}-{}", plan.requestStart,
                    plan.requestStart + plan.requestSize - 1);
  }

  // Reset old response
  activeResponse.reset();
  BELL_LOG(debug, LOG_TAG, "Requesting range: {}",
           httpRequest.headers["Range"]);

  auto startTime = std::chrono::system_clock::now();
  auto response = httpClient->rawRequest(httpRequest);
  if (!response) {
    BELL_LOG(error, LOG_TAG, "HTTP request error: {}", response.error());
    return tl::make_unexpected(response.error());
  }
  activeResponse = *response;

  auto* stream = activeResponse->stream();
  // Ensure buffer size
  if (lastReadChunk.size() < *response->contentLength) {
    lastReadChunk.resize(*response->contentLength);
  }
  stream->read(reinterpret_cast<char*>(lastReadChunk.data()),
               *response->contentLength);

  auto elapsed = std::chrono::system_clock::now() - startTime;
  totalRequestTimeMs +=
      std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
  BELL_LOG(
      debug, LOG_TAG, "Range request took {} ms, total time = {} ms",
      std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
      totalRequestTimeMs);

  if (stream->fail() && !stream->eof()) {
    return bell::make_unexpected_errc<>(std::errc::io_error);
  }

  if (!activeResponse->headers.contains("Content-Range")) {
    return bell::make_unexpected_errc<>(std::errc::bad_message);
  }

  auto rangeHeader = activeResponse->headers.at("Content-Range");
  // Expected format: bytes START-END/TOTAL
  size_t spacePos = rangeHeader.find(' ');
  size_t dashPos = rangeHeader.find('-');
  size_t slashPos = rangeHeader.find('/');
  size_t startVal = 0;
  size_t totalRaw = 0;
  try {
    if (spacePos != std::string::npos && dashPos != std::string::npos &&
        slashPos != std::string::npos) {
      startVal = static_cast<size_t>(std::stoull(
          rangeHeader.substr(spacePos + 1, dashPos - (spacePos + 1))));

      totalRaw =
          static_cast<size_t>(std::stoull(rangeHeader.substr(slashPos + 1)));
    } else {
      BELL_LOG(error, LOG_TAG, "Invalid Content-Range header: {}", rangeHeader);
      return bell::make_unexpected_errc<>(std::errc::bad_message);
    }
  } catch (const std::exception& e) {
    BELL_LOG(error, LOG_TAG, "Failed parsing Content-Range: {} ({})",
             rangeHeader, e.what());
    return bell::make_unexpected_errc<>(std::errc::bad_message);
  }

  bytesInLastReadChunk = static_cast<size_t>(stream->gcount());

  // Update total size info
  originalTotalSizeRaw = totalRaw;
  tailRemainderBytes = originalTotalSizeRaw % 16;
  size_t trimmed = originalTotalSizeRaw - tailRemainderBytes;
  totalSize = trimmed;

  if (tailRequest) {
    // Determine desired logical start for this tail read
    size_t desiredTailStart = (trimmed > length) ? (trimmed - length) : 0;

    // Build plan now that size is known.
    plan = planRange(desiredTailStart,
                     std::min(length, trimmed - desiredTailStart));

    // We requested 'bytes=-provisionalAlignedTail', actual startVal may be 0 or (totalRaw - provisionalAlignedTail)
    // Need to ensure we have enough prefix for alignment
    if (startVal > plan.requestStart) {
      // Not enough prefix fetched; fall back to re-issuing with proper aligned explicit range
      size_t newStart = plan.requestStart;
      size_t newEnd = plan.requestStart + plan.requestSize - 1;
      httpRequest.headers["Range"] =
          fmt::format("bytes={}-{}", newStart, newEnd);
      BELL_LOG(debug, LOG_TAG, "Re-issuing tail request for alignment: {}",
               httpRequest.headers["Range"]);
      return requestRange(plan.desiredStart, plan.desiredLength,
                          bell::io::DataStream::SeekOrigin::Begin);
    }

    // Decrypt only the aligned portion we will expose
    size_t alignedOffsetInBuffer = plan.requestStart - startVal;
    auto decryptRes = decryptData(lastReadChunk.data() + alignedOffsetInBuffer,
                                  plan.requestSize, plan.requestStart);
    if (!decryptRes) {
      BELL_LOG(error, LOG_TAG, "Failed to decrypt data (tail): {}",
               decryptRes.error());
      return decryptRes;
    }

    chunkStartPosition = alignedOffsetInBuffer + plan.skipPrefix;
    bytesInLastReadChunk =
        alignedOffsetInBuffer + plan.requestSize - plan.skipSuffix;
    currentPosition = plan.desiredStart;
    // Update reuse metadata (tail request)
    bufferAlignedStart = plan.requestStart;
    bufferAlignedEnd = plan.requestStart + plan.requestSize;
    bufferVisibleStart = plan.desiredStart;
    bufferVisibleEnd = plan.desiredStart + plan.desiredLength;
  } else {
    // Normal forward/aligned read
    auto decryptRes =
        decryptData(lastReadChunk.data() + (plan.requestStart - startVal),
                    plan.requestSize, plan.requestStart);
    if (!decryptRes) {
      BELL_LOG(error, LOG_TAG, "Failed to decrypt data: {}",
               decryptRes.error());
      return decryptRes;
    }

    chunkStartPosition = (plan.requestStart - startVal) + plan.skipPrefix;
    bytesInLastReadChunk =
        (plan.requestStart - startVal) + plan.requestSize - plan.skipSuffix;
    currentPosition = plan.desiredStart;
    // Update reuse metadata (normal request)
    bufferAlignedStart = plan.requestStart;
    bufferAlignedEnd = plan.requestStart + plan.requestSize;
    bufferVisibleStart = plan.desiredStart;
    bufferVisibleEnd = plan.desiredStart + plan.desiredLength;
  }

  // Ensure indices are sane
  if (bytesInLastReadChunk < chunkStartPosition) {
    BELL_LOG(error, LOG_TAG, "Internal alignment error: start {} end {}",
             chunkStartPosition, bytesInLastReadChunk);
    return bell::make_unexpected_errc<>(std::errc::bad_message);
  }

  return {};
}

bell::Result<> CDNDataStream::decryptData(std::byte* data, size_t size,
                                          size_t position) {
  if (size == 0) {
    return {};
  }

  const size_t alignedStart = position & ~size_t(15);
  size_t intraBlockOffset = position - alignedStart;
  size_t remaining = size;
  auto* buf = reinterpret_cast<uint8_t*>(data);

  // Reset IV to base then advance by blockIndex
  int res = mbedtls_mpi_read_binary(&aesIV, aesIVBase.data(), aesIVBase.size());
  if (res != 0) {
    BELL_LOG(error, LOG_TAG, "Failed to reset AES IV, mbedtls error: {}", res);
    return bell::make_unexpected_errc(std::errc::bad_message);
  }

  size_t blockIndex = alignedStart / 16;
  if (blockIndex > 0) {
    res =
        mbedtls_mpi_add_int(&aesIV, &aesIV, static_cast<long long>(blockIndex));
    if (res != 0) {
      BELL_LOG(error, LOG_TAG, "Failed to advance AES IV, mbedtls error: {}",
               res);
      return bell::make_unexpected_errc(std::errc::bad_message);
    }
  }

  std::array<uint8_t, 16> counterBlock{};
  res = mbedtls_mpi_write_binary(&aesIV, counterBlock.data(),
                                 counterBlock.size());
  if (res != 0) {
    BELL_LOG(error, LOG_TAG, "Failed to export AES IV, mbedtls error: {}", res);
    return bell::make_unexpected_errc(std::errc::bad_message);
  }

  while (remaining > 0) {
    std::array<uint8_t, 16> keystream{};
    if (mbedtls_aes_crypt_ecb(&aesCtx, MBEDTLS_AES_ENCRYPT, counterBlock.data(),
                              keystream.data()) != 0) {
      BELL_LOG(error, LOG_TAG, "Failed to generate CTR keystream block");
      return bell::make_unexpected_errc(std::errc::bad_message);
    }

    size_t take = std::min<size_t>(16 - intraBlockOffset, remaining);
    for (size_t i = 0; i < take; ++i) {
      buf[i] = static_cast<uint8_t>(buf[i]) ^ keystream[intraBlockOffset + i];
    }

    buf += take;
    remaining -= take;
    intraBlockOffset = 0;  // only applies to first block

    // Increment counter (big-endian)
    for (int i = 15; i >= 0; --i) {
      if (++counterBlock[i])
        break;
    }
  }

  ivPosition = static_cast<int32_t>(alignedStart);
  return {};
}
