#include "Crypto.h"

#include <mbedtls/base64.h>  // for mbedtls_base64_encode, mbedtls_base64_decode
#include <mbedtls/pkcs5.h>   // for mbedtls_pkcs5_pbkdf2_hmac_ext

// mbedtls/bignum.h is only guaranteed public via ESP-IDF's own mbedTLS
// port (components/mbedtls/port/include/mbedtls/bignum.h) - a "stock"
// mbedTLS 4.0 install may have moved it private, and the underlying
// header isn't even on the normal public include path outside ESP-IDF's
// from-source build (only under an internal drivers/builtin/.../private/
// directory - confirmed by checking, not assumed). Confined to the DH
// functions below (dhInit()/dhCalculateShared()), the only callers of
// mbedtls_mpi_*. Migrating those to PSA instead was investigated and
// ruled out: PSA's only FFDH family is RFC 7919 (2048-bit minimum), and
// Spotify's ZeroConf key exchange uses the old 768-bit RFC 2409 "Group
// 1" - not an RFC 7919 group, and PSA has no custom-prime DH escape
// hatch. The non-ESP_PLATFORM branch below uses a small self-contained
// modular exponentiation instead of chasing mbedTLS's internals further.
// See docs/spotify_component_analysis.md, findings F98/F99/F100.
//
// Must come before <psa/crypto.h> below: ESP-IDF's bignum.h wrapper
// does `#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS` then `#include_next`s
// the real (otherwise-private) header - if psa/crypto.h gets included
// first and transitively pulls that same header in without the define
// set yet, its own #pragma once locks it in undeclared, and this
// #include below becomes a silent no-op. Reordering these two once
// already reproduced exactly that - every mbedtls_mpi_* symbol
// "undeclared in this scope" despite the header existing right there.
#ifdef ESP_PLATFORM
#include <mbedtls/bignum.h>
#endif

#include <psa/crypto.h>
#include <algorithm>
#include <cstdint>
#include <stdexcept>

#include "BellLogger.h"
#include "psa_init.h"

static const char* CRYPTO_TAG = "crypto";

#ifndef ESP_PLATFORM
namespace {
// Minimal self-contained arbitrary-precision unsigned integer - only
// used by dhInit()/dhCalculateShared()'s non-ESP_PLATFORM branch below,
// as a stand-in for the mbedtls_mpi_* calls the ESP32 build uses (see
// the comment above). Big-endian byte vectors in/out, matching
// mbedtls_mpi_read_binary/write_binary's own convention; little-endian
// 32-bit limbs internally. Binary (square-and-multiply) modular
// exponentiation with a bit-by-bit shift-subtract reduction -
// deliberately not hardened against timing side-channels or optimized:
// this only runs in a host/test build (e.g. for the ThreadSanitizer
// testing discussed for F93), never in the production ESP32 firmware,
// which always takes the ESP_PLATFORM branch and the real mbedTLS
// implementation instead. Verified against Python's native
// arbitrary-precision pow(base, exp, mod) using this project's actual
// 768-bit DH prime before being written here - see
// docs/spotify_component_analysis.md, finding F100.
class BigUint {
 public:
  std::vector<uint32_t> limbs{0};  // little-endian; never empty

  static BigUint fromBytes(const std::vector<uint8_t>& be) {
    BigUint r;
    r.limbs.assign((be.size() + 3) / 4, 0);
    for (size_t i = 0; i < be.size(); i++) {
      size_t byteFromEnd = be.size() - 1 - i;
      r.limbs[byteFromEnd / 4] |= uint32_t(be[i]) << ((byteFromEnd % 4) * 8);
    }
    r.trim();
    return r;
  }

  std::vector<uint8_t> toBytes(size_t outSize) const {
    std::vector<uint8_t> out(outSize, 0);
    for (size_t i = 0; i < outSize; i++) {
      size_t byteFromEnd = outSize - 1 - i;
      size_t limbIdx = byteFromEnd / 4;
      if (limbIdx < limbs.size()) {
        out[i] = uint8_t(limbs[limbIdx] >> ((byteFromEnd % 4) * 8));
      }
    }
    return out;
  }

  void trim() {
    while (limbs.size() > 1 && limbs.back() == 0) limbs.pop_back();
  }

  int bitLength() const {
    int n = (int)limbs.size();
    uint32_t top = limbs[n - 1];
    int bits = (n - 1) * 32;
    while (top) {
      bits++;
      top >>= 1;
    }
    return bits;
  }

  bool getBit(int i) const {
    size_t limb = i / 32;
    if (limb >= limbs.size()) return false;
    return (limbs[limb] >> (i % 32)) & 1;
  }

  static BigUint multiply(const BigUint& a, const BigUint& b) {
    BigUint r;
    r.limbs.assign(a.limbs.size() + b.limbs.size(), 0);
    for (size_t i = 0; i < a.limbs.size(); i++) {
      uint64_t carry = 0;
      for (size_t j = 0; j < b.limbs.size(); j++) {
        uint64_t cur = (uint64_t)r.limbs[i + j] +
                       (uint64_t)a.limbs[i] * (uint64_t)b.limbs[j] + carry;
        r.limbs[i + j] = (uint32_t)cur;
        carry = cur >> 32;
      }
      size_t k = i + b.limbs.size();
      while (carry) {
        uint64_t cur = (uint64_t)r.limbs[k] + carry;
        r.limbs[k] = (uint32_t)cur;
        carry = cur >> 32;
        k++;
      }
    }
    r.trim();
    return r;
  }

  // Same bit-by-bit binary long division as a naive
  // shiftLeft1()/compare()/subtract() loop would do, but operating
  // in-place on one fixed-size limb buffer instead of allocating a new
  // BigUint (and its heap-backed std::vector) on every single bit of the
  // ~1536-bit reduction. The allocating version was measured taking
  // ~9.6s for one DH exchange's two modExp() calls under ThreadSanitizer
  // (each mod() call did up to ~1536 heap allocations, called ~1500+
  // times per modExp) - long enough to blow past PlainConnection's
  // hardcoded 3s socket timeout in the F93 host test. See
  // docs/spotify_component_analysis.md, the F93 host test finding.
  static BigUint mod(const BigUint& a, const BigUint& m) {
    size_t n = m.limbs.size() + 1;
    std::vector<uint32_t> r(n, 0);

    for (int i = a.bitLength() - 1; i >= 0; i--) {
      uint32_t carry = a.getBit(i) ? 1 : 0;
      for (size_t k = 0; k < n; k++) {
        uint32_t nextCarry = r[k] >> 31;
        r[k] = (r[k] << 1) | carry;
        carry = nextCarry;
      }

      bool ge = true;
      for (size_t k = n; k-- > 0;) {
        uint32_t mv = k < m.limbs.size() ? m.limbs[k] : 0;
        if (r[k] != mv) {
          ge = r[k] > mv;
          break;
        }
      }

      if (ge) {
        int64_t borrow = 0;
        for (size_t k = 0; k < n; k++) {
          int64_t mv = k < m.limbs.size() ? m.limbs[k] : 0;
          int64_t d = (int64_t)r[k] - mv - borrow;
          if (d < 0) {
            d += ((int64_t)1 << 32);
            borrow = 1;
          } else {
            borrow = 0;
          }
          r[k] = (uint32_t)d;
        }
      }
    }

    BigUint result;
    result.limbs = std::move(r);
    result.trim();
    return result;
  }

  static BigUint modExp(BigUint base, BigUint exp, const BigUint& modulus) {
    BigUint result;
    result.limbs = {1};
    base = mod(base, modulus);
    int bits = exp.bitLength();
    for (int i = 0; i < bits; i++) {
      if (exp.getBit(i)) {
        result = mod(multiply(result, base), modulus);
      }
      base = mod(multiply(base, base), modulus);
    }
    return result;
  }
};
}  // namespace
#endif  // !ESP_PLATFORM

extern "C" {
#include "aes.h"  // for AES_ECB_decrypt, AES_init_ctx, AES_ctx (tiny-AES-c,
                  // cspot/bell/main/utilities/{include/aes.h,aes.c},
                  // unchanged - only used for aesECBdecrypt, see below)
}

static unsigned char DHGenerator[1] = {2};

namespace {
// RAII wrapper around a PSA key handle - sha1HMAC() and aesCTRXcrypt() both
// used to repeat the same psa_import_key()-then-remember-to-psa_destroy_key()
// boilerplate; a 4th use added later could easily forget the destroy call
// on some early-return/throw path. The destructor runs unconditionally on
// scope exit (normal return or exception unwind), so there's nothing left
// to forget. See docs/spotify_component_analysis.md, finding F46.
class PsaKeyHandle {
 public:
  PsaKeyHandle(const psa_key_attributes_t& attributes, const uint8_t* data,
               size_t size) {
    if (psa_import_key(&attributes, data, size, &id_) != PSA_SUCCESS) {
      throw std::runtime_error("psa_import_key failed");
    }
  }
  ~PsaKeyHandle() { psa_destroy_key(id_); }

  PsaKeyHandle(const PsaKeyHandle&) = delete;
  PsaKeyHandle& operator=(const PsaKeyHandle&) = delete;

  mbedtls_svc_key_id_t get() const { return id_; }

 private:
  mbedtls_svc_key_id_t id_;
};
}  // namespace

// sha1Context starts as garbage (a plain struct member, never
// zero-initialized otherwise) - mbedtls_md_init() just zeroes it out (no
// allocation yet), which sha1Init()/the destructor below both rely on
// being able to safely mbedtls_md_free() a not-yet-setup context. See
// docs/spotify_component_analysis.md, finding F37.
CryptoMbedTLS::CryptoMbedTLS() {
  mbedtls_md_init(&sha1Context);
}

// Safety net: if a sha1Init()...sha1Update() sequence never reached
// sha1FinalBytes()/sha1Final() (the only two functions that previously
// freed sha1Context) - e.g. an exception thrown mid-sequence by one of the
// other Crypto calls interleaved with it in LoginBlob::decodeBlobSecondary()
// - the md context's internal allocation was leaked for the lifetime of
// this object. See docs/spotify_component_analysis.md, finding F37.
CryptoMbedTLS::~CryptoMbedTLS() {
  mbedtls_md_free(&sha1Context);
}

std::vector<uint8_t> CryptoMbedTLS::base64Decode(const std::string& data) {
  // Calculate max decode length
  size_t requiredSize;

  mbedtls_base64_encode(nullptr, 0, &requiredSize, (unsigned char*)data.c_str(),
                        data.size());

  std::vector<uint8_t> output(requiredSize);
  size_t outputLen = 0;
  // The original silently ignored this return code - kept as a real check
  // now (not just debug logging) since a malformed/truncated decode here
  // otherwise propagates as a confusing crash several calls further down
  // (see docs/spotify_component_analysis.md, section 2 / finding F14).
  int rc = mbedtls_base64_decode(output.data(), requiredSize, &outputLen,
                                 (unsigned char*)data.c_str(), data.size());
  if (rc != 0) {
    BELL_LOG(error, CRYPTO_TAG, "base64Decode failed: rc=%d input_len=%u",
             rc, (unsigned)data.size());
    throw std::runtime_error("base64Decode failed");
  }

  return std::vector<uint8_t>(output.begin(), output.begin() + outputLen);
}

std::string CryptoMbedTLS::base64Encode(const std::vector<uint8_t>& data) {
  // Calculate max output length
  size_t requiredSize;
  mbedtls_base64_encode(nullptr, 0, &requiredSize, data.data(), data.size());

  std::vector<uint8_t> output(requiredSize);
  size_t outputLen = 0;

  mbedtls_base64_encode(output.data(), requiredSize, &outputLen, data.data(),
                        data.size());

  return std::string(output.begin(), output.begin() + outputLen);
}

// Sha1
//
// The original passed hmac=1 to mbedtls_md_setup() here, even though this
// context is only ever used for plain hashing - sha1HMAC() (below) goes
// through the separate PSA MAC API and never touches this context at all.
// That was probably harmless-but-wasteful pre-4.0, but md.h now says
// outright: "From TF-PSA-Crypto 1.0 and Mbed TLS 4.0 onwards, hmac MUST be
// set to 0. HMAC operations are no longer supported via MD" - and
// mbedtls_md_setup()'s return value was never checked, so a failure here
// was invisible: every "plain SHA1" hash through this (LoginBlob's
// baseKey/secret/baseKeyHashed derivations) silently produced garbage,
// which is what was actually behind the "Mac doesn't match" /
// LoadProhibited crashes during ZeroConf pairing on real hardware, not the
// URL-decoding fixed earlier. See docs/spotify_component_analysis.md,
// finding F15.
void CryptoMbedTLS::sha1Init() {
  // Free first: if a previous sha1Init()...sha1Update() sequence never
  // reached sha1FinalBytes()/sha1Final(), mbedtls_md_setup()'s internal
  // allocation from that sequence would otherwise be silently orphaned
  // when this call overwrites sha1Context. Safe to call even on a
  // never-setup context (constructor zeroes it via mbedtls_md_init()).
  // See docs/spotify_component_analysis.md, finding F37.
  mbedtls_md_free(&sha1Context);

  // Init mbedtls md context, pick sha1
  mbedtls_md_init(&sha1Context);
  mbedtls_md_setup(&sha1Context, mbedtls_md_info_from_type(MBEDTLS_MD_SHA1), 0);
  mbedtls_md_starts(&sha1Context);
}

void CryptoMbedTLS::sha1Update(const std::string& s) {
  sha1Update(std::vector<uint8_t>(s.begin(), s.end()));
}
void CryptoMbedTLS::sha1Update(const std::vector<uint8_t>& vec) {
  mbedtls_md_update(&sha1Context, vec.data(), vec.size());
}

std::vector<uint8_t> CryptoMbedTLS::sha1FinalBytes() {
  std::vector<uint8_t> digest(20);  // SHA1 digest size

  mbedtls_md_finish(&sha1Context, digest.data());
  mbedtls_md_free(&sha1Context);

  return digest;
}

std::string CryptoMbedTLS::sha1Final() {
  auto digest = sha1FinalBytes();
  return std::string(digest.begin(), digest.end());
}

// HMAC SHA1
//
// mbedtls_md_hmac_{setup,starts,update,finish}() still exist in md.h but
// are gated behind MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS in mbedTLS 4.0 (see
// md.h: "HMAC operations are no longer supported via MD") - not meant for
// application use anymore. PSA's one-shot MAC API replaces them directly.
std::vector<uint8_t> CryptoMbedTLS::sha1HMAC(
    const std::vector<uint8_t>& inputKey, const std::vector<uint8_t>& message) {
  ensurePsaCryptoInit();

  psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
  psa_set_key_type(&attributes, PSA_KEY_TYPE_HMAC);
  psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_MESSAGE);
  psa_set_key_algorithm(&attributes, PSA_ALG_HMAC(PSA_ALG_SHA_1));

  PsaKeyHandle key(attributes, inputKey.data(), inputKey.size());

  std::vector<uint8_t> digest(20);  // SHA1 digest size
  size_t macLen = 0;
  psa_status_t status =
      psa_mac_compute(key.get(), PSA_ALG_HMAC(PSA_ALG_SHA_1), message.data(),
                      message.size(), digest.data(), digest.size(), &macLen);

  if (status != PSA_SUCCESS) {
    BELL_LOG(error, CRYPTO_TAG, "sha1HMAC: psa status=%d", (int)status);
    throw std::runtime_error("sha1HMAC failed");
  }

  return digest;
}

// AES CTR
//
// Ported from mbedtls_aes_crypt_ctr (mbedtls/aes.h, removed in mbedTLS 4.0)
// to the PSA cipher API - not to the tiny-AES-c library used below for ECB,
// because that copy is compiled for a fixed AES-192 (24-byte) key
// (external/../aes.h: `#define AES192 1`) while both callers of this
// function (CDNAudioFile::decrypt, LoginBlob::loadZeroconfQuery) use
// 16-byte (AES-128) keys - reusing it here would silently truncate/misread
// the key. PSA supports any AES key size through one API.
//
// Unlike the original, this does not write the advanced counter back into
// `iv` after processing - both current call sites compute a fresh IV per
// call (CDNAudioFile derives it from the absolute stream position, and
// LoginBlob only calls this once), so the original's implicit
// counter-continuation-via-mutation behavior is never actually relied upon.
void CryptoMbedTLS::aesCTRXcrypt(const std::vector<uint8_t>& key,
                                 std::vector<uint8_t>& iv, uint8_t* buffer,
                                 size_t nbytes) {
  ensurePsaCryptoInit();

  psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
  psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
  psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_DECRYPT);
  psa_set_key_algorithm(&attributes, PSA_ALG_CTR);

  PsaKeyHandle aesKey(attributes, key.data(), key.size());

  psa_cipher_operation_t op = PSA_CIPHER_OPERATION_INIT;
  psa_status_t status = psa_cipher_decrypt_setup(&op, aesKey.get(), PSA_ALG_CTR);
  if (status == PSA_SUCCESS) {
    status = psa_cipher_set_iv(&op, iv.data(), iv.size());
  }

  std::vector<uint8_t> out(nbytes);
  size_t outLen = 0, finalLen = 0;
  if (status == PSA_SUCCESS) {
    status = psa_cipher_update(&op, buffer, nbytes, out.data(), out.size(),
                               &outLen);
  }
  if (status == PSA_SUCCESS) {
    status = psa_cipher_finish(&op, out.data() + outLen,
                               out.size() - outLen, &finalLen);
  }
  psa_cipher_abort(&op);

  if (status != PSA_SUCCESS) {
    throw std::runtime_error("Failed to decrypt");
  }
  std::copy(out.begin(), out.begin() + outLen + finalLen, buffer);
}

void CryptoMbedTLS::aesECBdecrypt(const std::vector<uint8_t>& key,
                                  std::vector<uint8_t>& data) {

  struct AES_ctx aesCtr;
  AES_init_ctx(&aesCtr, key.data());

  for (unsigned int x = 0; x < data.size() / 16; x++) {

    AES_ECB_decrypt(&aesCtr, data.data() + (x * 16));
  }
}

// PBKDF2
//
// Real ZeroConf pairing against this exact PSA-based implementation
// failed on a real, currently-supported build (Ubuntu's mbedtls
// 3.6.2-3ubuntu1, this repo's own extras/cli host target):
// psa_key_derivation_setup(PSA_ALG_PBKDF2_HMAC(PSA_ALG_SHA_1)) returns
// PSA_ERROR_NOT_SUPPORTED there - PBKDF2-HMAC as a PSA key-derivation
// algorithm isn't compiled into every mbedtls build, distro-packaged or
// otherwise. mbedtls_pkcs5_pbkdf2_hmac_ext() (non-deprecated, still
// public in 3.x) has no such gate and is verified against the RFC 6070
// PBKDF2-HMAC-SHA1 test vector on that same system.
std::vector<uint8_t> CryptoMbedTLS::pbkdf2HmacSha1(
    const std::vector<uint8_t>& password, const std::vector<uint8_t>& salt,
    int iterations, int digestSize) {
  auto digest = std::vector<uint8_t>(digestSize);

  int ret = mbedtls_pkcs5_pbkdf2_hmac_ext(
      MBEDTLS_MD_SHA1, password.data(), password.size(), salt.data(),
      salt.size(), iterations, digestSize, digest.data());

  if (ret != 0) {
    throw std::runtime_error("pbkdf2HmacSha1 failed");
  }

  return digest;
}

void CryptoMbedTLS::dhInit() {
  privateKey = generateVectorWithRandomData(DH_KEY_SIZE);

#ifdef ESP_PLATFORM
  // initialize big num
  mbedtls_mpi prime, generator, res, privKey;
  mbedtls_mpi_init(&prime);
  mbedtls_mpi_init(&generator);
  mbedtls_mpi_init(&privKey);
  mbedtls_mpi_init(&res);

  // Read bin into big num mpi
  mbedtls_mpi_read_binary(&prime, DHPrime, sizeof(DHPrime));
  mbedtls_mpi_read_binary(&generator, DHGenerator, sizeof(DHGenerator));
  mbedtls_mpi_read_binary(&privKey, privateKey.data(), DH_KEY_SIZE);

  // perform diffie hellman G^X mod P
  mbedtls_mpi_exp_mod(&res, &generator, &privKey, &prime, NULL);

  // Write generated public key to vector
  this->publicKey = std::vector<uint8_t>(DH_KEY_SIZE);
  mbedtls_mpi_write_binary(&res, publicKey.data(), DH_KEY_SIZE);

  // Release memory
  mbedtls_mpi_free(&prime);
  mbedtls_mpi_free(&generator);
  mbedtls_mpi_free(&privKey);
  mbedtls_mpi_free(&res);
#else
  // See the BigUint comment above (F100) - same math as the
  // ESP_PLATFORM branch, self-contained instead of mbedtls_mpi_*.
  auto prime = BigUint::fromBytes(
      std::vector<uint8_t>(DHPrime, DHPrime + sizeof(DHPrime)));
  auto generator = BigUint::fromBytes(std::vector<uint8_t>(
      DHGenerator, DHGenerator + sizeof(DHGenerator)));
  auto privKey = BigUint::fromBytes(privateKey);

  auto res = BigUint::modExp(generator, privKey, prime);
  this->publicKey = res.toBytes(DH_KEY_SIZE);
#endif
}

std::vector<uint8_t> CryptoMbedTLS::dhCalculateShared(
    const std::vector<uint8_t>& remoteKey) {
#ifdef ESP_PLATFORM
  // initialize big num
  mbedtls_mpi prime, remKey, res, privKey;
  mbedtls_mpi_init(&prime);
  mbedtls_mpi_init(&remKey);
  mbedtls_mpi_init(&privKey);
  mbedtls_mpi_init(&res);

  // Read bin into big num mpi
  mbedtls_mpi_read_binary(&prime, DHPrime, sizeof(DHPrime));
  mbedtls_mpi_read_binary(&remKey, remoteKey.data(), remoteKey.size());
  mbedtls_mpi_read_binary(&privKey, privateKey.data(), DH_KEY_SIZE);

  // perform diffie hellman (G^Y)^X mod P (for shared secret)
  mbedtls_mpi_exp_mod(&res, &remKey, &privKey, &prime, NULL);

  auto sharedKey = std::vector<uint8_t>(DH_KEY_SIZE);
  mbedtls_mpi_write_binary(&res, sharedKey.data(), DH_KEY_SIZE);

  // Release memory
  mbedtls_mpi_free(&prime);
  mbedtls_mpi_free(&remKey);
  mbedtls_mpi_free(&privKey);
  mbedtls_mpi_free(&res);

  return sharedKey;
#else
  // See the BigUint comment above (F100) - same math as the
  // ESP_PLATFORM branch, self-contained instead of mbedtls_mpi_*.
  auto prime = BigUint::fromBytes(
      std::vector<uint8_t>(DHPrime, DHPrime + sizeof(DHPrime)));
  auto remKey = BigUint::fromBytes(remoteKey);
  auto privKey = BigUint::fromBytes(privateKey);

  auto res = BigUint::modExp(remKey, privKey, prime);
  return res.toBytes(DH_KEY_SIZE);
#endif
}

// Random stuff
//
// mbedtls/ctr_drbg.h and mbedtls/entropy.h were removed in mbedTLS 4.0;
// psa_generate_random() is the portable replacement everywhere,
// including ESP-IDF - no ESP_PLATFORM branch needed. On ESP-IDF,
// components/mbedtls/port/esp_hardware.c's mbedtls_psa_external_get_random()
// is a direct passthrough to esp_fill_random() (the hardware TRNG, no DRBG
// layer in between), enabled unconditionally for real targets via
// MBEDTLS_PSA_DRIVER_GET_ENTROPY in esp_config.h - so this call already
// resolves to esp_fill_random() there, same as calling it directly would,
// with the portability of not needing a platform branch (see
// docs/aprendizaje.md, "Seguridad: TLS, verificación de certificados y
// mbedTLS", section 4.1).
std::vector<uint8_t> CryptoMbedTLS::generateVectorWithRandomData(
    size_t length) {
  ensurePsaCryptoInit();
  std::vector<uint8_t> randomVector(length);
  psa_generate_random(randomVector.data(), length);
  return randomVector;
}
