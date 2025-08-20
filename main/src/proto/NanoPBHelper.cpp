#include "proto/NanoPBHelper.h"

using namespace nanopb_helper;

bool nanopb_helper::pbDecodeString(pb_istream_t* stream,
                                   const pb_field_t* field, void** arg) {
  auto& str = *static_cast<std::string*>(*arg);
  str.resize(stream->bytes_left);
  return pb_read(stream, reinterpret_cast<uint8_t*>(str.data()),
                 stream->bytes_left);
}

bool nanopb_helper::pbEncodeFixed64(pb_ostream_t* stream,
                                    const pb_field_t* field, void* const* arg) {
  if (!pb_encode_tag_for_field(stream, field)) {
    return false;
  }
  return pb_encode_fixed64(stream, *arg);
}

bool nanopb_helper::pbEncodeFixed32(pb_ostream_t* stream,
                                    const pb_field_t* field, void* const* arg) {
  if (!pb_encode_tag_for_field(stream, field)) {
    return false;
  }
  return pb_encode_fixed32(stream, *arg);
}

bool nanopb_helper::pbDecodeFixed64(pb_istream_t* stream,
                                    const pb_field_t* field, void** arg) {
  return pb_decode_fixed64(stream, *arg);
}
bool nanopb_helper::pbDecodeFixed32(pb_istream_t* stream,
                                    const pb_field_t* field, void** arg) {

  return pb_decode_fixed32(stream, *arg);
}

bool nanopb_helper::pbDecodeStringList(pb_istream_t* stream,
                                       const pb_field_t* field, void** arg) {
  auto& vec = *static_cast<std::vector<std::string>*>(*arg);

  std::string str;
  bool result = pbDecodeString(stream, field, reinterpret_cast<void**>(&str));

  if (result) {
    vec.push_back(str);
  }

  return result;
}

bool nanopb_helper::pbDecodeUint8Vector(pb_istream_t* stream,
                                        const pb_field_t* field, void** arg) {
  auto& vec = *static_cast<std::vector<uint8_t>*>(*arg);
  vec.resize(stream->bytes_left);
  return pb_read(stream, vec.data(), stream->bytes_left);
}

bool nanopb_helper::pbDecodeByteVector(pb_istream_t* stream,
                                       const pb_field_t* field, void** arg) {
  auto& vec = *static_cast<std::vector<std::byte>*>(*arg);
  vec.resize(stream->bytes_left);
  return pb_read(stream, reinterpret_cast<uint8_t*>(vec.data()),
                 stream->bytes_left);
}

// Basic encoders
bool nanopb_helper::pbEncodeString(pb_ostream_t* stream,
                                   const pb_field_t* field, void* const* arg) {
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

bool nanopb_helper::pbEncodeStringList(pb_ostream_t* stream,
                                       const pb_field_t* field,
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

bool nanopb_helper::pbEncodeUint8Vector(pb_ostream_t* stream,
                                        const pb_field_t* field,
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

bool nanopb_helper::pbEncodeByteVector(pb_ostream_t* stream,
                                       const pb_field_t* field,
                                       void* const* arg) {
  auto& vector = *static_cast<std::vector<std::byte>*>(*arg);

  if (!vector.empty()) {
    if (!pb_encode_tag_for_field(stream, field)) {
      return false;
    }

    if (!pb_encode_string(stream,
                          reinterpret_cast<const uint8_t*>(vector.data()),
                          vector.size())) {
      return false;
    }
  }

  return true;
}
