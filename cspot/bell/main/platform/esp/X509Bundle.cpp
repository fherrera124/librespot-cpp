#include "X509Bundle.h"

#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "sdkconfig.h"

// Real certificate-chain verification for ESP-IDF, replacing the
// no-verify stub every platform shared before. Delegates entirely to
// esp_crt_bundle_attach() - ESP-IDF's own actively-maintained trusted
// root bundle (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE) - instead of
// hand-porting upstream cspot/bell's crtCheckCertificate()/
// crtVerifyCallback() to PSA (see docs/aprendizaje.md, "Seguridad: TLS,
// verificación de certificados y mbedTLS", section 5, option B:
// security-critical, cert-chain-adjacent code is exactly the
// kind of thing better delegated to a maintained implementation than
// hand-rolled). Neither of those two functions is called from anywhere
// outside X509Bundle.h's own declaration (verified by grep), so they're
// intentionally left undefined here, same as the stub they replace.
//
// Whether verification is actually turned on is a build-time choice
// (CONFIG_CSPOT_TLS_VERIFY_CERTIFICATES, default y - fail-safe: on
// unless a developer deliberately opts out via `idf.py menuconfig`,
// unlike the previous hardcoded `false`) rather than something buried in
// this file.
static const char* TAG = "X509Bundle";

namespace bell::X509Bundle {

void init(const uint8_t*, size_t) {
  // No-op: esp_crt_bundle_attach() below loads ESP-IDF's own
  // CONFIG_MBEDTLS_CERTIFICATE_BUNDLE bundle directly - it doesn't take
  // an externally-supplied bundle buffer the way upstream's original
  // init(x509_bundle, bundle_size) did.
}

void attach(mbedtls_ssl_config* conf) {
  esp_err_t err = esp_crt_bundle_attach(conf);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_crt_bundle_attach failed: %s", esp_err_to_name(err));
  }
}

bool shouldVerify() {
#ifdef CONFIG_CSPOT_TLS_VERIFY_CERTIFICATES
  return true;
#else
  return false;
#endif
}

}  // namespace bell::X509Bundle
