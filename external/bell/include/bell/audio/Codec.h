#pragma once

// Standard includes
#include <cstddef>
#include <optional>
#include <variant>

// Own headers
#include "bell/Result.h"
#include "bell/audio/Common.h"

namespace bell::audio {

struct OpusConfig {
  // Preferred samples per packet (optional)
  std::optional<uint32_t> samplesPerPacket;

  // Opus application, defaults to OPUS_APPLICATION_AUDIO
  std::optional<int> application;

  // This will be the maximum decoded frame size in bytes, in a single decode call.
  // The default is set to be enough to store 100ms of audio at 48kHz, 2 channels, 16-bit.
  size_t bufferSize = (100 * 48000 / 1000) * 2 * 2;
};

struct AACConfig {
  // Preferred samples per packet (optional)
  std::optional<uint32_t> samplesPerPacket;

  int transportType = 0;

  enum class Profile : uint8_t {
    AAC_LC,

    // TODO: Support for other AAC types
    // Currently only AAC_LC is supported, due to its patent being expired
    HE_AAC,     // High Efficiency AAC
    HE_AAC_V2,  // High Efficiency AAC v2
    AAC_LD,     // Low Delay
    AAC_ELD,    // Enhanced Low Delay
    BSAC,       // Bit-Sliced Arithmetic Coding
    USAC,       // Unified Speech and Audio Coding
  } profile = Profile::AAC_LC;

  // Bitrate configuration
  struct Bitrate {
    enum class Mode { VBR, CBR } mode = Mode::VBR;
    int quality = 3;               // For VBR (0=low, 5=high)
    std::optional<size_t> cbrBps;  // For CBR (bits/sec)
  } bitrate;

  // Optional decoder audio specific config
  std::optional<std::vector<uint8_t>> decoderAudioSpecificConfig;
};

struct TremorConfig {};

using CodecConfig = std::variant<OpusConfig, AACConfig, TremorConfig>;

/**
 * Base class for audio codecs
 */
class Codec {
 public:
  Codec() = default;
  virtual ~Codec() = default;

  struct EncodeResult {
    std::vector<EncodedPacket> packets;
    size_t consumedInputBytes = 0;
  };

  struct DecodeResult {
    tcb::span<std::byte> pcm;
    size_t consumedInputBytes = 0;
  };

  enum class SetupStatus : uint8_t {
    Incomplete,  // More headers/data needed
    Ready        // Codec is ready to decode
  };

  /**
   * @brief Setups the codec in encode mode
   *
   * @param audioFormat PCM audio format to use with this codec
   * @param codecSpecificConfig codec-specific configuration, see implementation details
   * @return Result of the operation, might return Errc::UnsupportedConfig if codec does not support the requested config
   */
  virtual bell::Result<> setupEncode(
      const AudioFormat& audioFormat,
      const CodecConfig& codecSpecificConfig = {}) = 0;

  /**
   * @brief Setups the codec in decode mode
   *
   * @param audioFormat PCM audio format to use with this codec
   * @param codecSpecificConfig codec-specific configuration, see implementation details
   * @return Result of the operation, might return Errc::UnsupportedConfig if codec does not support the requested config
   */
  virtual bell::Result<> setupDecode(
      const AudioFormat& audioFormat,
      const CodecConfig& codecSpecificConfig = {}) = 0;

  /**
   * @brief Setups the codec in decode mode, by reading the codec-specific configuration from the encoded input
   *
   * @remark In case this method returns false, call it again with the next encoded input data. Some codecs require multiple headers to be parsed.
   * @param encodedInput Pointer to the encoded input data
   * @param inputLength Length of the encoded input data
   * @return Setup result, might return Errc::UnsupportedConfig if codec does not support the requested config
   */
  virtual bell::Result<SetupStatus> setupDecodeFromHeaders(
      tcb::span<const std::byte> encodedInput) = 0;

  /**
   * @brief Returns the configured audio format
   */
  virtual bell::audio::Format getAudioFormat() const = 0;

  /**
   * @brief Encodes the input PCM data, returning the encoded data
   *
   * @param pcmInput Span of the input PCM data, in the format specified by the audioFormat
   * @return EncodeResult contains the encoded data, or error
   */
  virtual bell::Result<EncodeResult> encode(
      tcb::span<const std::byte> pcmInput) = 0;

  /**
   * @brief Decodes the input encoded data, returning the decoded PCM data
   *
   * @param encodedInput Span of the encoded input data
   * @return DecodeResult contains the decoded PCM data, or error
   */
  virtual bell::Result<DecodeResult> decode(
      tcb::span<const std::byte> encodedInput) = 0;
};
}  // namespace bell::audio

namespace bell {
using AudioCodec = audio::Codec;

using OpusConfig = audio::OpusConfig;
using AACConfig = audio::AACConfig;
using TremorConfig = audio::TremorConfig;
}  // namespace bell
