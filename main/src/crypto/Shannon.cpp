#include "crypto/Shannon.h"

#include <array>
#include <climits>
#include <cstddef>

using namespace cspot;

namespace {
// Rotates a 32-bit integer left.
inline uint32_t rotl(uint32_t n, unsigned int c) {
  const unsigned int mask = (CHAR_BIT * sizeof(n) - 1);
  c &= mask;
  return (n << c) | (n >> ((-c) & mask));
}

// Converts 4 bytes to a 32-bit word.
inline uint32_t bytesToWord(const std::byte* b) {
  return (static_cast<uint32_t>(b[3]) << 24) |
         (static_cast<uint32_t>(b[2]) << 16) |
         (static_cast<uint32_t>(b[1]) << 8) | (static_cast<uint32_t>(b[0]));
}

// Convert a 32-bit word to 4 bytes.
inline void wordToBytes(uint32_t w, std::byte* b) {
  b[3] = static_cast<std::byte>(w >> 24);
  b[2] = static_cast<std::byte>(w >> 16);
  b[1] = static_cast<std::byte>(w >> 8);
  b[0] = static_cast<std::byte>(w);
}
}  // namespace

// --- No changes needed in the functions below this line ---

uint32_t Shannon::sbox1(uint32_t w) {
  w ^= rotl(w, 5) | rotl(w, 7);
  w ^= rotl(w, 19) | rotl(w, 22);
  return w;
}

uint32_t Shannon::sbox2(uint32_t w) {
  w ^= rotl(w, 7) | rotl(w, 22);
  w ^= rotl(w, 5) | rotl(w, 19);
  return w;
}

void Shannon::cycle() {
  uint32_t t;

  /* nonlinear feedback function */
  t = this->R[12] ^ this->R[13] ^ this->konst;
  t = Shannon::sbox1(t) ^ rotl(this->R[0], 1);
  /* shift register */
  for (uint32_t i = 1; i < N; ++i)
    this->R[i - 1] = this->R[i];
  this->R[N - 1] = t;
  t = Shannon::sbox2(this->R[2] ^ this->R[15]);
  this->R[0] ^= t;
  this->sbuf = t ^ this->R[8] ^ this->R[12];
}

void Shannon::crcfunc(uint32_t i) {
  uint32_t t;

  /* Accumulate CRC of input */
  t = this->CRC[0] ^ this->CRC[2] ^ this->CRC[15] ^ i;
  for (uint32_t j = 1; j < N; ++j)
    this->CRC[j - 1] = this->CRC[j];
  this->CRC[N - 1] = t;
}

void Shannon::macfunc(uint32_t i) {
  this->crcfunc(i);
  this->R[KEYP] ^= i;
}

void Shannon::initState() {
  /* Register initialised to Fibonacci numbers; Counter zeroed. */
  this->R[0] = 1;
  this->R[1] = 1;
  for (uint32_t i = 2; i < N; ++i)
    this->R[i] = this->R[i - 1] + this->R[i - 2];
  this->konst = Shannon::INITKONST;
}

void Shannon::saveState() {
  for (uint32_t i = 0; i < Shannon::N; ++i)
    this->initR[i] = this->R[i];
}

void Shannon::reloadState() {
  for (uint32_t i = 0; i < Shannon::N; ++i)
    this->R[i] = this->initR[i];
}

void Shannon::genkonst() {
  this->konst = this->R[0];
}

void Shannon::diffuse() {
  for (uint16_t i = 0; i < Shannon::N; ++i) {
    this->cycle();
  }
}

// --- Start of functions with fixes ---

void Shannon::loadKey(const std::byte* key, size_t keyLen) {
  uint32_t i;
  uint32_t j;
  uint32_t k;

  /* start folding in key */
  for (i = 0; i < (keyLen & ~0x3); i += 4) {
    k = bytesToWord(&key[i]);
    this->R[KEYP] ^= k;
    this->cycle();
  }

  /* if there were any extra key bytes, zero pad to a word */
  if (i < keyLen) {
    // Correctly handle partial words with a std::byte array
    std::array<std::byte, 4> xtra{};
    for (j = 0; i < keyLen; ++i, ++j)
      xtra[j] = key[i];
    k = bytesToWord(xtra.data());
    this->R[KEYP] ^= k;
    this->cycle();
  }

  /* also fold in the length of the key */
  this->R[KEYP] ^= keyLen;
  this->cycle();

  /* save a copy of the register */
  for (i = 0; i < N; ++i)
    this->CRC[i] = this->R[i];

  /* now diffuse */
  this->diffuse();

  /* now xor the copy back -- makes key loading irreversible */
  for (i = 0; i < N; ++i)
    this->R[i] ^= this->CRC[i];
}

void Shannon::key(const std::byte* key, size_t keyLen) {
  this->initState();
  this->loadKey(key, keyLen);
  this->genkonst(); /* in case we proceed to stream generation */
  this->saveState();
  this->nbuf = 0;
}

void Shannon::nonce(const std::byte* nonce, size_t nonceLen) {
  this->reloadState();
  this->konst = Shannon::INITKONST;
  this->loadKey(nonce, nonceLen);
  this->genkonst();
  this->nbuf = 0;
}

void Shannon::encrypt(std::byte* buffer, size_t bufferLen) {
  /* handle any previously buffered bytes */
  if (this->nbuf != 0) {
    while (this->nbuf != 0 && bufferLen != 0) {
      // Cast std::byte to integer for bitwise operations
      this->mbuf ^= static_cast<uint32_t>(*buffer) << (32 - this->nbuf);
      // Cast result back to std::byte for assignment
      *buffer ^=
          static_cast<std::byte>((this->sbuf >> (32 - this->nbuf)) & 0xFF);
      ++buffer;
      this->nbuf -= 8;
      --bufferLen;
    }
    if (this->nbuf != 0) /* not a whole word yet */
      return;
    this->macfunc(this->mbuf);
  }

  /* handle whole words */
  std::byte* endbuf = &buffer[bufferLen & ~((uint32_t)0x03)];
  while (buffer < endbuf) {
    this->cycle();
    uint32_t t = bytesToWord(buffer);
    this->macfunc(t);
    t ^= this->sbuf;
    wordToBytes(t, buffer);
    buffer += 4;
  }

  /* handle any trailing bytes */
  bufferLen &= 0x03;
  if (bufferLen != 0) {
    this->cycle();
    this->mbuf = 0;
    this->nbuf = 32;
    while (this->nbuf != 0 && bufferLen != 0) {
      this->mbuf ^= static_cast<uint32_t>(*buffer) << (32 - this->nbuf);
      *buffer ^=
          static_cast<std::byte>((this->sbuf >> (32 - this->nbuf)) & 0xFF);
      ++buffer;
      this->nbuf -= 8;
      --bufferLen;
    }
  }
}

void Shannon::decrypt(std::byte* buffer, size_t bufferLen) {
  /* handle any previously buffered bytes */
  if (this->nbuf != 0) {
    while (this->nbuf != 0 && bufferLen != 0) {
      *buffer ^=
          static_cast<std::byte>((this->sbuf >> (32 - this->nbuf)) & 0xFF);
      this->mbuf ^= static_cast<uint32_t>(*buffer) << (32 - this->nbuf);
      ++buffer;
      this->nbuf -= 8;
      --bufferLen;
    }
    if (this->nbuf != 0) /* not a whole word yet */
      return;
    this->macfunc(this->mbuf);
  }

  /* handle whole words */
  std::byte* endbuf = &buffer[bufferLen & ~((uint32_t)0x03)];
  while (buffer < endbuf) {
    this->cycle();
    uint32_t t = bytesToWord(buffer) ^ this->sbuf;
    this->macfunc(t);
    wordToBytes(t, buffer);
    buffer += 4;
  }

  /* handle any trailing bytes */
  bufferLen &= 0x03;
  if (bufferLen != 0) {
    this->cycle();
    this->mbuf = 0;
    this->nbuf = 32;
    while (this->nbuf != 0 && bufferLen != 0) {
      *buffer ^=
          static_cast<std::byte>((this->sbuf >> (32 - this->nbuf)) & 0xFF);
      this->mbuf ^= static_cast<uint32_t>(*buffer) << (32 - this->nbuf);
      ++buffer;
      this->nbuf -= 8;
      --bufferLen;
    }
  }
}

void Shannon::finish(std::byte* macBuffer, size_t macBufferLen) {
  /* handle any previously buffered bytes */
  if (this->nbuf != 0) {
    this->macfunc(this->mbuf);
  }

  this->cycle();
  this->R[KEYP] ^= (INITKONST ^ (this->nbuf << 3));
  this->nbuf = 0;

  /* now add the CRC to the stream register and diffuse it */
  for (uint32_t i = 0; i < N; ++i)
    this->R[i] ^= this->CRC[i];
  this->diffuse();

  /* produce output from the stream buffer */
  while (macBufferLen > 0) {
    this->cycle();
    if (macBufferLen >= 4) {
      wordToBytes(this->sbuf, macBuffer);
      macBufferLen -= 4;
      macBuffer += 4;
    } else {
      for (uint32_t i = 0; i < macBufferLen; ++i) {
        // Extract i-th byte and cast to std::byte
        macBuffer[i] = static_cast<std::byte>((this->sbuf >> (8 * i)) & 0xFF);
      }
      break;
    }
  }
}
