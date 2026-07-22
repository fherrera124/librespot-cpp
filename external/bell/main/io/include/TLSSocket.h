#ifndef BELL_TLS_SOCKET_H
#define BELL_TLS_SOCKET_H

#include <stdint.h>  // for uint8_t, uint16_t

#include "BellSocket.h"  // for Socket
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#endif
#include <stdlib.h>  // for size_t
#include <string>    // for string

#include "mbedtls/net_sockets.h"  // for mbedtls_net_context
#include "mbedtls/ssl.h"          // for mbedtls_ssl_config, mbedtls_ssl_con...

// Ported for mbedTLS 4.0 (see docs/spotify_component_analysis.md, section
// 2). Only change from upstream cspot/bell: no more manual entropy/DRBG
// context members - mbedTLS 4.0 removed mbedtls_ctr_drbg_context/
// mbedtls_entropy_context entirely, so randomness for those (DH key
// generation, etc.) now comes from the PSA subsystem instead. That does
// NOT extend to mbedtls_ssl_config's own f_rng field, though -
// mbedtls_ssl_conf_rng() is still mandatory (TLSSocket.cpp calls it with
// a small PSA-backed adapter) even on mbedtls 3.x, which still requires
// it explicitly; skipping it entirely made every handshake fail
// immediately with MBEDTLS_ERR_SSL_BAD_INPUT_DATA.
namespace bell {
class TLSSocket : public bell::Socket {
 private:
  mbedtls_net_context server_fd;
  mbedtls_ssl_context ssl;
  mbedtls_ssl_config conf;

  bool isClosed = true;

  // Default matches the previous hardcoded F58 value (15000ms) - every
  // caller that never calls setReadTimeout() keeps identical behavior.
  // WebSocketTransport (WebSocketTransport.cpp) is the one caller that
  // overrides this, since a Dealer connection is expected to sit idle for
  // 30s+ between server pushes - reusing the HTTP-tuned 15s value there
  // would misdetect routine idle as a dead connection. See
  // docs/dealer_websocket_migration.md §22.
  uint32_t readTimeoutMs = 15000;

  // Set by the last read() call: true only when mbedtls_ssl_read() returned
  // MBEDTLS_ERR_SSL_TIMEOUT specifically (no data within readTimeoutMs, the
  // connection may still be perfectly fine) - false for every other outcome
  // (a genuine byte count, or a real fatal error). Existing callers that
  // never check this keep working exactly as before (any non-positive read
  // is still just "0 bytes" to them); WebSocketTransport.cpp uses it to
  // avoid tearing down the connection on a routine idle poll while still
  // reacting immediately to an actual failure (RST, etc.) via the false
  // case. See docs/dealer_websocket_migration.md §22.
  bool timedOut = false;

 public:
  TLSSocket();
  ~TLSSocket() { close(); };

  void open(const std::string& host, uint16_t port);

  // Must be called before open() to affect the handshake/connect phase, or
  // any time after open() to change the timeout used by subsequent read()
  // calls (applied immediately - safe to call from the same thread that
  // also does the reading, which is the only supported usage pattern here;
  // see WebSocketTransport.cpp's single-task design and
  // docs/dealer_websocket_migration.md §22 for why concurrent read/write
  // from two threads on one mbedtls_ssl_context is deliberately avoided).
  void setReadTimeout(uint32_t ms);
  bool lastReadTimedOut() const { return timedOut; }

  size_t read(uint8_t* buf, size_t len);
  size_t write(uint8_t* buf, size_t len);
  size_t poll();
  bool isOpen();

  void close();
  int getFd() { return server_fd.fd; }
};

}  // namespace bell

#endif
