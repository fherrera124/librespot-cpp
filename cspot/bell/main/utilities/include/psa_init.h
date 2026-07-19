#pragma once

#include <psa/crypto.h>

// mbedTLS 4.0 removed manual RNG context setup (mbedtls_ctr_drbg_context /
// mbedtls_entropy_context) - everything that needs randomness now draws
// from the PSA subsystem instead, which must be initialized once before
// first use. C++11 function-local statics are initialized exactly once,
// in a thread-safe way, the first time control passes through here.
inline void ensurePsaCryptoInit() {
  static const psa_status_t status = psa_crypto_init();
  (void)status;
}
