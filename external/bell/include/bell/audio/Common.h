#pragma once

#include <cstdint>
#include <optional>
#include <system_error>

#include <tcb/span.hpp>

namespace bell::audio {

/**
 * @brief Error enumereation for various audio operations
 */
enum class Errc {
  Success = 0,
  NotEnoughBytes = 1,
  CodecError = 2,
  UnsupportedConfig = 3,
  InvalidFormat = 4,
  OperationNotSupported = 5,
  EndOfStream = 6,
  IoError = 7,
};

struct EncodedPacket {
  tcb::span<std::byte> data;
  std::optional<uint32_t> timestamp = std::nullopt;
  std::optional<uint32_t> duration = std::nullopt;
  uint32_t streamIdx = 0;
};

namespace internal {
struct audio_error_category : public std::error_category {
  const char* name() const noexcept override { return "BellHTTP"; }
  std::string message(int ev) const noexcept override {
    switch (static_cast<Errc>(ev)) {
      case Errc::Success:
        return "Success";
      case Errc::NotEnoughBytes:
        return "Not enough bytes for operation";
      case Errc::CodecError:
        return "Unknown error during codec operation";
      case Errc::InvalidFormat:
        return "Invalid audio format";
      case Errc::UnsupportedConfig:
        return "Unsupported config";
      default:
        return "Unknown error";
    }
  }
};

const audio_error_category audioErrorCategory{};
}  // namespace internal

// Plug in the error code category for std::error_code
inline std::error_code make_error_code(const bell::audio::Errc& e) {
  return {static_cast<int>(e), internal::audioErrorCategory};
};

// Enum class for the sample format
enum class SampleFormat : uint8_t {
  U8,   // Unsigned 8-bit PCM
  S16,  // Signed 16-bit PCM
  S24,  // Signed 24-bit PCM (usually stored in 32 bits)
  S32,  // Signed 32-bit PCM
  F32,  // 32-bit Floating-Point
  F64,  // 64-bit Floating-Point
};

// Helper to get the size of a sample in bytes
constexpr size_t getSampleSizeInBytes(SampleFormat format) {
  switch (format) {
    case SampleFormat::U8:
      return 1;
    case SampleFormat::S16:
      return 2;
    case SampleFormat::S24:
      return 3;  // Packed 24-bit
    case SampleFormat::S32:
    case SampleFormat::F32:
      return 4;
    case SampleFormat::F64:
      return 8;
  }
  return 0;  // Should not happen
}

constexpr SampleFormat bitwidthToFixedSampleFormat(int bw) {
  switch (bw) {
    case 8:
      return SampleFormat::U8;
    case 16:
      return SampleFormat::S16;
    case 24:
      return SampleFormat::S24;
    case 32:
      return SampleFormat::S32;
    default:
      return SampleFormat::S16;  // Default to S16 for unsupported bit widths
  }
}
// Enum class for the sample rate of audio samples.
enum class SampleRate : uint32_t {
  SR_8000HZ = 8000,
  SR_16000HZ = 16000,
  SR_22050HZ = 22050,
  SR_44100HZ = 44100,
  SR_48000HZ = 48000,
};

// Class for the audio format of audio samples.
class Format {
 public:
  // Default constructor
  Format() = default;

  Format(int numChannels, SampleFormat sampleFormat, SampleRate sampleRate)
      : ch(numChannels), sf(sampleFormat), sr(sampleRate) {}
  // Getters
  constexpr SampleFormat getSampleFormat() const { return sf; }
  constexpr SampleRate getSampleRate() const { return sr; }
  constexpr uint8_t getNumChannels() const { return ch; }
  constexpr uint32_t getSampleRateValue() const {
    return static_cast<uint32_t>(sr);
  }

  // Setters
  void setSampleFormat(SampleFormat sampleFormat) { sf = sampleFormat; }
  void setSampleRate(SampleRate sampleRate) { sr = sampleRate; }
  void setNumChannels(int numChannels) { ch = numChannels; }

  // Operators
  constexpr bool operator==(const Format& other) const noexcept {
    return sf == other.sf && sr == other.sr && ch == other.ch;
  }

  constexpr bool operator!=(const Format& other) const noexcept {
    return !(*this == other);
  }

  // Convert sample count to bytes
  constexpr uint32_t samplesToBytes(uint32_t samples) const noexcept {
    return getSampleSizeInBytes(sf) * samples * ch;
  }

  // Convert bytes to sample count
  constexpr uint32_t bytesToSamples(uint32_t bytes) const noexcept {
    // Avoid division by zero if format is invalid
    const uint32_t frameSize = getSampleSizeInBytes(sf) * ch;
    return frameSize > 0 ? bytes / frameSize : 0;
  }

  // Convert milliseconds to samples (FIXED)
  constexpr uint32_t msToSamples(uint32_t ms) const noexcept {
    // Multiply first to preserve precision, use 64-bit intermediate to prevent overflow
    return (static_cast<uint64_t>(getSampleRateValue()) * ms) / 1000;
  }

  // Convert samples to milliseconds (FIXED)
  constexpr uint32_t samplesToMs(uint32_t samples) const noexcept {
    return (static_cast<uint64_t>(samples) * 1000) / getSampleRateValue();
  }

  // Convert milliseconds to bytes
  constexpr uint32_t msToBytes(uint32_t ms) const noexcept {
    return samplesToBytes(msToSamples(ms));
  }

 private:
  int ch = 2;
  SampleFormat sf = SampleFormat::S16;
  SampleRate sr = SampleRate::SR_44100HZ;
};
}  // namespace bell::audio

namespace std {
template <>
struct is_error_code_enum<bell::audio::Errc> : true_type {};
}  // namespace std

namespace bell {
using AudioFormat = audio::Format;
using SampleRate = audio::SampleRate;
using SampleFormat = audio::SampleFormat;
}  // namespace bell
