#include "audio/AesCtrCipher.h"

#include <array>
#include <cstdint>

#include "bell/Logger.h"

using namespace cspot;

namespace {
// Base IV for AES decryption, incremented per block - same constant every
// Spotify CDN audio file uses (see CDNDataStream.cpp's own comment on
// kSpotifyHeaderSize for how this was confirmed).
const std::array<uint8_t, 16> aesIVBase = {
    0x72, 0xe0, 0x67, 0xfb, 0xdd, 0xcb, 0xcf, 0x77,
    0xeb, 0xe8, 0xbc, 0x64, 0x3f, 0x63, 0x0d, 0x93,
};

// Adds 'addend' to a 128-bit big-endian counter, with carry propagation
// (wraps mod 2^128, same as the AES-CTR spec expects). Generalizes the
// per-block +1 increment further down in decrypt() to an arbitrary
// addend - plain byte arithmetic instead of mbedtls_mpi (an arbitrary-
// precision bignum type that heap-allocates internally) since 'addend'
// never exceeds 64 bits and the counter itself is a fixed 16 bytes; no
// need for arbitrary precision to do this specific addition.
void addToCounter(std::array<uint8_t, 16>& counter, size_t addend) {
  uint64_t carry = addend;
  for (int i = 15; i >= 0 && carry != 0; --i) {
    uint64_t sum = counter[i] + (carry & 0xFF);
    counter[i] = static_cast<uint8_t>(sum);
    carry = (carry >> 8) + (sum >> 8);
  }
}
}  // namespace

AesCtrCipher::AesCtrCipher(const std::vector<std::byte>& decryptKey) {
  mbedtls_aes_init(&aesCtx);
  keySetupResult = mbedtls_aes_setkey_enc(
      &aesCtx, reinterpret_cast<const uint8_t*>(decryptKey.data()),
      static_cast<unsigned int>(decryptKey.size() * 8));
}

AesCtrCipher::~AesCtrCipher() {
  mbedtls_aes_free(&aesCtx);
}

bell::Result<> AesCtrCipher::decrypt(std::byte* data, size_t size,
                                     size_t position) {
  if (keySetupResult != 0) {
    BELL_LOG(error, LOG_TAG, "AES key was rejected, mbedtls error: {}",
             keySetupResult);
    return bell::make_unexpected_errc(std::errc::bad_message);
  }

  if (size == 0) {
    return {};
  }

  const size_t alignedStart = position & ~size_t(15);
  size_t intraBlockOffset = position - alignedStart;
  size_t remaining = size;
  auto* buf = reinterpret_cast<uint8_t*>(data);

  // Counter = base IV advanced by the block index - derived fresh from
  // 'position' every call, no state carried between calls.
  std::array<uint8_t, 16> counterBlock = aesIVBase;
  addToCounter(counterBlock, alignedStart / 16);

  while (remaining > 0) {
    std::array<uint8_t, 16> keystream{};
    if (mbedtls_aes_crypt_ecb(&aesCtx, MBEDTLS_AES_ENCRYPT, counterBlock.data(),
                              keystream.data()) != 0) {
      BELL_LOG(error, LOG_TAG, "Failed to generate CTR keystream block");
      return bell::make_unexpected_errc(std::errc::bad_message);
    }

    size_t take = std::min<size_t>(16 - intraBlockOffset, remaining);
    for (size_t i = 0; i < take; ++i) {
      buf[i] = static_cast<uint8_t>(buf[i]) ^ keystream[intraBlockOffset + i];
    }

    buf += take;
    remaining -= take;
    intraBlockOffset = 0;  // only applies to first block

    // Increment counter (big-endian)
    for (int i = 15; i >= 0; --i) {
      if (++counterBlock[i])
        break;
    }
  }

  return {};
}
