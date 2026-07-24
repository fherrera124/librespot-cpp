#include "bell/audio/Codec.h"

// Standard includes
#include "bell/audio/Common.h"
#include "ivorbiscodec.h"

namespace bell::audio {

namespace internal {
// Map Tremor error codes to std::error_code
struct tremor_error_category : public std::error_category {
  const char* name() const noexcept override { return "Tremor"; }
  std::string message(int ev) const noexcept override {
    switch (ev) {
      case OV_FALSE:
        return "False return value";
      case OV_EOF:
        return "End of file";
      case OV_HOLE:
        return "Hole in data";
      case OV_EREAD:
        return "Read error";
      case OV_EFAULT:
        return "Internal fault";
      case OV_EIMPL:
        return "Not implemented";
      case OV_EINVAL:
        return "Invalid argument";
      case OV_ENOTVORBIS:
        return "Not a Vorbis stream";
      case OV_EBADHEADER:
        return "Bad header";
      case OV_EVERSION:
        return "Version mismatch";
      case OV_ENOTAUDIO:
        return "Not audio data";
      case OV_EBADPACKET:
        return "Bad packet";
      case OV_EBADLINK:
        return "Bad link";
      case OV_ENOSEEK:
        return "Not seekable";
      default:
        return "Unknown tremor error";
    }
  }
};

const tremor_error_category tremorErrorCategory{};
}  // namespace internal

// std::error_code helper
inline std::error_code make_tremor_error_code(int err) {
  return {static_cast<int>(err), internal::tremorErrorCategory};
};

class TremorVorbisCodec : public Codec {
 public:
  TremorVorbisCodec() = default;
  ~TremorVorbisCodec() override;

  // Delete copy constructor and copy assignment operator
  TremorVorbisCodec(const TremorVorbisCodec&) = delete;
  TremorVorbisCodec& operator=(const TremorVorbisCodec&) = delete;

  // Codec implementation
  bell::Result<> setupEncode(const AudioFormat& audioFormat,
                             const CodecConfig& codecSpecificConfig) override;
  bell::Result<> setupDecode(const AudioFormat& audioFormat,
                             const CodecConfig& codecSpecificConfig) override;
  bell::Result<SetupStatus> setupDecodeFromHeaders(
      tcb::span<const std::byte> encodedInput) override;
  bell::Result<EncodeResult> encode(
      tcb::span<const std::byte> pcmInput) override;
  bell::Result<DecodeResult> decode(
      tcb::span<const std::byte> encodedInput) override;

  audio::Format getAudioFormat() const override { return audioFormat; }

 private:
  const char* LOG_TAG = "audio::TremorVorbisCodec";

  // Codec configuration
  TremorConfig config{};

  AudioFormat audioFormat;

  vorbis_info info{};
  vorbis_comment comment{};
  vorbis_dsp_state dspState{};
  vorbis_block block{};

  bool infoInitialized = false;
  int headerPacketCount = 0;
};
}  // namespace bell::audio

namespace bell {
using TremorVorbisCodec = audio::TremorVorbisCodec;
}  // namespace bell
