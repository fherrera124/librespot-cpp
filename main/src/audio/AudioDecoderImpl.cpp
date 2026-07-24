#include "tracks/AudioDecoder.h"

#include "audio/CDNDataStream.h"
#include "bell/Logger.h"
#include "bell/audio/OggContainer.h"
#include "bell/audio/TremorVorbisCodec.h"
#include "bell/http/Client.h"

using namespace cspot;

namespace {
const char* LOG_TAG = "AudioDecoderImpl";
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
      } else {
        BELL_LOG(error, LOG_TAG, "Failed to read Ogg packet: {}",
                 packetRes.error());
      }
      return;
    }

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
    codec.reset();
    container.reset();
    dataStream.reset();
  }

  bool isEOF() const override { return eof; }

 private:
  AudioOutputCallback outputCallback;
  std::shared_ptr<bell::HTTPClient> httpClient;
  std::shared_ptr<bell::io::DataStream> dataStream;
  std::unique_ptr<bell::audio::OggContainer> container;
  std::unique_ptr<bell::TremorVorbisCodec> codec;
  SpotifyId currentTrackId;
  bool isOpenFlag = false;
  bool eof = false;
};

std::unique_ptr<AudioDecoder> cspot::createAudioDecoder(
    AudioOutputCallback outputCallback) {
  return std::make_unique<AudioDecoderImpl>(std::move(outputCallback));
}
