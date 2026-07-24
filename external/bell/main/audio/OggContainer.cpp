#include "bell/audio/OggContainer.h"

#include "bell/Logger.h"
#include "bell/Result.h"
#include "bell/audio/Common.h"
#include "ogg/ogg.h"

using namespace bell::audio;

namespace {
const int traillingBytesCount = 16 * 1024;  // 16 KiB
const int decodeBytesCount = 8 * 1024;      // 8 KiB

// rescales the number x from the range of [0,from] to [0,to] x is in the range [0,from] from, to are in the range [1, 1<<62-1]
ogg_int64_t rescalePosition(ogg_int64_t x, ogg_int64_t from, ogg_int64_t to) {
  ogg_int64_t frac = 0;
  ogg_int64_t ret = 0;
  int i;
  if (x >= from)
    return to;
  if (x <= 0)
    return 0;

  for (i = 0; i < 64; i++) {
    if (x >= from) {
      frac |= 1;
      x -= from;
    }
    x <<= 1;
    frac <<= 1;
  }

  for (i = 0; i < 64; i++) {
    if (frac & 1) {
      ret += to;
    }
    frac >>= 1;
    ret >>= 1;
  }

  return ret;
}
}  // namespace

bell::Result<> OggContainer::openForRead(
    std::shared_ptr<bell::io::DataStream> dataStream) {
  stream = std::move(dataStream);
  ogg_sync_init(&oggSyncState);

  // Seek the data source to last 16 KiB
  if (stream->isSeekable()) {
    auto seekRes = stream->seek(traillingBytesCount,
                                bell::io::DataStream::SeekOrigin::End);
    if (!seekRes) {
      BELL_LOG(warn, LOG_TAG, "Could not seek to end - {}", seekRes.error());
      return tl::make_unexpected(seekRes.error());
    }

    bool reachedEnd = false;
    while (!reachedEnd) {
      auto pageRes = readNextPage();
      if (pageRes.error() == bell::audio::Errc::EndOfStream) {
        reachedEnd = true;
      } else if (!pageRes) {
        BELL_LOG(warn, LOG_TAG, "Could not read page during init - {}",
                 pageRes.error());
        return tl::make_unexpected(pageRes.error());
      }
    }

    totalFrames = ogg_page_granulepos(&oggPage);
    BELL_LOG(info, LOG_TAG, "Stream has {} total frames", totalFrames);

    // Seek back to start
    seekRes = stream->seek(0, bell::io::DataStream::SeekOrigin::Begin);
    if (!seekRes) {
      BELL_LOG(warn, LOG_TAG, "Could not seek back to start - {}",
               seekRes.error());
      return tl::make_unexpected(seekRes.error());
    }
  }

  // Read the first page to get the stream serial number.
  auto firstPageRes = readNextPage();
  if (!firstPageRes) {
    BELL_LOG(error, "OggContainer", "Could not read the first Ogg page.");
    return tl::make_unexpected(audio::Errc::InvalidFormat);
  }
  streamSerialNo = ogg_page_serialno(&oggPage);

  ogg_stream_init(&oggStreamState, streamSerialNo);

  if (ogg_stream_pagein(&oggStreamState, &oggPage) < 0) {
    BELL_LOG(error, "OggContainer", "ogg_stream_pagein failed on first page.");
    return tl::make_unexpected(audio::Errc::CodecError);
  }

  dataStartOffset = stream->position();

  BELL_LOG(info, LOG_TAG, "Opened Ogg stream with serial {}", streamSerialNo);
  return {};
}

bell::Result<> OggContainer::readNextPage() {
  while (true) {
    long r = ogg_sync_pageseek(&oggSyncState, &oggPage);
    if (r > 0) {
      return {};
    }
    if (r < 0) {
      // skipped bytes; keep looping
      continue;
    }

    // Need more data
    if (stream->size() && stream->position() >= *stream->size()) {
      return tl::make_unexpected(audio::Errc::EndOfStream);
    }

    char* buffer = ogg_sync_buffer(&oggSyncState, decodeBytesCount);
    if (!buffer) {
      return tl::make_unexpected(audio::Errc::CodecError);
    }
    auto readRes =
        stream->read(reinterpret_cast<std::byte*>(buffer), decodeBytesCount);
    if (!readRes)
      return tl::make_unexpected(readRes.error());
    if (*readRes == 0)
      return tl::make_unexpected(audio::Errc::EndOfStream);
    ogg_sync_wrote(&oggSyncState, *readRes);
  }

  return {};
}

bell::Result<EncodedPacket> OggContainer::readNextPacket() {
  while (ogg_stream_packetout(&oggStreamState, &packet) != 1) {
    if (auto pageRes = readNextPage(); !pageRes) {
      return tl::make_unexpected(pageRes.error());
    }
    // Only feed pages for our serial
    if (ogg_page_serialno(&oggPage) != streamSerialNo) {
      // Skip other logical streams (chained files)
      continue;
    }
    if (ogg_stream_pagein(&oggStreamState, &oggPage) < 0) {
      BELL_LOG(warn, LOG_TAG, "ogg_stream_pagein failed; skipping page.");
      continue;
    }
  }

  EncodedPacket frame;
  frame.data = {reinterpret_cast<std::byte*>(packet.packet),
                static_cast<size_t>(packet.bytes)};
  frame.streamIdx = 0;

  if (packet.granulepos > 0) {
    currentFrame = packet.granulepos;  // page-level timing; decoder may refine
  }
  frame.timestamp = currentFrame;
  return frame;
}

bell::Result<> OggContainer::seekToFrame(size_t frameIndex,
                                         size_t allowedDistance) {
  if (!stream->isSeekable() || !stream->size()) {
    return tl::make_unexpected(audio::Errc::OperationNotSupported);
  }

  if (frameIndex > totalFrames) {
    frameIndex = totalFrames;
  }

  // Bisection search variables
  uint64_t searchBegin = dataStartOffset;
  uint64_t searchEnd = *stream->size();
  int64_t bestOffset = -1;
  uint64_t beginTime = 0;
  uint64_t endTime = totalFrames;

  // Coarse seek with binary search
  // This loop quickly finds the page immediately preceding the target frame.
  while (searchBegin < searchEnd) {
    ogg_int64_t bisect;
    if (searchEnd - searchBegin < decodeBytesCount) {
      bisect = searchBegin;
    } else {
      // Make an intelligent guess based on frame position
      bisect = searchBegin + rescalePosition(frameIndex - beginTime,
                                             endTime - beginTime,
                                             searchEnd - searchBegin);
      // Back up a bit to ensure we don't land inside the target page
      if (bisect >= decodeBytesCount) {
        bisect -= decodeBytesCount;
      }
    }

    BELL_LOG(debug, LOG_TAG,
             "Bisection seek: frameIndex={} begin={} end={} bisect={} "
             "beginTime={} endTime={}",
             frameIndex, searchBegin, searchEnd, bisect, beginTime, endTime);
    if (auto seekRes = stream->seek(bisect, io::DataStream::SeekOrigin::Begin);
        !seekRes) {
      return tl::make_unexpected(seekRes.error());
    }

    ogg_int64_t lastPageOffset = -1;
    while (stream->position() < searchEnd) {
      lastPageOffset = stream->position();
      auto pageRes = readNextPage();
      if (pageRes.error() == audio::Errc::EndOfStream) {
        searchEnd = lastPageOffset;  // No more pages in this range
        break;
      }
      if (!pageRes)
        return tl::make_unexpected(pageRes.error());

      if (ogg_page_serialno(&oggPage) == streamSerialNo) {
        ogg_int64_t granulepos = ogg_page_granulepos(&oggPage);
        if (granulepos != -1) {
          if (granulepos < (ogg_int64_t)frameIndex) {
            // This page is a candidate, it's before our target
            bestOffset = lastPageOffset;
            beginTime = granulepos;
            searchBegin = stream->position();
          } else {
            // We've overshot the target
            searchEnd = lastPageOffset;
            endTime = granulepos;
            break;  // Narrow the search to the lower half
          }
        }
      }
    }
  }

  // If no suitable page was found, seek to the beginning of the data.
  ogg_int64_t seekTo = (bestOffset != -1) ? bestOffset : dataStartOffset;
  if (auto seekRes = stream->seek(seekTo, io::DataStream::SeekOrigin::Begin);
      !seekRes) {
    return tl::make_unexpected(seekRes.error());
  }

  // Reset all decoder state
  ogg_sync_reset(&oggSyncState);
  ogg_stream_reset(&oggStreamState);
  currentFrame = beginTime;

  // Fine tuning to exact page
  if (frameIndex > currentFrame &&
      (frameIndex - currentFrame) <= allowedDistance) {
    // We landed close enough, no need to fine-tune.
    return {};
  }

  // Discard packets until we reach the target frame
  while (currentFrame < frameIndex) {
    auto packetRes = readNextPacket();
    if (!packetRes) {
      // Reached EOF or error before finding the frame
      return (packetRes.error() == audio::Errc::EndOfStream)
                 ? bell::Result<>()
                 : tl::make_unexpected(packetRes.error());
    }
  }

  return {};
}

void OggContainer::close() {
  ogg_stream_destroy(&oggStreamState);
  ogg_sync_destroy(&oggSyncState);
}

uint64_t OggContainer::tellFrame() const {
  return currentFrame;
}

uint64_t OggContainer::getTotalFrames() {
  return totalFrames;
}
