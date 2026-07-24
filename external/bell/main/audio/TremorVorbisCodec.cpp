#include "bell/audio/TremorVorbisCodec.h"

// Standard includes
#include <cassert>
#include <cstddef>
#include <unordered_map>

// Library includes

// Own includes
#include "bell/Logger.h"
#include "bell/audio/Codec.h"
#include "bell/audio/Common.h"
#include "ivorbiscodec.h"

using namespace bell::audio;

TremorVorbisCodec::~TremorVorbisCodec() {
  if (infoInitialized) {
    vorbis_block_clear(&block);
    vorbis_dsp_clear(&dspState);
    vorbis_comment_clear(&comment);
    vorbis_info_clear(&info);
  }
}

bell::Result<> TremorVorbisCodec::setupEncode(
    const AudioFormat& audioFormat, const CodecConfig& codecSpecificConfig) {
  (void)audioFormat;
  (void)codecSpecificConfig;
  // Libtremor does not support encoding
  return tl::make_unexpected(audio::Errc::OperationNotSupported);
}

bell::Result<> TremorVorbisCodec::setupDecode(
    const AudioFormat& audioFormat, const CodecConfig& codecSpecificConfig) {

  // Check if the config is of the correct type
  if (!std::holds_alternative<TremorConfig>(codecSpecificConfig)) {
    return tl::make_unexpected(Errc::UnsupportedConfig);
  }
  config = std::get<TremorConfig>(codecSpecificConfig);

  if (infoInitialized) {
    vorbis_block_clear(&block);
    vorbis_dsp_clear(&dspState);
  }

  vorbis_info_init(&info);
  vorbis_comment_init(&comment);
  vorbis_synthesis_init(&dspState, &info);

  infoInitialized = true;
  this->audioFormat = audioFormat;

  return {};
}

bell::Result<Codec::SetupStatus> TremorVorbisCodec::setupDecodeFromHeaders(
    tcb::span<const std::byte> encodedInput) {
  if (headerPacketCount == 3)
    return SetupStatus::Ready;

  ogg_packet packet;
  packet.b_o_s = 0;
  packet.packet = const_cast<uint8_t*>(
      reinterpret_cast<const uint8_t*>(encodedInput.data()));
  packet.bytes = encodedInput.size();
  packet.e_o_s = 0;
  packet.granulepos = 0;
  packet.packetno = 0;

  if (!infoInitialized) {
    vorbis_info_init(&info);
    vorbis_comment_init(&comment);
    infoInitialized = true;
    headerPacketCount = 0;
    packet.b_o_s = 1;  // Beginning of stream
  }

  int res = vorbis_synthesis_headerin(&info, &comment, &packet);
  if (res < 0) {
    BELL_LOG(info, LOG_TAG, "Failed to initialize vorbis decoder {}", res);
    return tl::make_unexpected(make_tremor_error_code(res));
  }

  headerPacketCount++;

  if (headerPacketCount == 3) {
    // Initialize DSP state and block
    vorbis_synthesis_init(&dspState, &info);
    vorbis_block_init(&dspState, &block);

    audioFormat = audio::Format{info.channels, SampleFormat::S16,
                                static_cast<SampleRate>(info.rate)};
  }
  return headerPacketCount == 3 ? SetupStatus::Ready : SetupStatus::Incomplete;
}

bell::Result<Codec::EncodeResult> TremorVorbisCodec::encode(
    tcb::span<const std::byte> pcmInput) {
  (void)pcmInput;
  // Libtremor does not support encoding
  return tl::make_unexpected(audio::Errc::OperationNotSupported);
}

bell::Result<Codec::DecodeResult> TremorVorbisCodec::decode(
    tcb::span<const std::byte> encodedInput) {
  assert(infoInitialized);

  ogg_packet packet;
  packet.packet = const_cast<uint8_t*>(
      reinterpret_cast<const uint8_t*>(encodedInput.data()));
  packet.bytes = encodedInput.size();
  packet.b_o_s = 0;
  packet.e_o_s = 0;
  packet.granulepos = 0;
  packet.packetno = 0;

  int res = vorbis_synthesis(&block, &packet);
  if (res < 0) {
    return tl::make_unexpected(make_tremor_error_code(res));
  }

  res = vorbis_synthesis_blockin(&dspState, &block);
  if (res < 0) {
    return tl::make_unexpected(make_tremor_error_code(res));
  }

  int32_t** pcm;
  int samples = vorbis_synthesis_pcmout(&dspState, &pcm);
  if (samples > 0) {
    vorbis_synthesis_read(&dspState, samples);

    return DecodeResult{
        .pcm = {reinterpret_cast<std::byte*>(pcm[0]),
                audioFormat.samplesToBytes(samples)},
        .consumedInputBytes = encodedInput.size(),
    };
  }

  return tl::make_unexpected(audio::Errc::NotEnoughBytes);
}
