#include "tracks/AudioDecoder.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <optional>

#include "audio/CDNDataStream.h"
#include "audio/PrefetchWorker.h"
#include "audio/RangeAlignment.h"
#include "audio/ReadAheadPolicy.h"
#include "audio/SpotifySeekTable.h"
#include "bell/Logger.h"
#include "bell/audio/OggContainer.h"
#include "bell/audio/TremorVorbisCodec.h"
#include "bell/http/Client.h"
#include "bell/utils/Utils.h"
#include "nonstd/expected.hpp"

using namespace cspot;

namespace {
const char* LOG_TAG = "AudioDecoderImpl";
const int kMaxConsecutiveReadErrors = 5;
const uint32_t kReadErrorBackoffMs = 100;
// The real header (confirmed against a hardware capture) is exactly 167
// bytes - comfortable headroom for other formats/bitrates without
// fetching much more than needed.
const size_t kHeaderProbeSize = 256;

// Approximate encoded bitrate per Spotify's own Vorbis quality tiers,
// converted to bytes/sec - used to turn a target chunk *duration* into a
// concrete byte count per track (FileProvider may resolve a lower quality
// than requested if a track doesn't offer it, so this has to run per-open,
// not once at construction). Falls back to OGG_VORBIS_160's rate for any
// non-Vorbis format (out of scope - see FileProvider's own selection,
// which only ever picks among the three Vorbis tiers today).
size_t bytesPerSecond(AudioFormat format) {
  switch (format) {
    case AudioFormat_OGG_VORBIS_96:
      return 12 * 1000;
    case AudioFormat_OGG_VORBIS_320:
      return 40 * 1000;
    case AudioFormat_OGG_VORBIS_160:
    default:
      return 20 * 1000;
  }
}
}

class AudioDecoderImpl : public cspot::AudioDecoder {
 public:
  explicit AudioDecoderImpl(std::shared_ptr<AudioSink> audioSink,
                            size_t prefetchDepth,
                            std::chrono::milliseconds targetChunkDuration)
      : audioSink(std::move(audioSink)),
        httpClient(std::make_shared<bell::HTTPClient>()),
        // One long-lived worker for this decoder's whole lifetime, reused
        // across tracks - see PrefetchWorker's own header comment for why
        // (mirrors AudioSinkI2S's own bell::Task, not spun up per track).
        prefetchWorker(std::make_shared<PrefetchWorker>(
            httpClient,
            std::make_shared<FixedDepthReadAheadPolicy>(prefetchDepth))),
        targetChunkDuration(targetChunkDuration) {}

  bell::Result<> openStream(const std::string& cdnUrl,
                            const std::vector<std::byte>& decryptKey,
                            const SpotifyId&, AudioFormat format) override {
    resetStream();

    // Derived per-track from targetChunkDuration and format's bitrate, 16-byte aligned (AES-CTR block size).
    size_t chunkSize = alignUp16(static_cast<size_t>(targetChunkDuration.count()) *
                                 bytesPerSecond(format) / 1000);
    BELL_LOG(info, LOG_TAG, "chunkSize={} bytes for format={} (target {}ms)",
             chunkSize, static_cast<int>(format), targetChunkDuration.count());

    auto stream =
        std::make_shared<CDNDataStream>(httpClient, prefetchWorker, chunkSize);
    auto openRes = stream->open(cdnUrl, decryptKey);
    if (!openRes) {
      BELL_LOG(error, LOG_TAG, "Failed to open CDN stream: {}",
               openRes.error());
      return nonstd::make_unexpected(openRes.error());
    }

    // Best-effort: a missing/unparseable seek table just means seekToMs()
    // falls back to bisection search below, not an open failure.
    auto headerRes = stream->readRawHeaderBytes(kHeaderProbeSize);
    if (headerRes) {
      seekTable = SpotifySeekTable::tryParse(*headerRes);
    }
    BELL_LOG(info, LOG_TAG, "Spotify seek table {}",
             seekTable ? "found" : "not found - will use bisection search");

    dataStream = stream;

    container = std::make_unique<bell::audio::OggContainer>();
    auto containerRes = container->openForRead(dataStream);
    if (!containerRes) {
      BELL_LOG(error, LOG_TAG, "Failed to open Ogg container: {}",
               containerRes.error());
      resetStream();
      return nonstd::make_unexpected(containerRes.error());
    }

    codec = std::make_unique<bell::TremorVorbisCodec>();

    // The first 3 packets of a Vorbis stream are always the id/comment/
    // setup headers - setupDecodeFromHeaders() self-initializes on the
    // first call (needs to run before any setupDecode() call, which
    // would otherwise mark the stream as already-initialized and break
    // its beginning-of-stream detection on the id header).
    for (int i = 0; i < 3; i++) {
      auto packetRes = container->readNextPacket();
      if (!packetRes) {
        BELL_LOG(error, LOG_TAG, "Failed to read Vorbis header packet {}: {}",
                 i, packetRes.error());
        resetStream();
        return nonstd::make_unexpected(packetRes.error());
      }
      auto headerRes = codec->setupDecodeFromHeaders(packetRes->data);
      if (!headerRes) {
        BELL_LOG(error, LOG_TAG, "Failed to parse Vorbis header packet {}: {}",
                 i, headerRes.error());
        resetStream();
        return nonstd::make_unexpected(headerRes.error());
      }
    }

    auto pcmFormat = codec->getAudioFormat();
    if (pcmFormat.getSampleRateValue() != 44100 ||
        pcmFormat.getNumChannels() != 2) {
      BELL_LOG(warn, LOG_TAG,
               "Vorbis stream is {}Hz/{}ch - AudioSinkI2S assumes "
               "44100Hz/2ch, audio will sound wrong",
               pcmFormat.getSampleRateValue(), pcmFormat.getNumChannels());
    }

    isOpenFlag = true;
    return {};
  }

  void processPacket() override {
    if (!isOpenFlag || eof) {
      return;
    }

    auto packetRes = container->readNextPacket();
    if (!packetRes) {
      if (packetRes.error() == bell::audio::Errc::EndOfStream) {
        eof = true;
        return;
      }

      int errorCount = ++consecutiveReadErrors;
      if (errorCount >= kMaxConsecutiveReadErrors) {
        BELL_LOG(error, LOG_TAG,
                 "Giving up after {} consecutive read errors, last: {}",
                 errorCount, packetRes.error());
        eof = true;
        return;
      }

      BELL_LOG(error, LOG_TAG, "Failed to read Ogg packet ({}/{}): {}",
               errorCount, kMaxConsecutiveReadErrors, packetRes.error());
      bell::utils::sleepMs(kReadErrorBackoffMs);
      return;
    }
    consecutiveReadErrors = 0;

    auto decodeRes = codec->decode(packetRes->data);
    if (!decodeRes) {
      // NotEnoughBytes is expected while Vorbis's windowing lookahead
      // fills up right after the headers - not a real error.
      if (decodeRes.error() != bell::audio::Errc::NotEnoughBytes) {
        BELL_LOG(error, LOG_TAG, "Failed to decode Vorbis packet: {}",
                 decodeRes.error());
      }
      return;
    }

    audioSink->feedPCMFrames(
        reinterpret_cast<const uint8_t*>(decodeRes->pcm.data()),
        decodeRes->pcm.size());
  }

  bool isOpen() const override { return isOpenFlag; }

  void resetStream() override {
    isOpenFlag = false;
    eof = false;
    consecutiveReadErrors = 0;
    codec.reset();
    container.reset();
    dataStream.reset();
    seekTable.reset();
  }

  bool isEOF() const override { return eof; }

  bell::Result<> seekToMs(int64_t positionMs) override {
    if (!isOpenFlag) {
      return bell::make_unexpected_errc(std::errc::invalid_argument);
    }

    auto sampleRate = codec->getAudioFormat().getSampleRateValue();
    auto frameIndex = static_cast<size_t>(
        std::max<int64_t>(positionMs, 0) * sampleRate / 1000);

    // Table lookup is O(1) (one HTTP range request at the resulting
    // offset); bisection costs several round-trips to search for that
    // same offset. Same fallback either way once we have a byte offset -
    // land on a valid page, resync, fine-tune forward if needed.
    auto seekRes =
        seekTable ? container->seekToByteOffset(
                        seekTable->getBytePosition(frameIndex), frameIndex)
                  : container->seekToFrame(frameIndex);
    if (!seekRes) {
      BELL_LOG(error, LOG_TAG, "Failed to seek to {}ms: {}", positionMs,
               seekRes.error());
      return nonstd::make_unexpected(seekRes.error());
    }

    // A seek can land before EOF even if we'd already hit it (or clear a
    // transient read-error streak) - let processPacket() resume from here
    // instead of staying stuck in whatever state it was in before the seek.
    eof = false;
    consecutiveReadErrors = 0;
    return {};
  }

 private:
  std::shared_ptr<AudioSink> audioSink;
  std::shared_ptr<bell::HTTPClient> httpClient;
  std::shared_ptr<PrefetchWorker> prefetchWorker;
  const std::chrono::milliseconds targetChunkDuration;
  std::shared_ptr<bell::io::DataStream> dataStream;
  std::unique_ptr<bell::audio::OggContainer> container;
  std::unique_ptr<bell::TremorVorbisCodec> codec;
  std::optional<SpotifySeekTable> seekTable;
  // isOpen()/isEOF() read without playbackMutex from the player thread
  // (avoids blocking flush/queue handling); written under that mutex from
  // openStream()/resetStream(). Atomic for visibility, not compound-op safety.
  std::atomic<bool> isOpenFlag{false};
  std::atomic<bool> eof{false};
  // Same cross-thread pattern as isOpenFlag/eof above: written in
  // processPacket() (player thread, outside the mutex), reset in
  // resetStream() (can run on the EventLoop thread).
  std::atomic<int> consecutiveReadErrors{0};
};

std::unique_ptr<AudioDecoder> cspot::createAudioDecoder(
    std::shared_ptr<AudioSink> audioSink, size_t prefetchDepth,
    std::chrono::milliseconds targetChunkDuration) {
  return std::make_unique<AudioDecoderImpl>(std::move(audioSink),
                                            prefetchDepth,
                                            targetChunkDuration);
}
