#include "bell/audio/Codec.h"

// Standard includes
#include <vector>

// Library includes
#include "aacdecoder_lib.h"
#include "aacenc_lib.h"
#include "bell/audio/Common.h"

namespace bell::audio {
namespace internal {
// Map AAC dec codes to std::error_code
struct fdk_aacdec_error_category : public std::error_category {
  const char* name() const noexcept override { return "FDKAACDecoder"; }
  std::string message(int ev) const noexcept override {
    switch (static_cast<AAC_DECODER_ERROR>(ev)) {
      case AAC_DEC_OK:
        return "No error occurred. Output buffer is valid and error free.";
      case AAC_DEC_OUT_OF_MEMORY:
        return "Heap returned NULL pointer. Output buffer is invalid.";
      case AAC_DEC_UNKNOWN:
        return "Error condition is of unknown reason, or from a another "
               "module. Output buffer is invalid.";
      case AAC_DEC_TRANSPORT_SYNC_ERROR:
        return "The transport decoder had synchronization problems. Do not "
               "exit decoding. Just feed new bitstream data.";
      case AAC_DEC_NOT_ENOUGH_BITS:
        return "The input buffer ran out of bits.";
      case AAC_DEC_INVALID_HANDLE:
        return "The handle passed to the function call was invalid (NULL).";
      case AAC_DEC_UNSUPPORTED_AOT:
        return "The AOT found in the configuration is not supported.";
      case AAC_DEC_UNSUPPORTED_FORMAT:
        return "The bitstream format is not supported.";
      case AAC_DEC_UNSUPPORTED_ER_FORMAT:
        return "The error resilience tool format is not supported.";
      case AAC_DEC_UNSUPPORTED_EPCONFIG:
        return "The error protection format is not supported.";
      case AAC_DEC_UNSUPPORTED_MULTILAYER:
        return "More than one layer for AAC scalable is not supported.";
      case AAC_DEC_UNSUPPORTED_CHANNELCONFIG:
        return "The channel configuration (either number or arrangement) is "
               "not supported.";
      case AAC_DEC_UNSUPPORTED_SAMPLINGRATE:
        return "The sample rate specified in the configuration is not "
               "supported.";
      case AAC_DEC_INVALID_SBR_CONFIG:
        return "The SBR configuration is not supported.";
      case AAC_DEC_SET_PARAM_FAIL:
        return "The parameter could not be set. Either the value was out of "
               "range or the parameter does not exist.";
      case AAC_DEC_NEED_TO_RESTART:
        return "The decoder needs to be restarted, since the required "
               "configuration change cannot be performed.";
      case AAC_DEC_OUTPUT_BUFFER_TOO_SMALL:
        return "The provided output buffer is too small.";
      case AAC_DEC_TRANSPORT_ERROR:
        return "The transport decoder encountered an unexpected error.";
      case AAC_DEC_PARSE_ERROR:
        return "Error while parsing the bitstream. Most probably it is "
               "corrupted, or the system crashed.";
      case AAC_DEC_UNSUPPORTED_EXTENSION_PAYLOAD:
        return "Error while parsing the extension payload of the bitstream. "
               "The extension payload type found is not supported.";
      case AAC_DEC_DECODE_FRAME_ERROR:
        return "The parsed bitstream value is out of range. Most probably the "
               "bitstream is corrupt, or the system crashed.";
      case AAC_DEC_CRC_ERROR:
        return "The embedded CRC did not match.";
      case AAC_DEC_INVALID_CODE_BOOK:
        return "An invalid codebook was signaled. Most probably the bitstream "
               "is corrupt, or the system crashed.";
      case AAC_DEC_UNSUPPORTED_PREDICTION:
        return "Predictor found, but not supported in the AAC Low Complexity "
               "profile. Most probably the bitstream is corrupt, or has a "
               "wrong format.";
      case AAC_DEC_UNSUPPORTED_CCE:
        return "A CCE element was found which is not supported. Most probably "
               "the bitstream is corrupt, or has a wrong format.";
      case AAC_DEC_UNSUPPORTED_LFE:
        return "A LFE element was found which is not supported. Most probably "
               "the bitstream is corrupt, or has a wrong format.";
      case AAC_DEC_UNSUPPORTED_GAIN_CONTROL_DATA:
        return "Gain control data found but not supported. Most probably the "
               "bitstream is corrupt, or has a wrong format.";
      case AAC_DEC_UNSUPPORTED_SBA:
        return "SBA found, but currently not supported in the BSAC profile.";
      case AAC_DEC_TNS_READ_ERROR:
        return "Error while reading TNS data. Most probably the bitstream is "
               "corrupt or the system crashed.";
      case AAC_DEC_RVLC_ERROR:
        return "Error while decoding error resilient data.";
      case AAC_DEC_ANC_DATA_ERROR:
        return "Non severe error concerning the ancillary data handling.";
      case AAC_DEC_TOO_SMALL_ANC_BUFFER:
        return "The registered ancillary data buffer is too small to receive "
               "the parsed data.";
      case AAC_DEC_TOO_MANY_ANC_ELEMENTS:
        return "More than the allowed number of ancillary data elements should "
               "be written to buffer.";
      default:
        return "Unknown FDK AAC decoder error";
    }
  }
};

// Map AAC enc codes to std::error_code
struct fdk_aacenc_error_category : public std::error_category {
  const char* name() const noexcept override { return "FDKAACEncoder"; }
  std::string message(int ev) const noexcept override {
    switch (static_cast<AACENC_ERROR>(ev)) {
      case AACENC_OK:
        return "No error happened. All fine.";
      case AACENC_INVALID_HANDLE:
        return "Handle passed to function call was invalid.";
      case AACENC_MEMORY_ERROR:
        return "Memory allocation failed.";
      case AACENC_UNSUPPORTED_PARAMETER:
        return "Parameter not available.";
      case AACENC_INVALID_CONFIG:
        return "Configuration not provided.";
      case AACENC_INIT_ERROR:
        return "General initialization error.";
      case AACENC_INIT_AAC_ERROR:
        return "AAC library initialization error.";
      case AACENC_INIT_SBR_ERROR:
        return "SBR library initialization error.";
      case AACENC_INIT_TP_ERROR:
        return "Transport library initialization error.";
      case AACENC_INIT_META_ERROR:
        return "Meta data library initialization error.";
      case AACENC_INIT_MPS_ERROR:
        return "MPS library initialization error.";
      case AACENC_ENCODE_ERROR:
        return "The encoding process was interrupted by an unexpected error.";
      case AACENC_ENCODE_EOF:
        return "End of file reached.";
      default:
        return "Unknown FDK AAC encoder error";
    }
  }
};

const fdk_aacdec_error_category fdkAACDecErrorCategory{};
const fdk_aacenc_error_category fdkAACEncErrorCategory{};
}  // namespace internal

// std::error_code helper
inline std::error_code make_fdk_aacdec_error_code(int err) {
  return {static_cast<int>(err), internal::fdkAACDecErrorCategory};
};

// std::error_code helper
inline std::error_code make_fdk_aacenc_error_code(int err) {
  return {static_cast<int>(err), internal::fdkAACEncErrorCategory};
};

class AACCodec : public Codec {
 public:
  AACCodec() = default;
  ~AACCodec() override;

  // Delete copy constructor and copy assignment operator
  AACCodec(const AACCodec&) = delete;
  AACCodec& operator=(const AACCodec&) = delete;

  // Codec implementation
  bell::Result<> setupEncode(const AudioFormat& audioFormat,
                             const CodecConfig& codecSpecificConfig) override;
  bell::Result<> setupDecode(const AudioFormat& audioFormat,
                             const CodecConfig& codecSpecificConfig) override;
  bell::Result<SetupStatus> setupDecodeFromHeaders(
      tcb::span<const std::byte> encodedInput) override {
    (void)encodedInput;
    return tl::make_unexpected(Errc::OperationNotSupported);
  };
  bell::Result<EncodeResult> encode(
      tcb::span<const std::byte> pcmInput) override;
  bell::Result<DecodeResult> decode(
      tcb::span<const std::byte> encodedInput) override;
  audio::Format getAudioFormat() const override { return audioFormat; }

 private:
  const char* LOG_TAG = "audio::AACCodec";

  AudioFormat audioFormat;

  // Codec configuration
  AACConfig config{};
  uint32_t samplesPerFrame = 960;

  AACENCODER* encoder = nullptr;
  AAC_DECODER_INSTANCE* decoder = nullptr;
  CStreamInfo* streamInfo = nullptr;  // Stream info for decoded audio

  // tmp buffer for encode / decode calls
  std::vector<std::byte> tmpBuffer;
};
}  // namespace bell::audio

namespace bell {
using AACCodec = audio::AACCodec;
}  // namespace bell
