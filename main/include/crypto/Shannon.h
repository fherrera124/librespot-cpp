#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace cspot {
// Implementation of the Shannon stream cipher, used for communication with the Spotify AP
class Shannon {
 public:
  void key(const std::byte* key, size_t keyLen);          /* set key */
  void nonce(const std::byte* nonce, size_t nonceLen);    /* set Init Vector */
  void encrypt(std::byte* buffer, size_t bufferLen);      /* encrypt + MAC */
  void decrypt(std::byte* buffer, size_t bufferLen);      /* finalize + MAC */
  void finish(std::byte* macBuffer, size_t macBufferLen); /* finalise MAC */

 private:
  static constexpr unsigned int N = 16;
  static constexpr unsigned int INITKONST = 0x6996c53a;
  static constexpr unsigned int KEYP = 13;
  std::array<uint32_t, Shannon::N> R;
  std::array<uint32_t, Shannon::N> CRC;
  std::array<uint32_t, Shannon::N> initR;
  uint32_t konst;
  uint32_t sbuf;
  uint32_t mbuf;
  int nbuf;
  static uint32_t sbox1(uint32_t w);
  static uint32_t sbox2(uint32_t w);
  void cycle();
  void crcfunc(uint32_t i);
  void macfunc(uint32_t i);
  void initState();
  void saveState();
  void reloadState();
  void genkonst();
  void diffuse();
  void loadKey(const std::byte* key, size_t keyLen);
};
}  // namespace cspot
