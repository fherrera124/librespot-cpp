#include "bell/audio/OpusCodec.h"

// Standar includes
#include <any>
#include <cassert>
#include <unordered_map>

// Library includes
#include "bell/audio/Common.h"
#include "opus.h"
#include "opus_defines.h"

// Own includes
#include "bell/Logger.h"
#include "tl/expected.hpp"

using namespace bell::audio;

OpusCodec::~OpusCodec() {
  if (encoder) {
    opus_encoder_destroy(encoder);
  }

  if (decoder) {
    opus_decoder_destroy(decoder);
  }
}

int OpusCodec::getOpusFrameSize(int frameDuration) {
  const std::unordered_map<int, int> durationMapping = {
      {5, OPUS_FRAMESIZE_5_MS},     {10, OPUS_FRAMESIZE_10_MS},
      {20, OPUS_FRAMESIZE_20_MS},   {40, OPUS_FRAMESIZE_40_MS},
      {60, OPUS_FRAMESIZE_60_MS},   {80, OPUS_FRAMESIZE_80_MS},
      {100, OPUS_FRAMESIZE_100_MS}, {120, OPUS_FRAMESIZE_120_MS}};

  if (durationMapping.contains(frameDuration)) {
    return durationMapping.at(frameDuration);
  }

  return OPUS_FRAMESIZE_20_MS;
}

bell::Result<> OpusCodec::setupEncode(const AudioFormat& audioFormat,
                                      const CodecConfig& codecSpecificConfig) {
  // Check if the config is of the correct type
  if (!std::holds_alternative<OpusConfig>(codecSpecificConfig)) {
    return tl::make_unexpected(Errc::UnsupportedConfig);
  }
  config = std::get<OpusConfig>(codecSpecificConfig);

  tmpBuffer.resize(tmpBufferSize);

  // Destroy the encoder if it exists
  if (encoder) {
    opus_encoder_destroy(encoder);
  }

  if (audioFormat.getSampleRate() != SampleRate::SR_48000HZ) {
    BELL_LOG(warn, LOG_TAG, "Opus only supports 48kHz sample rate");
    return tl::make_unexpected(audio::Errc::UnsupportedConfig);
  }
  this->audioFormat = audioFormat;

  int opusError = 0;

  // Allocate opus enc memory and initialize it
  encoder = opus_encoder_create(
      static_cast<int32_t>(audioFormat.getSampleRateValue()),
      audioFormat.getNumChannels(),
      config.application.value_or(OPUS_APPLICATION_AUDIO), &opusError);
  if (opusError != OPUS_OK) {
    BELL_LOG(error, LOG_TAG, "Failed to create opus encoder: {}",
             opus_strerror(opusError));
    return tl::make_unexpected(make_opus_error_code(opusError));
  }

  if (config.samplesPerPacket.has_value()) {
    int frameLength =
        static_cast<int>(audioFormat.samplesToMs(*config.samplesPerPacket));
    auto opusDuration = getOpusFrameSize(frameLength);

    // Fallback on 20ms if unsupported
    if (opusDuration == OPUS_FRAMESIZE_20_MS) {
      config.samplesPerPacket = 960;
    }

    // Encoder settings
    opusError =
        opus_encoder_ctl(encoder, OPUS_SET_EXPERT_FRAME_DURATION(opusDuration));
    if (opusError != OPUS_OK) {
      BELL_LOG(error, LOG_TAG, "Failed to set opus frame duration: {}",
               opus_strerror(opusError));
      return tl::make_unexpected(make_opus_error_code(opusError));
    }
  }

  return {};
}

bell::Result<> OpusCodec::setupDecode(const AudioFormat& audioFormat,
                                      const CodecConfig& codecSpecificConfig) {
  // Check if the config is of the correct type
  if (!std::holds_alternative<OpusConfig>(codecSpecificConfig)) {
    return tl::make_unexpected(Errc::UnsupportedConfig);
  }
  config = std::get<OpusConfig>(codecSpecificConfig);

  // Resize the tmpbuffer
  tmpBuffer.resize(config.bufferSize);

  if (decoder) {
    opus_decoder_destroy(decoder);
  }

  if (audioFormat.getSampleRate() != SampleRate::SR_48000HZ) {
    BELL_LOG(warn, LOG_TAG, "Opus only supports 48kHz sample rate");
    return tl::make_unexpected(audio::Errc::UnsupportedConfig);
  }
  this->audioFormat = audioFormat;

  int opusError = 0;

  // Allocate opus enc memory and initialize it
  decoder = opus_decoder_create(
      static_cast<int32_t>(audioFormat.getSampleRateValue()),
      audioFormat.getNumChannels(), &opusError);

  if (opusError != OPUS_OK) {
    BELL_LOG(error, LOG_TAG, "Failed to create opus decoder: {}",
             opus_strerror(opusError));
    // Opus errors most likely come from unsupported config
    return tl::make_unexpected(make_opus_error_code(opusError));
  }

  return {};
}

bell::Result<Codec::EncodeResult> OpusCodec::encode(
    tcb::span<const std::byte> pcmInput) {
  assert(encoder != nullptr);
  int32_t packetSize = opus_encode(
      encoder, reinterpret_cast<const opus_int16*>(pcmInput.data()),
      static_cast<int>(pcmInput.size()) / getAudioFormat().getNumChannels(),
      reinterpret_cast<uint8_t*>(tmpBuffer.data()),
      static_cast<int>(tmpBuffer.size()));

  // Handle encoded result
  if (packetSize < 0) {
    BELL_LOG(info, LOG_TAG, "Could not encode opus packet, err = {}",
             opus_strerror(packetSize));
    return tl::make_unexpected(make_opus_error_code(packetSize));
  }

  if (packetSize == 0) {
    return tl::make_unexpected(audio::Errc::NotEnoughBytes);
  }

  return EncodeResult{
      .packets =
          {
              {.data = {tmpBuffer.data(), static_cast<uint32_t>(packetSize)}},
          },
      .consumedInputBytes = pcmInput.size()};
}

bell::Result<Codec::DecodeResult> OpusCodec::decode(
    tcb::span<const std::byte> encodedInput) {
  assert(decoder != nullptr);

  auto pcmLen = opus_decode(
      decoder, reinterpret_cast<const unsigned char*>(encodedInput.data()),
      static_cast<int32_t>(encodedInput.size()),
      reinterpret_cast<opus_int16*>(tmpBuffer.data()),
      config.samplesPerPacket.value_or(960), false);

  // Handle encoded result
  if (pcmLen < 0) {
    BELL_LOG(info, LOG_TAG, "Could not decode opus packet, err = {}",
             opus_strerror(pcmLen));
    return tl::make_unexpected(make_opus_error_code(pcmLen));
  }
  if (pcmLen == 0) {
    return tl::make_unexpected(audio::Errc::NotEnoughBytes);
  }

  return DecodeResult{
      .pcm = {tmpBuffer.data(), getAudioFormat().samplesToBytes(pcmLen)},
      .consumedInputBytes = encodedInput.size(),
  };
}
