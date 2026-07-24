#include "bell/utils/DigestCrypto.h"
#include "fmt/format.h"

#include <psa/crypto.h>

using namespace bell;

namespace {
// PSA crypto must be initialized once before first use. A function-local
// static makes this thread-safe and exactly-once.
void ensurePsaCryptoInit() {
  static const psa_status_t status = psa_crypto_init();
  (void)status;
}

// PSA_ALG_HMAC() needs a PSA hash algorithm, not an mbedtls_md_type_t -
// covers the types this class is realistically constructed with.
psa_algorithm_t psaHashAlgFor(mbedtls_md_type_t type) {
  switch (type) {
    case MBEDTLS_MD_MD5:
      return PSA_ALG_MD5;
    case MBEDTLS_MD_SHA1:
      return PSA_ALG_SHA_1;
    case MBEDTLS_MD_SHA256:
      return PSA_ALG_SHA_256;
    case MBEDTLS_MD_SHA384:
      return PSA_ALG_SHA_384;
    case MBEDTLS_MD_SHA512:
      return PSA_ALG_SHA_512;
    default:
      throw std::invalid_argument(
          "DigestCrypto::getHmac: unsupported hash type for PSA_ALG_HMAC");
  }
}

// RAII wrapper around a PSA key handle - the destructor runs unconditionally
// on scope exit (normal return or exception unwind), so there's nothing to
// forget on an early-return/throw path.
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

utils::DigestCrypto::DigestCrypto(mbedtls_md_type_t type) {
  if (type == MBEDTLS_MD_NONE) {
    throw std::invalid_argument("Invalid hash type");
  }

  digestType = type;

  // Initialize the context. Always a plain digest (never mbedtls_md's own
  // HMAC mode, "hmac" param 0) - getHmac() below goes through PSA's MAC API
  // instead and never touches this context at all. mbedTLS 4.0 requires
  // this: "From TF-PSA-Crypto 1.0 and Mbed TLS 4.0 onwards, hmac MUST be
  // set to 0. HMAC operations are no longer supported via MD" (md.h).
  mbedtls_md_init(&ctx);
  auto result = mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(type), 0);
  if (result != 0) {
    throw std::runtime_error(fmt::format(
        "Failed to setup digest context, mbedtls error: {}", result));
  }

  reset();
}

utils::DigestCrypto::~DigestCrypto() {
  // Free the context
  mbedtls_md_free(&ctx);
}

void utils::DigestCrypto::reset() {
  // Reset the context
  auto result = mbedtls_md_starts(&ctx);
  if (result != 0) {
    throw std::runtime_error(fmt::format(
        "Failed to reset digest context, mbedtls error: {}", result));
  }
}

void utils::DigestCrypto::update(const std::byte* bytes, size_t length) {
  // Update the context with the specified bytes
  auto result =
      mbedtls_md_update(&ctx, reinterpret_cast<const uint8_t*>(bytes), length);
  if (result != 0) {
    throw std::runtime_error(fmt::format(
        "Failed to update digest context, mbedtls error: {}", result));
  }
}

void utils::DigestCrypto::updateString(std::string_view str) {
  // Update the context with the specified string
  update(reinterpret_cast<const std::byte*>(str.data()), str.size());
}

void utils::DigestCrypto::finish(std::byte* output) {
  // Finalize the context and store the result in the output array
  auto result = mbedtls_md_finish(&ctx, reinterpret_cast<uint8_t*>(output));
  if (result != 0) {
    throw std::runtime_error(fmt::format(
        "Failed to finish digest computation, mbedtls error: {}", result));
  }
}

size_t utils::DigestCrypto::getDigestSize() {
  // Get the length of the digest
  return mbedtls_md_get_size(mbedtls_md_info_from_type(digestType));
}

void utils::DigestCrypto::getDigest(const std::byte* bytes, size_t length,
                                    std::byte* output) {
  reset();
  update(bytes, length);
  finish(output);
}

void utils::DigestCrypto::getHmac(const std::byte* key, size_t keyLength,
                                  const std::byte* message,
                                  size_t messageLength, std::byte* output) {
  ensurePsaCryptoInit();

  psa_algorithm_t hashAlg = psaHashAlgFor(digestType);

  psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
  psa_set_key_type(&attributes, PSA_KEY_TYPE_HMAC);
  psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_MESSAGE);
  psa_set_key_algorithm(&attributes, PSA_ALG_HMAC(hashAlg));

  PsaKeyHandle keyHandle(attributes, reinterpret_cast<const uint8_t*>(key),
                        keyLength);

  size_t macLen = 0;
  psa_status_t status = psa_mac_compute(
      keyHandle.get(), PSA_ALG_HMAC(hashAlg),
      reinterpret_cast<const uint8_t*>(message), messageLength,
      reinterpret_cast<uint8_t*>(output), getDigestSize(), &macLen);

  if (status != PSA_SUCCESS) {
    throw std::runtime_error(
        fmt::format("Failed to compute HMAC, psa status: {}", (int)status));
  }
}
