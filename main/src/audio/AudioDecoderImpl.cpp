#include "tracks/AudioDecoder.h"

#include <algorithm>
#include <atomic>
#include <optional>

#include "audio/CDNDataStream.h"
#include "audio/SpotifySeekTable.h"
#include "bell/Logger.h"
#include "bell/audio/OggContainer.h"
#include "bell/audio/TremorVorbisCodec.h"
#include "bell/http/Client.h"
#include "bell/utils/Utils.h"

using namespace cspot;

namespace {
const char* LOG_TAG = "AudioDecoderImpl";
const int kMaxConsecutiveReadErrors = 5;
const uint32_t kReadErrorBackoffMs = 100;
// The real header (confirmed against a hardware capture) is exactly 167
// bytes - comfortable headroom for other formats/bitrates without
// fetching much more than needed.
const size_t kHeaderProbeSize = 256;
}

class AudioDecoderImpl : public cspot::AudioDecoder {
 public:
  explicit AudioDecoderImpl(AudioOutputCallback outputCallback)
      : outputCallback(std::move(outputCallback)),
        httpClient(std::make_shared<bell::HTTPClient>()) {}

  bell::Result<> openStream(const std::string& cdnUrl,
                            const std::vector<std::byte>& decryptKey,
                            const SpotifyId& trackId) override {
    resetStream();
    currentTrackId = trackId;

    auto stream = std::make_shared<CDNDataStream>(httpClient);
    auto openRes = stream->open(cdnUrl, decryptKey);
    if (!openRes) {
      BELL_LOG(error, LOG_TAG, "Failed to open CDN stream: {}",
               openRes.error());
      return tl::make_unexpected(openRes.error());
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
      return tl::make_unexpected(containerRes.error());
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
        return tl::make_unexpected(packetRes.error());
      }
      auto headerRes = codec->setupDecodeFromHeaders(packetRes->data);
      if (!headerRes) {
        BELL_LOG(error, LOG_TAG, "Failed to parse Vorbis header packet {}: {}",
                 i, headerRes.error());
        resetStream();
        return tl::make_unexpected(headerRes.error());
      }
    }

    auto format = codec->getAudioFormat();
    if (format.getSampleRateValue() != 44100 || format.getNumChannels() != 2) {
      BELL_LOG(warn, LOG_TAG,
               "Vorbis stream is {}Hz/{}ch - AudioSinkI2S assumes "
               "44100Hz/2ch, audio will sound wrong",
               format.getSampleRateValue(), format.getNumChannels());
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

    outputCallback(decodeRes->pcm, currentTrackId);
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
      return tl::make_unexpected(seekRes.error());
    }

    // A seek can land before EOF even if we'd already hit it (or clear a
    // transient read-error streak) - let processPacket() resume from here
    // instead of staying stuck in whatever state it was in before the seek.
    eof = false;
    consecutiveReadErrors = 0;
    return {};
  }

 private:
  AudioOutputCallback outputCallback;
  std::shared_ptr<bell::HTTPClient> httpClient;
  std::shared_ptr<bell::io::DataStream> dataStream;
  std::unique_ptr<bell::audio::OggContainer> container;
  std::unique_ptr<bell::TremorVorbisCodec> codec;
  std::optional<SpotifySeekTable> seekTable;
  SpotifyId currentTrackId;
  // isOpen()/isEOF() are read from StreamPlayer's player thread without
  // holding playbackMutex (deliberately, to avoid blocking flush/queue
  // handling on the EventLoop thread during a blocking processPacket()
  // call) while being written from whichever thread calls openStream()/
  // resetStream() under that same mutex - atomic for cross-thread
  // visibility, not for compound-operation safety.
  std::atomic<bool> isOpenFlag{false};
  std::atomic<bool> eof{false};
  // Same cross-thread pattern as isOpenFlag/eof above: written in
  // processPacket() (player thread, outside the mutex), reset in
  // resetStream() (can run on the EventLoop thread).
  std::atomic<int> consecutiveReadErrors{0};
};

std::unique_ptr<AudioDecoder> cspot::createAudioDecoder(
    AudioOutputCallback outputCallback) {
  return std::make_unique<AudioDecoderImpl>(std::move(outputCallback));
}
