#include "X509Bundle.h"

// No certificate-chain verification implemented for this platform -
// preserves the same behavior the single shared stub used to have for
// every platform before it was split per-platform (see
// docs/aprendizaje.md, "Seguridad: TLS, verificación de certificados y
// mbedTLS", section 5). See
// main/platform/esp/X509Bundle.cpp for a real implementation
// (esp_crt_bundle_attach()); crtCheckCertificate()/crtVerifyCallback()
// declared in X509Bundle.h are intentionally left undefined - nothing
// calls them directly, only this namespace's own init/attach/shouldVerify
// would have, and this stub doesn't need them.
namespace bell::X509Bundle {

void init(const uint8_t*, size_t) {}

void attach(mbedtls_ssl_config*) {}

bool shouldVerify() {
  return false;
}

}  // namespace bell::X509Bundle
