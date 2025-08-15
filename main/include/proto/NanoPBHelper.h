#pragma once

// System includes
#include <array>
#include <cassert>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

// NanoPB includes
#include "pb.h"
#include "pb_decode.h"
#include "pb_encode.h"

namespace nanopb_helper {

// Basic decoders
bool pbDecodeString(pb_istream_t* stream, const pb_field_t* field, void** arg);
bool pbDecodeFixed64(pb_istream_t* stream, const pb_field_t* field, void** arg);
bool pbDecodeFixed32(pb_istream_t* stream, const pb_field_t* field, void** arg);
bool pbDecodeStringList(pb_istream_t* stream, const pb_field_t* field,
                        void** arg);
bool pbDecodeUint8Vector(pb_istream_t* stream, const pb_field_t* field,
                         void** arg);

// Basic encoders
bool pbEncodeString(pb_ostream_t* stream, const pb_field_t* field,
                    void* const* arg);
bool pbEncodeFixed64(pb_ostream_t* stream, const pb_field_t* field,
                     void* const* arg);
bool pbEncodeFixed32(pb_ostream_t* stream, const pb_field_t* field,
                     void* const* arg);
bool pbEncodeStringList(pb_ostream_t* stream, const pb_field_t* field,
                        void* const* arg);
bool pbEncodeUint8Vector(pb_ostream_t* stream, const pb_field_t* field,
                         void* const* arg);

// Integer handling
template <typename IntegerT>
bool pbDecodeVarint(pb_istream_t* stream, const pb_field_t* field, void** arg) {
  auto* target = static_cast<IntegerT*>(*arg);
  uint64_t value;
  if (!pb_decode_varint(stream, &value)) {
    return false;
  }
  *target = static_cast<IntegerT>(value);
  return true;
}

// Integer handling
template <typename IntegerT>
bool pbDecodeVarintList(pb_istream_t* stream, const pb_field_t* field,
                        void** arg) {
  auto& vec = *static_cast<std::vector<IntegerT>*>(*arg);
  IntegerT integer;
  void* ptrToValue = &integer;
  bool result = pbDecodeVarint<IntegerT>(stream, field, &ptrToValue);
  if (result) {
    vec.push_back(result);
  }
  return result;
}

// Integer handling
template <typename IntegerT>
bool pbDecodeSvarint(pb_istream_t* stream, const pb_field_t* field,
                     void** arg) {
  auto* target = static_cast<IntegerT*>(*arg);
  int64_t value;
  if (!pb_decode_svarint(stream, &value)) {
    return false;
  }
  *target = static_cast<IntegerT>(value);
  return true;
}

template <std::size_t N>
bool pbDecodeUint8Array(pb_istream_t* stream, const pb_field_t* field,
                        void** arg) {
  auto& arr = *static_cast<std::array<uint8_t, N>*>(*arg);
  assert(N == stream->bytes_left);
  return pb_read(stream, arr.data(), std::min(stream->bytes_left, N));
}

template <typename IntegerT>
bool pbEncodeVarint(pb_ostream_t* stream, const pb_field_t* field,
                    void* const* arg) {
  const auto value = static_cast<uint64_t>(*static_cast<const IntegerT*>(*arg));
  if (!pb_encode_tag_for_field(stream, field)) {
    return false;
  }
  return pb_encode_varint(stream, value);
}

template <typename IntegerT>
bool pbEncodeVarintList(pb_ostream_t* stream, const pb_field_t* field,
                        void* const* arg) {
  auto& vec = *static_cast<std::vector<IntegerT>*>(*arg);

  if (vec.empty()) {
    return true;  // Nothing to encode
  }

  pb_ostream_t sizingStream = PB_OSTREAM_SIZING;
  for (const auto& value : vec) {
    if (!pb_encode_varint(&sizingStream, static_cast<uint64_t>(value))) {
      return false;
    }
  }

  if (!pb_encode_tag(stream, PB_WT_STRING, field->tag)) {
    return false;
  }

  if (!pb_encode_varint(stream, sizingStream.bytes_written)) {
    return false;
  }

  for (const auto& value : vec) {
    if (!pb_encode_varint(stream, static_cast<uint64_t>(value))) {
      return false;
    }
  }

  return true;
}

template <typename IntegerT>
bool pbEncodeSvarint(pb_ostream_t* stream, const pb_field_t* field,
                     void* const* arg) {
  const auto value = static_cast<int64_t>(*static_cast<const IntegerT*>(*arg));
  if (!pb_encode_tag_for_field(stream, field)) {
    return false;
  }
  return pb_encode_svarint(stream, value);
}

template <std::size_t N>
bool pbEncodeUint8Array(pb_ostream_t* stream, const pb_field_t* field,
                        void* const* arg) {
  auto& arr = *static_cast<std::array<uint8_t, N>*>(*arg);
  if (!arr.empty()) {
    if (!pb_encode_tag_for_field(stream, field)) {
      return false;
    }

    if (!pb_encode_string(stream, arr.data(), arr.size())) {
      return false;
    }
  }

  return true;
}

// Binding helpers
template <typename FieldT>
void bindField(pb_callback_t& pbField, FieldT& field, bool isDecode) {
  static_assert(sizeof(FieldT) == 0, "No specialization for this field type");
}

// String types
inline void bindField(pb_callback_t& pbField, std::string& field,
                      bool isDecode) {
  if (isDecode) {
    pbField.funcs.decode = &pbDecodeString;
  } else {
    pbField.funcs.encode = &pbEncodeString;
  }
  pbField.arg = &field;
}

// Integer types
template <typename IntegerT>
void bindVarintField(pb_callback_t& pbField, IntegerT& field, bool isDecode) {
  if (isDecode) {
    pbField.funcs.decode = &pbDecodeVarint<IntegerT>;
  } else {
    pbField.funcs.encode = &pbEncodeVarint<IntegerT>;
  }
  pbField.arg = &field;
}

// String vector types
inline void bindField(pb_callback_t& pbField, std::vector<std::string>& field,
                      bool isDecode) {
  if (isDecode) {
    pbField.funcs.decode = &pbDecodeStringList;
  } else {
    pbField.funcs.encode = &pbEncodeStringList;
  }

  pbField.arg = &field;
}

// Varint vector types
template <typename IntegerT>
inline void bindVarintListField(pb_callback_t& pbField,
                                std::vector<IntegerT>& field, bool isDecode) {
  if (isDecode) {
    pbField.funcs.decode = &pbDecodeVarintList<IntegerT>;
  } else {
    pbField.funcs.encode = &pbEncodeVarintList<IntegerT>;
  }

  pbField.arg = &field;
}

// Bytes vector types
inline void bindField(pb_callback_t& pbField, std::vector<uint8_t>& field,
                      bool isDecode) {
  if (isDecode) {
    pbField.funcs.decode = &pbDecodeUint8Vector;
  } else {
    pbField.funcs.encode = &pbEncodeUint8Vector;
  }

  pbField.arg = &field;
}

// Bytes array type
template <std::size_t N>
inline void bindField(pb_callback_t& pbField, std::array<uint8_t, N>& field,
                      bool isDecode) {
  if (isDecode) {
    pbField.funcs.decode = &pbDecodeUint8Array<N>;
  } else {
    pbField.funcs.encode = &pbEncodeUint8Array<N>;
  }

  pbField.arg = &field;
}

// Bytes boolean type
inline void bindField(pb_callback_t& pbField, bool& field, bool isDecode) {
  if (isDecode) {
    pbField.funcs.decode = &pbDecodeVarint<bool>;
  } else {
    pbField.funcs.encode = &pbEncodeVarint<bool>;
  }

  pbField.arg = &field;
}

// Double type
inline void bindField(pb_callback_t& pbField, double& field, bool isDecode) {
  if (isDecode) {
    pbField.funcs.decode = &pbDecodeFixed64;
  } else {
    pbField.funcs.encode = &pbEncodeFixed64;
  }

  pbField.arg = &field;
}

// Float type
inline void bindField(pb_callback_t& pbField, float& field, bool isDecode) {
  if (isDecode) {
    pbField.funcs.decode = &pbDecodeFixed32;
  } else {
    pbField.funcs.encode = &pbEncodeFixed32;
  }

  pbField.arg = &field;
}

// int32_t type
inline void bindField(pb_callback_t& pbField, int32_t& field, bool isDecode) {
  if (isDecode) {
    pbField.funcs.decode = &pbDecodeSvarint<int32_t>;
  } else {
    pbField.funcs.encode = &pbEncodeSvarint<int32_t>;
  }

  pbField.arg = &field;
}

// int64_t type
inline void bindField(pb_callback_t& pbField, int64_t& field, bool isDecode) {
  if (isDecode) {
    pbField.funcs.decode = &pbDecodeSvarint<int64_t>;
  } else {
    pbField.funcs.encode = &pbEncodeSvarint<int64_t>;
  }

  pbField.arg = &field;
}

template <typename T>
struct Optional {
  T value{};
  bool hasValue = false;
  pb_callback_t wrappedCallback;

  static bool decode(pb_istream_t* stream, const pb_field_t* field,
                     void** arg) {
    auto* self = static_cast<Optional<T>*>(*arg);
    self->hasValue = true;
    void* valuePtr = &self->value;
    return self->wrappedCallback.funcs.decode(stream, field, &valuePtr);
  }

  static bool encode(pb_ostream_t* stream, const pb_field_t* field,
                     void* const* arg) {
    auto* self = static_cast<Optional<T>*>(*arg);
    if (!self->hasValue) {
      return true;  // No value to encode, just return true
    }

    void* valuePtr = &self->value;
    return self->wrappedCallback.funcs.encode(stream, field, &valuePtr);
  }
};

// Specialization for Optional<T>
template <typename T>
inline void bindField(pb_callback_t& pbField, Optional<T>& field,
                      bool isDecode) {
  bindField(field.wrappedCallback, field.value, isDecode);

  if (isDecode) {
    pbField.funcs.decode = &Optional<T>::decode;
  } else {
    pbField.funcs.encode = &Optional<T>::encode;
  }

  pbField.arg = &field;
}

// Struct handlers
template <typename StructT>
struct StructCodec {
  static bool decode(pb_istream_t* stream, const pb_field_t* field,
                     void** arg) {
    static_assert(sizeof(StructT) == 0, "No decoder specialization found");
    return false;
  }

  static bool encode(pb_ostream_t* stream, const pb_field_t* field,
                     void* const* arg) {
    static_assert(sizeof(StructT) == 0, "No encoder specialization found");
    return false;
  }
};

#define NANOPB_STRUCT(StructName, NanopbFields)                                \
  namespace nanopb_helper {                                                    \
  template <>                                                                  \
  struct StructCodec<StructName> {                                             \
    static bool decode(pb_istream_t* s, const pb_field_t* /*f*/, void** a) {   \
      auto proto = StructName::bindFields(static_cast<StructName*>(*a), true); \
      return pb_decode(s, NanopbFields, &proto);                               \
    }                                                                          \
    static bool encodeSubmessage(pb_ostream_t* s, const pb_field_t* f,         \
                                 void* const* a) {                             \
                                                                               \
      if (!pb_encode_tag_for_field(s, f))                                      \
        return false;                                                          \
      auto proto =                                                             \
          StructName::bindFields(static_cast<StructName*>(*a), false);         \
      return pb_encode_submessage(s, NanopbFields, &proto);                    \
    }                                                                          \
    static bool encode(pb_ostream_t* s, const pb_field_t* /*f*/,               \
                       void* const* a) {                                       \
      auto proto =                                                             \
          StructName::bindFields(static_cast<StructName*>(*a), false);         \
      return pb_encode(s, NanopbFields, &proto);                               \
    }                                                                          \
    static bool decodeVector(pb_istream_t* s, const pb_field_t* f, void** a) { \
      StructName item{};                                                       \
      auto* vec = static_cast<std::vector<StructName>*>(*a);                   \
      void* itemPtr = &item;                                                   \
      bool result = decode(s, f, &itemPtr);                                    \
      if (result)                                                              \
        vec->push_back(item);                                                  \
      return result;                                                           \
    }                                                                          \
    static bool encodeVector(pb_ostream_t* s, const pb_field_t* f,             \
                             void* const* a) {                                 \
      auto* vec = static_cast<std::vector<StructName>*>(*a);                   \
      for (auto& item : *vec) {                                                \
        void* itemPtr = &item;                                                 \
        if (!encodeSubmessage(s, f, &itemPtr))                                 \
          return false;                                                        \
      }                                                                        \
      return true;                                                             \
    }                                                                          \
  };                                                                           \
  inline void bindField(pb_callback_t& pbField, StructName& field,             \
                        bool isDecode) {                                       \
    if (isDecode) {                                                            \
      pbField.funcs.decode = &nanopb_helper::StructCodec<StructName>::decode;  \
    } else {                                                                   \
      pbField.funcs.encode =                                                   \
          &nanopb_helper::StructCodec<StructName>::encodeSubmessage;           \
    }                                                                          \
    pbField.arg = &field;                                                      \
  }                                                                            \
  inline void bindField(pb_callback_t& pbField,                                \
                        nanopb_helper::Optional<StructName>& field,            \
                        bool isDecode) {                                       \
    bindField(field.wrappedCallback, field.value, isDecode);                   \
    if (isDecode) {                                                            \
      pbField.funcs.decode = &Optional<StructName>::decode;                    \
    } else {                                                                   \
      pbField.funcs.encode = &Optional<StructName>::encode;                    \
    }                                                                          \
                                                                               \
    pbField.arg = &field;                                                      \
  }                                                                            \
  inline void bindField(pb_callback_t& pbField,                                \
                        std::vector<StructName>& field, bool isDecode) {       \
    if (isDecode) {                                                            \
      pbField.funcs.decode =                                                   \
          &nanopb_helper::StructCodec<StructName>::decodeVector;               \
    } else {                                                                   \
      pbField.funcs.encode =                                                   \
          &nanopb_helper::StructCodec<StructName>::encodeVector;               \
    }                                                                          \
    pbField.arg = &field;                                                      \
  }                                                                            \
  }

template <typename MessageT>
bool encodeToVector(MessageT& message, std::vector<uint8_t>& output) {
  // Actual encoding
  pb_ostream_t stream;
  stream.callback = [](pb_ostream_t* stream, const pb_byte_t* buf,
                       size_t count) -> bool {
    auto* vec = static_cast<std::vector<uint8_t>*>(stream->state);
    vec->insert(vec->end(), buf, buf + count);
    return true;
  };
  stream.state = &output;
  stream.max_size = SIZE_MAX;
  stream.bytes_written = 0;
  output.clear();
  void* messagePtr = &message;
  return nanopb_helper::StructCodec<MessageT>::encode(&stream, nullptr,
                                                      &messagePtr);
}

template <typename MessageT>
bool decodeFromBuffer(MessageT& message, const uint8_t* buffer,
                      size_t bufferLen) {
  message = MessageT();  // Reset the message to its default state

  pb_istream_t stream = pb_istream_from_buffer(buffer, bufferLen);
  void* messagePtr = &message;
  return nanopb_helper::StructCodec<MessageT>::decode(&stream, nullptr,
                                                      &messagePtr);
}

template <typename MessageT>
bool decodeFromVector(MessageT& message, const std::vector<uint8_t>& input) {
  return decodeFromBuffer(message, input.data(), input.size());
}

}  // namespace nanopb_helper
