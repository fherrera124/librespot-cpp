#pragma once

#if __has_include(<bit>)
#include <bit>  // for endian
#endif

// This toolchain's <bit> (xtensa-esp-elf GCC 15.2 / picolibc) doesn't define
// __cpp_lib_endian despite compiling in a C++20-or-later mode - std::endian
// itself ends up simply absent from the header. Backport it with the exact
// values the standard mandates (endian::little/big are the platform's own
// byte-order tags; native equals whichever one matches), not a workaround
// abstraction - a real gap in this specific vendored toolchain, confirmed
// missing, not assumed.
#ifndef __cpp_lib_endian
#include <cstdint>
namespace std {
enum class endian {
  little = __ORDER_LITTLE_ENDIAN__,
  big = __ORDER_BIG_ENDIAN__,
  native = __BYTE_ORDER__
};
}  // namespace std
#endif

#include <cstddef>   // for byte
#include <cstdint>   // for int16_t, int32_t, int64_t, uint16_t, uint32_t
#include <iostream>  // for istream, ostream

namespace bell::io {
class BinaryStream {
 public:
  BinaryStream(std::ostream* ostr);
  BinaryStream(std::istream* istr);

  /**
   * @brief Set byte order used by stream.
   *
   * @param byteOrder stream's byteorder. Defaults to native.
   */
  void setByteOrder(std::endian byteOrder);

  /**
   * @brief Skip a number of bytes in the stream.
   * 
   * @param bytes Number of bytes to skip.
   */
  void skip(ssize_t bytes);

  // Read operations
  BinaryStream& operator>>(char& value);
  BinaryStream& operator>>(std::byte& value);
  BinaryStream& operator>>(uint8_t& value);
  BinaryStream& operator>>(int16_t& value);
  BinaryStream& operator>>(uint16_t& value);
  BinaryStream& operator>>(int32_t& value);
  BinaryStream& operator>>(uint32_t& value);
  BinaryStream& operator>>(int64_t& value);
  BinaryStream& operator>>(uint64_t& value);

  // Write operations
  BinaryStream& operator<<(char value);
  BinaryStream& operator<<(std::byte value);
  BinaryStream& operator<<(uint8_t value);
  BinaryStream& operator<<(int16_t value);
  BinaryStream& operator<<(uint16_t value);
  BinaryStream& operator<<(int32_t value);
  BinaryStream& operator<<(uint32_t value);
  BinaryStream& operator<<(int64_t value);
  BinaryStream& operator<<(uint64_t value);

 private:
  std::endian byteOrder;

  std::istream* istr = nullptr;
  std::ostream* ostr = nullptr;

  void ensureReadable();
  void ensureWritable();
  bool flipBytes = false;

  template <typename T>
  T swap16(T value) {
#ifdef _WIN32
    return _byteswap_ushort(value);
#else
    return __builtin_bswap16(value);
#endif
  }
  template <typename T>
  T swap32(T value) {
#ifdef _WIN32
    return _byteswap_ulong(value);
#else
    return __builtin_bswap32(value);
#endif
  }
  template <typename T>
  T swap64(T value) {
#ifdef _WIN32
    return _byteswap_uint64(value);
#else
    return __builtin_bswap64(value);
#endif
  }
};
}  // namespace bell::io

namespace bell {
using BinaryStream = io::BinaryStream;
}
