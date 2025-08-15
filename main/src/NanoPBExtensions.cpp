#include "NanoPBExtensions.h"

// System includes
#include <iostream>
#include <optional>
#include <ostream>
#include <string>
#include <vector>
#include "bell/Result.h"
#include "pb.h"
#include "pb_decode.h"

#include <connect.pb.h>

// NanoPB includes
#include <pb_encode.h>

using namespace cspot;

// Decode callback for std::vector<ContextTrack>
bool cspot::pbDecodeContextTrackList(pb_istream_t* stream,
                                     const pb_field_t* /*field*/, void** arg) {
  auto& trackQueue = *static_cast<std::vector<cspot::ContextTrack>*>(*arg);

  std::cout << "Decoding context track list" << std::endl;

  _ContextTrack contextTrackProto = ContextTrack_init_zero;
  cspot::ContextTrack decodedTrack;

  contextTrackProto.uid.funcs.decode = &cspot::pbDecodeString;
  contextTrackProto.uid.arg = &decodedTrack.uid;

  contextTrackProto.uri.funcs.decode = &cspot::pbDecodeString;
  contextTrackProto.uri.arg = &decodedTrack.uri;

  contextTrackProto.gid.funcs.decode = &cspot::pbDecodeUint8Vector;
  contextTrackProto.gid.arg = &decodedTrack.gid;

  if (!pb_decode(stream, ContextTrack_fields, &contextTrackProto)) {
    std::cout << "Failed to decode context track list" << std::endl;
    return false;
  }

  trackQueue.push_back(std::move(decodedTrack));
  return true;
}

// Decode callback for std::vector<ContextPage>
bool cspot::pbDecodeContextPageList(pb_istream_t* stream,
                                    const pb_field_t* /*field*/, void** arg) {
  auto& pages = *static_cast<std::vector<cspot::ContextPage>*>(*arg);

  _ContextPage contextPageProto = ContextPage_init_zero;
  cspot::ContextPage decodedPage;

  contextPageProto.page_url.funcs.decode = &cspot::pbDecodeString;
  contextPageProto.page_url.arg = &decodedPage.page_url;

  contextPageProto.next_page_url.funcs.decode = &cspot::pbDecodeString;
  contextPageProto.next_page_url.arg = &decodedPage.next_page_url;

  contextPageProto.tracks.funcs.decode = &pbDecodeContextTrackList;
  contextPageProto.tracks.arg = &decodedPage.tracks;

  if (!pb_decode(stream, ContextPage_fields, &contextPageProto)) {
    return false;
  }

  pages.push_back(std::move(decodedPage));
  return true;
}

bool cspot::pbEncodeString(pb_ostream_t* stream, const pb_field_t* field,
                           void* const* arg) {
  auto& str = *static_cast<std::string*>(*arg);

  if (!str.empty()) {
    if (!pb_encode_tag_for_field(stream, field)) {
      return false;
    }

    if (!pb_encode_string(stream, reinterpret_cast<pb_byte_t*>(str.data()),
                          str.size())) {
      return false;
    }
  }

  return true;
}

bool cspot::pbEncodeStringVector(pb_ostream_t* stream, const pb_field_t* field,
                                 void* const* arg) {
  auto& vector = *static_cast<std::vector<std::string>*>(*arg);

  for (const auto& str : vector) {
    if (!pb_encode_tag_for_field(stream, field)) {
      return false;
    }

    if (!pb_encode_string(stream,
                          reinterpret_cast<const pb_byte_t*>(str.data()),
                          str.size())) {
      return false;
    }
  }

  return true;
}

bool cspot::pbEncodeProvidedTrackList(pb_ostream_t* stream,
                                      const pb_field_t* field,
                                      void* const* arg) {
  auto& vector = *static_cast<std::vector<ProvidedTrack>*>(*arg);
  _ProvidedTrack msg = ProvidedTrack_init_zero;

  // Prepare nanopb callbacks
  msg.provider.funcs.encode = &pbEncodeString;
  msg.uid.funcs.encode = &pbEncodeString;
  msg.uri.funcs.encode = &pbEncodeString;

  for (auto& providedTrack : vector) {
    if (!pb_encode_tag_for_field(stream, field)) {
      return false;
    }

    msg.uri.arg = &providedTrack.uri;
    msg.uid.arg = &providedTrack.uid;
    msg.provider.arg = &providedTrack.provider;

    if (!pb_encode_submessage(stream, ProvidedTrack_fields, &msg)) {
      return false;
    }
  }

  return true;
}

bool cspot::pbDecodeString(pb_istream_t* stream, const pb_field_t* field,
                           void** arg) {
  auto& str = *static_cast<std::string*>(*arg);

  str.resize(stream->bytes_left);

  return pb_read(stream, reinterpret_cast<uint8_t*>(str.data()),
                 stream->bytes_left);
}

bool cspot::pbEncodeOptionalBool(pb_ostream_t* stream, const pb_field_t* field,
                                 void* const* arg) {
  auto& boolean = *static_cast<std::optional<bool>*>(*arg);

  if (boolean.has_value()) {
    if (!pb_encode_tag_for_field(stream, field)) {
      return false;
    }

    if (!pb_encode_varint(stream, boolean.value())) {
      return false;
    }
  }

  return true;
}

bool cspot::pbEncodeUint8Vector(pb_ostream_t* stream, const pb_field_t* field,
                                void* const* arg) {
  auto& vector = *static_cast<std::vector<uint8_t>*>(*arg);

  if (!vector.empty()) {
    if (!pb_encode_tag_for_field(stream, field)) {
      return false;
    }

    if (!pb_encode_string(stream, vector.data(), vector.size())) {
      return false;
    }
  }

  return true;
}

bool cspot::pbDecodeUint8Vector(pb_istream_t* stream, const pb_field_t* field,
                                void** arg) {
  auto& vec = *static_cast<std::vector<uint8_t>*>(*arg);

  vec.resize(stream->bytes_left);

  return pb_read(stream, vec.data(), stream->bytes_left);
}

bell::Result<size_t> cspot::pbEncodeMessage(uint8_t* buffer, size_t size,
                                            const pb_msgdesc_t* messageType,
                                            const void* src) {
  pb_ostream_t stream = pb_ostream_from_buffer(buffer, size);
  if (!pb_encode(&stream, messageType, src)) {
    return bell::make_unexpected_errc<size_t>(std::errc::bad_message);
  }

  return stream.bytes_written;
}

bell::Result<size_t> cspot::pbCalculateEncodedSize(
    const pb_msgdesc_t* messageType, const void* src) {
  size_t size = 0;
  if (!pb_get_encoded_size(&size, messageType, src)) {
    return bell::make_unexpected_errc<size_t>(std::errc::bad_message);
  }
  return size;
}

bell::Result<> cspot::pbDecodeMessage(const uint8_t* buffer, size_t size,
                                      const pb_msgdesc_t* messageType,
                                      void* dest) {
  pb_istream_t stream = pb_istream_from_buffer(buffer, size);
  bool res = pb_decode(&stream, messageType, dest);

  if (!res) {
    return bell::make_unexpected_errc(std::errc::bad_message);
  }

  return {};
}
