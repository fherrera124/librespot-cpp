#include "TLSSocket.h"

#include <netinet/in.h>   // for IPPROTO_TCP
#include <netinet/tcp.h>  // for TCP_NODELAY
#include <sys/socket.h>   // for setsockopt, select
#include <sys/time.h>     // for struct timeval (SO_SNDTIMEO)
#include <sys/select.h>   // for fd_set, select
#include <mbedtls/entropy.h>      // for MBEDTLS_ERR_ENTROPY_SOURCE_FAILED
#include <mbedtls/net_sockets.h>  // for mbedtls_net_connect, mbedtls_net_free
#include <mbedtls/ssl.h>          // for mbedtls_ssl_conf_authmode, mbedtls_...
#include <cstddef>                // for NULL
#include <stdexcept>              // for runtime_error

#include "BellLogger.h"  // for AbstractLogger, BELL_LOG
#include "X509Bundle.h"  // for shouldVerify, attach
#include "psa_init.h"

namespace {
// mbedtls_ssl_conf_rng()'s callback signature - adapts the PSA random
// generator this codebase already uses elsewhere (Crypto.cpp's
// generateVectorWithRandomData()) to it.
int psaRandomForMbedtls(void*, unsigned char* output, size_t len) {
  return psa_generate_random(output, len) == PSA_SUCCESS
             ? 0
             : MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
}
}  // namespace

/**
 * Platform TLSSocket implementation for the mbedtls, ported for mbedTLS 4.0.
 * See docs/spotify_component_analysis.md, section 2, and TLSSocket.h.
 */
bell::TLSSocket::TLSSocket() {
  this->isClosed = false;
  ensurePsaCryptoInit();
  mbedtls_net_init(&server_fd);
  mbedtls_ssl_init(&ssl);
  mbedtls_ssl_config_init(&conf);

  if (bell::X509Bundle::shouldVerify()) {
    bell::X509Bundle::attach(&conf);
  }
}

void bell::TLSSocket::open(const std::string& hostUrl, uint16_t port) {
  int ret;
  // Connect failure used to fall through into TLS handshake setup on a
  // dead socket instead of erroring here. See F29.
  if ((ret = mbedtls_net_connect(&server_fd, hostUrl.c_str(),
                                 std::to_string(port).c_str(),
                                 MBEDTLS_NET_PROTO_TCP)) != 0) {
    BELL_LOG(error, "http_tls", "failed! connect returned %d\n", ret);
    throw std::runtime_error("mbedtls_net_connect failed");
  }

  // Disable Nagle - every request/response exchange on this socket (TLS
  // handshake, Login5, spclient, CDN) otherwise pays up to ~200ms per
  // round-trip waiting to coalesce. See dealer_websocket_migration.md §20.
  int nodelay = 1;
  setsockopt(server_fd.fd, IPPROTO_TCP, TCP_NODELAY, &nodelay,
            sizeof(nodelay));

  // Write-side timeout, mirroring the read-side one (F58) - without it
  // mbedtls_ssl_write() can hang the calling task forever on a stalled
  // peer. See dealer_websocket_migration.md §22.
  struct timeval sndTimeout;
  sndTimeout.tv_sec = 15;
  sndTimeout.tv_usec = 0;
  setsockopt(server_fd.fd, SOL_SOCKET, SO_SNDTIMEO, &sndTimeout,
            sizeof(sndTimeout));

  // OS-level TCP keepalive, matching go-librespot's 30s default for dead-peer
  // detection independent of the app-level ping/pong watchdogs (§44, AP-level
  // in MercurySession.cpp). See dealer_websocket_migration.md §49.
  int keepalive = 1;
  setsockopt(server_fd.fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive,
            sizeof(keepalive));
  int keepIdle = 30;
  setsockopt(server_fd.fd, IPPROTO_TCP, TCP_KEEPIDLE, &keepIdle,
            sizeof(keepIdle));
  int keepInterval = 10;
  setsockopt(server_fd.fd, IPPROTO_TCP, TCP_KEEPINTVL, &keepInterval,
            sizeof(keepInterval));
  int keepCount = 3;
  setsockopt(server_fd.fd, IPPROTO_TCP, TCP_KEEPCNT, &keepCount,
            sizeof(keepCount));

  if ((ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
                                         MBEDTLS_SSL_TRANSPORT_STREAM,
                                         MBEDTLS_SSL_PRESET_DEFAULT)) != 0) {

    BELL_LOG(error, "http_tls", "failed! config returned %d\n", ret);
    throw std::runtime_error("mbedtls_ssl_config_defaults failed");
  }

  // Only verify if the X509 bundle is present
  if (bell::X509Bundle::shouldVerify()) {
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
  } else {
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
  }

  // mandatory per mbedtls_ssl_conf_rng()'s own doc comment ("RNG function
  // (mandatory)") - without it, conf.f_rng stays NULL and
  // mbedtls_ssl_handshake() fails immediately with
  // MBEDTLS_ERR_SSL_BAD_INPUT_DATA before any bytes go on the wire (real,
  // pre-existing bug: this call used to be missing entirely, on the theory
  // that PSA's RNG applies unconditionally - true for the entropy/DRBG
  // subsystem itself, but the SSL layer's own f_rng callback is a separate,
  // still-mandatory field mbedTLS never fills in on its own). Reuses the
  // same PSA generator Crypto.cpp's generateVectorWithRandomData() does.
  mbedtls_ssl_conf_rng(&conf, psaRandomForMbedtls, nullptr);

  // Without this, mbedtls_ssl_read() can block forever on a connection an
  // intermediate NAT/carrier silently dropped without a FIN/RST. Default is
  // F58's 15000ms; overridable per-instance via setReadTimeout() before
  // open() - Dealer WebSocket needs a longer value (§22).
  mbedtls_ssl_conf_read_timeout(&conf, readTimeoutMs);

  if ((ret = mbedtls_ssl_setup(&ssl, &conf)) != 0) {
    throw std::runtime_error("mbedtls_ssl_setup failed");
  }

  if ((ret = mbedtls_ssl_set_hostname(&ssl, hostUrl.c_str())) != 0) {
    throw std::runtime_error("mbedtls_ssl_set_hostname failed");
  }
  // f_recv left NULL: passing f_recv_timeout makes mbedTLS always use it,
  // enforcing readTimeoutMs via mbedtls_net_recv_timeout() instead of
  // blocking indefinitely.
  mbedtls_ssl_set_bio(&ssl, &server_fd, mbedtls_net_send, NULL,
                      mbedtls_net_recv_timeout);

  while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
    if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
      BELL_LOG(error, "http_tls", "failed! config returned %d\n", ret);
      throw std::runtime_error("mbedtls_ssl_handshake error");
    }
  }
}

size_t bell::TLSSocket::read(uint8_t* buf, size_t len) {
  int ret;
  do {
    ret = mbedtls_ssl_read(&ssl, buf, len);
    
    // Break out on hard network errors instead of looping forever: on
    // ECONNRESET mid-ticket, ssl.state stays parked on NEW_SESSION_TICKET
    // and would otherwise spin and hang the Task Watchdog.
    if (ret < 0 &&
        ret != MBEDTLS_ERR_SSL_TIMEOUT && 
        ret != MBEDTLS_ERR_SSL_WANT_READ && 
        ret != MBEDTLS_ERR_SSL_WANT_WRITE && 
        ret != MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) {
        break;
    }

    if (ret == MBEDTLS_ERR_SSL_TIMEOUT) {
      break;
    }
  } while (ret == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET ||
           ssl.MBEDTLS_PRIVATE(state) == MBEDTLS_SSL_TLS1_3_NEW_SESSION_TICKET);

  // Recorded, not returned directly, so callers checking "0 bytes" keep
  // behaving as before; only WebSocketTransport.cpp reads this flag (F30).
  timedOut = (ret == MBEDTLS_ERR_SSL_TIMEOUT);
  return ret < 0 ? 0 : static_cast<size_t>(ret);
}

size_t bell::TLSSocket::write(uint8_t* buf, size_t len) {
  int ret;
  // Retry on WANT_WRITE/WANT_READ (e.g. TLS renegotiation) instead of
  // returning 0, which callers would read as a dead connection.
  do {
    ret = mbedtls_ssl_write(&ssl, buf, len);
  } while (ret == MBEDTLS_ERR_SSL_WANT_WRITE || ret == MBEDTLS_ERR_SSL_WANT_READ);
  
  return ret < 0 ? 0 : static_cast<size_t>(ret);
}

size_t bell::TLSSocket::poll() {
  // mbedtls_ssl_get_bytes_avail() only sees already-decrypted bytes; also
  // check the raw socket for encrypted payload still waiting in LwIP.
  size_t avail = mbedtls_ssl_get_bytes_avail(&ssl);
  if (avail > 0) {
    return avail;
  }

  if (isClosed || server_fd.fd < 0) {
      return 0;
  }

  fd_set readset;
  FD_ZERO(&readset);
  FD_SET(server_fd.fd, &readset);

  struct timeval tv = {0, 0}; // Non-blocking check
  int res = select(server_fd.fd + 1, &readset, NULL, NULL, &tv);
  
  // Return 1 (data available at socket level) or 0
  return (res > 0 && FD_ISSET(server_fd.fd, &readset)) ? 1 : 0;
}

bool bell::TLSSocket::isOpen() {
  return !isClosed;
}

void bell::TLSSocket::setReadTimeout(uint32_t ms) {
  readTimeoutMs = ms;
  if (!isClosed) {
    // Already past open(): apply immediately. Only the reading thread is
    // expected to call this (see header), so no concurrent mbedtls_ssl_xxx
    // call can race with it.
    mbedtls_ssl_conf_read_timeout(&conf, ms);
  }
}

void bell::TLSSocket::close() {
  if (!isClosed) {
    // Best-effort clean TLS shutdown before tearing down the raw socket;
    // failure ignored since we're closing either way. See §22.
    mbedtls_ssl_close_notify(&ssl);

    mbedtls_net_free(&server_fd);
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    this->isClosed = true;
  }
}