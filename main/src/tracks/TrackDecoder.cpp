// #include "tracks/TrackDecoder.h"
// #include "bell/Logger.h"
// #include "bell/audio/Codec.h"
// #include "bell/audio/Common.h"
// #include "bell/http/Client.h"
// #include "tl/expected.hpp"

// using namespace cspot;

// TrackDecoder::TrackDecoder() = default;

// bell::Result<> TrackDecoder::open(const std::string& cdnUrl,
//                                   const std::vector<std::byte>& decryptKey) {
//   cdnDataStream =
//       std::make_unique<CDNDataStream>(std::make_shared<bell::HTTPClient>());
//   auto res = cdnDataStream->open(cdnUrl, decryptKey);

//   if (!res) {
//     // Could not open CDN data stream
//     return res;
//   }

//   oggContainer = std::make_unique<bell::audio::OggContainer>();
//   auto oggRes = oggContainer->openForRead(cdnDataStream);
//   if (!oggRes) {
//     BELL_LOG(error, LOG_TAG, "Failed to open Ogg container: {}",
//              oggRes.error());
//     return oggRes;
//   }

//   // Skip first packet (spotify metadata)
//   (void)oggContainer->readNextPacket();

//   vorbisCodec = std::make_unique<bell::audio::TremorVorbisCodec>();

//   bool setupReady = false;
//   while (!setupReady) {
//     auto packet = oggContainer->readNextPacket();
//     if (!packet) {
//       BELL_LOG(error, LOG_TAG, "Failed to read Ogg packet: {}", packet.error());
//       continue;
//     }

//     auto setupRes = vorbisCodec->setupDecodeFromHeaders(packet->data);
//     if (!setupRes) {
//       BELL_LOG(error, LOG_TAG, "Failed to setup Vorbis decoder: {}",
//                setupRes.error());
//       return tl::make_unexpected(setupRes.error());
//     }

//     setupReady = (setupRes == bell::AudioCodec::SetupStatus::Ready);
//   }

//   BELL_LOG(info, LOG_TAG, "TrackDecoder successfully opened");
//   isActiveStream = true;
//   isEOF = false;

//   return {};
// }

// bell::Result<bell::AudioCodec::DecodeResult> TrackDecoder::decodePacket() {
//   if (!isActiveStream) {
//     return bell::make_unexpected_errc<bell::AudioCodec::DecodeResult>(
//         std::errc::not_connected);
//   }

//   auto packet = oggContainer->readNextPacket();
//   if (!packet) {
//     if (packet.error() == bell::audio::Errc::EndOfStream) {
//       isActiveStream = false;
//       isEOF = true;
//     }
//     return tl::make_unexpected(packet.error());
//   }

//   auto decodeRes = vorbisCodec->decode(packet->data);
//   if (!decodeRes) {
//     return tl::make_unexpected(decodeRes.error());
//   }

//   return *decodeRes;
// }

// bool TrackDecoder::isOpen() const {
//   return isActiveStream;
// }

// bool TrackDecoder::isEndOfStream() const {
//   return isEOF;
// }

// void TrackDecoder::resetStream() {
//   isActiveStream = false;
//   isEOF = false;

//   if (cdnDataStream) {
//     cdnDataStream.reset();
//   }
//   if (oggContainer) {
//     oggContainer.reset();
//   }
//   if (vorbisCodec) {
//     vorbisCodec.reset();
//   }
// }
