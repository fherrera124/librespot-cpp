#pragma once

// System includes
#include <array>
#include <cassert>
#include <cstdio>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

// NanoPB includes
#include "bell/Logger.h"
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
bool pbDecodeByteVector(pb_istream_t* stream, const pb_field_t* field,
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
bool pbEncodeByteVector(pb_ostream_t* stream, const pb_field_t* field,
                        void* const* arg);

// Integer handling
template <typename IntegerT>
bool pbDecodeVarint(pb_istream_t* stream, const pb_field_t* field, void** arg) {
  auto* target = static_cast<IntegerT*>(*arg);
  uint64_t value;
  if (!pb_decode_varint(stream, &value)) {
    printf("Could not decode varint\n");
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

template <std::size_t N>
bool pbDecodeByteArray(pb_istream_t* stream, const pb_field_t* field,
                       void** arg) {
  auto& arr = *static_cast<std::array<std::byte, N>*>(*arg);
  assert(N == stream->bytes_left);
  return pb_read(stream, reinterpret_cast<uint8_t*>(arr.data()),
                 std::min(stream->bytes_left, N));
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

template <std::size_t N>
bool pbEncodeByteArray(pb_ostream_t* stream, const pb_field_t* field,
                       void* const* arg) {
  auto& arr = *static_cast<std::array<std::byte, N>*>(*arg);
  if (!arr.empty()) {
    if (!pb_encode_tag_for_field(stream, field)) {
      return false;
    }

    if (!pb_encode_string(stream, reinterpret_cast<const uint8_t*>(arr.data()),
                          arr.size())) {
      return false;
    }
  }

  return true;
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

// Wrapper for a signed field that is genuinely a protobuf sint32/sint64 on
// the wire (zigzag varint) - the common case for a signed C++ field is a
// plain proto int32/int64 (two's complement varint, same as unsigned,
// just reinterpreted), which bindField() below already handles by
// default. Only wrap a field in this when its .proto declares it
// `sint32`/`sint64` specifically (e.g. Track.duration/Episode.duration in
// metadata.proto) - using this for a plain int32/int64 field, or leaving
// a genuine sint32/64 field unwrapped, silently corrupts every decoded/
// encoded value via zigzag (n>>1)^-(n&1), which isn't the identity
// transform and doesn't preserve ordering either.
template <typename T>
struct ZigZag {
  T value{};

  ZigZag() = default;
  ZigZag(T v) : value(v) {}  // NOLINT(google-explicit-constructor)
  operator T() const { return value; }
  ZigZag& operator=(T v) {
    value = v;
    return *this;
  }

  static bool decode(pb_istream_t* stream, const pb_field_t* field,
                     void** arg) {
    auto* self = static_cast<ZigZag<T>*>(*arg);
    void* valuePtr = &self->value;
    return pbDecodeSvarint<T>(stream, field, &valuePtr);
  }

  static bool encode(pb_ostream_t* stream, const pb_field_t* field,
                     void* const* arg) {
    auto* self = static_cast<ZigZag<T>*>(const_cast<void*>(*arg));
    void* valuePtr = &self->value;
    return pbEncodeSvarint<T>(stream, field, &valuePtr);
  }
};

template <typename>
struct is_zigzag : std::false_type {};
template <typename T>
struct is_zigzag<ZigZag<T>> : std::true_type {};
template <typename T>
inline constexpr bool is_zigzag_v = is_zigzag<T>::value;

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
  }

// Helper to check if a type is a std::vector
template <typename>
struct is_vector : std::false_type {};
template <typename T, typename A>
struct is_vector<std::vector<T, A>> : std::true_type {};
template <typename T>
inline constexpr bool is_vector_v = is_vector<T>::value;

// Helper to check if a type is a std::array
template <typename>
struct is_array : std::false_type {};
template <typename T, std::size_t N>
struct is_array<std::array<T, N>> : std::true_type {};
template <typename T>
inline constexpr bool is_array_v = is_array<T>::value;

// Helper to check if a type is your custom Optional
template <typename>
struct is_optional : std::false_type {};
template <typename T>
struct is_optional<Optional<T>> : std::true_type {};
template <typename T>
inline constexpr bool is_optional_v = is_optional<T>::value;

template <typename T>
struct always_false : std::false_type {};

template <typename FieldT>
void bindField(pb_callback_t& pbField, FieldT& field, bool isDecode) {
  // Use compile-time checks to determine the field's type
  if constexpr (is_optional_v<FieldT>) {
    using T = typename std::remove_reference<decltype(field.value)>::type;
    bindField(field.wrappedCallback, field.value, isDecode);
    if (isDecode)
      pbField.funcs.decode = &Optional<T>::decode;
    else
      pbField.funcs.encode = &Optional<T>::encode;

  } else if constexpr (is_zigzag_v<FieldT>) {
    if (isDecode)
      pbField.funcs.decode = &FieldT::decode;
    else
      pbField.funcs.encode = &FieldT::encode;

  } else if constexpr (std::is_same_v<FieldT, std::string>) {
    if (isDecode)
      pbField.funcs.decode = &pbDecodeString;
    else
      pbField.funcs.encode = &pbEncodeString;
  } else if constexpr (is_vector_v<FieldT>) {
    // Vector types
    using T = typename FieldT::value_type;  // The type inside the vector
    if constexpr (std::is_same_v<T, std::string>) {
      if (isDecode)
        pbField.funcs.decode = &pbDecodeStringList;
      else
        pbField.funcs.encode = &pbEncodeStringList;
    } else if constexpr (std::is_integral_v<T>) {
      if (isDecode)

        pbField.funcs.decode = &pbDecodeVarintList<T>;
      else
        pbField.funcs.encode = &pbEncodeVarintList<T>;
    } else if constexpr (std::is_same_v<T, uint8_t>) {
      if (isDecode)
        pbField.funcs.decode = &pbDecodeUint8Vector;
      else
        pbField.funcs.encode = &pbEncodeUint8Vector;
    } else if constexpr (std::is_same_v<T, std::byte>) {
      if (isDecode)

        pbField.funcs.decode = &pbDecodeByteVector;
      else
        pbField.funcs.encode = &pbEncodeByteVector;
    } else if constexpr (std::is_class_v<T>) {
      // This handles std::vector<YourStruct>
      if (isDecode)

        pbField.funcs.decode = &StructCodec<T>::decodeVector;
      else
        pbField.funcs.encode = &StructCodec<T>::encodeVector;
    }

  } else if constexpr (is_array_v<FieldT>) {
    // Array types
    using T = typename FieldT::value_type;
    constexpr std::size_t N = std::tuple_size<FieldT>::value;
    if constexpr (std::is_same_v<T, uint8_t>) {
      if (isDecode)

        pbField.funcs.decode = &pbDecodeUint8Array<N>;
      else
        pbField.funcs.encode = &pbEncodeUint8Array<N>;
    } else if constexpr (std::is_same_v<T, std::byte>) {
      if (isDecode)

        pbField.funcs.decode = &pbDecodeByteArray<N>;
      else
        pbField.funcs.encode = &pbEncodeByteArray<N>;
    }

  } else if constexpr (std::is_integral_v<FieldT>) {
    // --- All integral types (int, uint, bool) ---
    // Plain varint for everything, signed included: a signed C++ field
    // defaults to matching a plain proto int32/int64 (two's complement
    // varint, same wire representation as unsigned - pbDecodeVarint/
    // pbEncodeVarint's static_cast<IntegerT> already reinterprets the
    // bit pattern correctly for a negative value). A field that's
    // genuinely sint32/sint64 on the wire needs to opt into zigzag
    // explicitly via the ZigZag<T> wrapper above instead - see its
    // comment for why this can't be inferred from the C++ type alone.
    if (isDecode)
      pbField.funcs.decode = &pbDecodeVarint<FieldT>;
    else
      pbField.funcs.encode = &pbEncodeVarint<FieldT>;
  } else if constexpr (std::is_enum_v<FieldT>) {
    if (isDecode)
      pbField.funcs.decode = &pbDecodeVarint<FieldT>;
    else
      pbField.funcs.encode = &pbEncodeVarint<FieldT>;
  } else if constexpr (std::is_floating_point_v<FieldT>) {
    // --- Float and Double ---
    if constexpr (std::is_same_v<FieldT, float>) {
      if (isDecode)

        pbField.funcs.decode = &pbDecodeFixed32;
      else
        pbField.funcs.encode = &pbEncodeFixed32;
    } else {  // double
      if (isDecode)

        pbField.funcs.decode = &pbDecodeFixed64;
      else
        pbField.funcs.encode = &pbEncodeFixed64;
    }

  } else if constexpr (std::is_class_v<FieldT>) {
    // --- Custom Struct type ---
    // This should be the fallback for any user-defined struct
    if (isDecode)

      pbField.funcs.decode = &StructCodec<FieldT>::decode;
    else
      pbField.funcs.encode = &StructCodec<FieldT>::encodeSubmessage;
  } else {
    static_assert(always_false<FieldT>::value, "Invalid protobuf type");
  }

  // Finally, assign the argument pointer for all types
  pbField.arg = &field;
}

template <typename MessageT>
bool decodeFromBuffer(MessageT& message, const std::byte* buffer,
                      size_t bufferLen) {
  message = MessageT();  // Reset the message to its default state

  pb_istream_t stream = pb_istream_from_buffer(
      reinterpret_cast<const uint8_t*>(buffer), bufferLen);
  void* messagePtr = &message;
  bool success = nanopb_helper::StructCodec<MessageT>::decode(&stream, nullptr,
                                                              &messagePtr);
  if (!success) {
    BELL_LOG(error, "nanopb_helper", "Decoding of message failed, err={}",
             PB_GET_ERROR(&stream));
  }

  return success;
}

template <typename MessageT>
bool decodeFromVector(MessageT& message, const std::vector<std::byte>& input) {
  return decodeFromBuffer(message, input.data(), input.size());
}

template <typename MessageT>
bool encodeToVector(MessageT& message, std::vector<std::byte>& output) {
  // Actual encoding
  pb_ostream_t stream;
  stream.callback = [](pb_ostream_t* stream, const pb_byte_t* buf,
                       size_t count) -> bool {
    auto* vec = static_cast<std::vector<std::byte>*>(stream->state);
    vec->insert(vec->end(), reinterpret_cast<const std::byte*>(buf),
                reinterpret_cast<const std::byte*>(buf + count));
    return true;
  };
  stream.state = &output;
  stream.max_size = SIZE_MAX;
  stream.bytes_written = 0;
  output.clear();
  void* messagePtr = &message;
  bool success = nanopb_helper::StructCodec<MessageT>::encode(&stream, nullptr,
                                                              &messagePtr);
  if (!success) {
    BELL_LOG(error, "nanopb_helper", "Encoding of message failed, err={}",
             PB_GET_ERROR(&stream));
  }
  return success;
}
}  // namespace nanopb_helper
