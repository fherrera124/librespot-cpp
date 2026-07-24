#include "bell/net/TLSSocket.h"
#include <netinet/tcp.h>

// Standard includes
#include <cerrno>
#include <stdexcept>
#include <system_error>

// MbedTLS includes
#include "mbedtls/error.h"

#include "bell/Logger.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "tl/expected.hpp"

// mbedTLS 4.0 removed the classic entropy/ctr_drbg RNG chain entirely - the
// SSL layer draws randomness from the PSA subsystem unconditionally instead
// (mbedtls_ssl_conf_rng() itself isn't even declared in 4.0's public API
// anymore - confirmed against the real ESP-IDF v6.0.1 toolchain's bundled
// mbedtls, exactly 4.0.0 - zero matches for the symbol anywhere in its
// include tree). Only needed on this branch; mbedTLS <4.0 keeps using the
// classic entropy+ctr_drbg chain below unchanged.
#if MBEDTLS_VERSION_NUMBER >= 0x04000000
#include "psa/crypto.h"
#endif

using namespace bell;

namespace {
// Personalization string used to seed the entropy context (mbedTLS <4.0 only)
const char* socketPers = "bell-tls";

#if MBEDTLS_VERSION_NUMBER >= 0x04000000
// PSA crypto must be initialized once before first use. A function-local
// static makes this thread-safe and exactly-once.
void ensurePsaCryptoInit() {
  static const psa_status_t status = psa_crypto_init();
  (void)status;
}
#endif

// Transform MbedTLS error codes to std::error_code, while attempting to map the errors to common errc values
std::error_code mbedtlsToCommonErrc(int mbedtlsErr) {
  switch (mbedtlsErr) {
    case MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY:
    case MBEDTLS_ERR_NET_CONN_RESET:
      return std::make_error_code(std::errc::connection_reset);
    case MBEDTLS_ERR_SSL_WANT_READ:
    case MBEDTLS_ERR_SSL_WANT_WRITE:
      return std::make_error_code(std::errc::operation_would_block);
    case MBEDTLS_ERR_NET_RECV_FAILED:
    case MBEDTLS_ERR_NET_SEND_FAILED:
      return std::make_error_code(std::errc::io_error);
    default:
      return net::make_tls_error_code(mbedtlsErr);
  }
}

// Maps the TCP socket result values to MbedTLS BIO result values
bell::Result<int> transformBioRes(bell::Result<size_t> res, bool reading) {
  if (res) {
    return {static_cast<int>(*res)};
  }

  if (res.error() == std::errc::broken_pipe ||
      res.error() == std::errc::connection_reset) {
    return tl::make_unexpected(
        net::make_tls_error_code(MBEDTLS_ERR_NET_CONN_RESET));
  }

  if (res.error() == std::errc::operation_would_block ||
      res.error() == std::errc::interrupted ||
      res.error() == std::errc::timed_out) {
    return reading ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_SSL_WANT_WRITE;
  }

  return reading ? MBEDTLS_ERR_NET_RECV_FAILED : MBEDTLS_ERR_NET_SEND_FAILED;
}
}  // namespace

net::TLSSocket::~TLSSocket() {
  close();

  // Free the MbedTLS structures
  mbedtls_ssl_free(&sslCtx);
  mbedtls_ssl_config_free(&sslConf);
#if MBEDTLS_VERSION_NUMBER < 0x04000000
  mbedtls_ctr_drbg_free(&ctrDrbgCtx);
  mbedtls_entropy_free(&entropyCtx);
#endif
}

net::TLSSocket::TLSSocket() {
  // Initialize the MbedTLS structures
  mbedtls_ssl_init(&sslCtx);
  mbedtls_ssl_config_init(&sslConf);

  // TODO: Attach a bundle here

#if MBEDTLS_VERSION_NUMBER < 0x04000000
  mbedtls_entropy_init(&entropyCtx);
  mbedtls_ctr_drbg_init(&ctrDrbgCtx);

  // Seed the ctr_drbg context
  int ret = mbedtls_ctr_drbg_seed(
      &ctrDrbgCtx, mbedtls_entropy_func, &entropyCtx,
      reinterpret_cast<const uint8_t*>(socketPers), std::strlen(socketPers));

  if (ret != 0) {
    auto err = make_tls_error_code(ret);
    throw std::system_error(err);
  }
#else
  ensurePsaCryptoInit();
#endif
}

std::error_code net::TLSSocket::lastError() const {
  return innerSocket.lastError();
}

bell::Result<> net::TLSSocket::connect(const std::string& host, uint16_t port,
                                       int timeoutMs) {
  auto res = innerSocket.connect(host, port, timeoutMs);
  if (!res) {
    BELL_LOG(error, LOG_TAG, "Failed to connect to {}: {}", host, res.error());
    return res;
  }

  auto setBlockingRes = setBlocking(timeoutMs > 0);
  if (!setBlockingRes) {
    return setBlockingRes;
  }

  int ret = mbedtls_ssl_config_defaults(&sslConf, MBEDTLS_SSL_IS_CLIENT,
                                        MBEDTLS_SSL_TRANSPORT_STREAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT);
  if (ret != 0) {
    return tl::make_unexpected(make_tls_error_code(ret));
  }
  // TODO: Bundle verification & TLS 1.3
  mbedtls_ssl_conf_authmode(&sslConf, MBEDTLS_SSL_VERIFY_NONE);
  mbedtls_ssl_conf_max_tls_version(&sslConf, MBEDTLS_SSL_VERSION_TLS1_2);

  // mbedTLS 4.0+ doesn't declare mbedtls_ssl_conf_rng() at all - the SSL
  // layer draws randomness from PSA unconditionally there instead (see
  // ensurePsaCryptoInit() above). mbedTLS <4.0 requires this explicitly
  // ("RNG function (mandatory)" per its own doc comment).
#if MBEDTLS_VERSION_NUMBER < 0x04000000
  mbedtls_ssl_conf_rng(&sslConf, mbedtls_ctr_drbg_random, &ctrDrbgCtx);
#endif
  ret = mbedtls_ssl_setup(&sslCtx, &sslConf);
  if (ret != 0) {
    return tl::make_unexpected(make_tls_error_code(ret));
  }

  ret = mbedtls_ssl_set_hostname(&sslCtx, host.c_str());
  if (ret != 0) {
    return tl::make_unexpected(make_tls_error_code(ret));
  }

  while ((ret = mbedtls_ssl_handshake(&sslCtx)) != 0) {
    if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
      BELL_LOG(error, LOG_TAG, "Failed to perform TLS handshake {}", ret);
      return tl::make_unexpected(make_tls_error_code(ret));
    }
  }

  return {};
}

void net::TLSSocket::setupBioCallbacks(bool blocking) {
  mbedtls_ssl_send_t* sendFunc = [](void* ctx, const unsigned char* buf,
                                    size_t len) {
    auto* socket = static_cast<TCPSocket*>(ctx);

    auto res = transformBioRes(
        socket->write(reinterpret_cast<const std::byte*>(buf), len), false);
    if (res) {
      return *res;
    }

    return res.error().value();
  };

  mbedtls_ssl_recv_t* recvFunc = nullptr;
  mbedtls_ssl_recv_timeout_t* recvTimeoutFunc = nullptr;

  if (blocking) {
    recvTimeoutFunc = [](void* ctx, unsigned char* buf, size_t len,
                         uint32_t timeoutMs) {
      auto* socket = static_cast<TCPSocket*>(ctx);

      auto timeoutRes = socket->setReceiveTimeout(timeoutMs);
      if (!timeoutRes) {
        return timeoutRes.error().value();
      }

      auto res = transformBioRes(
          socket->read(reinterpret_cast<std::byte*>(buf), len), true);
      if (res) {
        return *res;
      }

      return res.error().value();
    };
  } else {

    recvFunc = [](void* ctx, unsigned char* buf, size_t len) {
      auto* socket = static_cast<TCPSocket*>(ctx);

      auto res = transformBioRes(
          socket->read(reinterpret_cast<std::byte*>(buf), len), true);
      if (res) {
        return *res;
      }

      return res.error().value();
    };
  }

  mbedtls_ssl_set_bio(&sslCtx, &innerSocket, sendFunc, recvFunc,
                      recvTimeoutFunc);
}

bell::Result<> net::TLSSocket::setReceiveTimeout(int timeoutMs) {
  return innerSocket.setReceiveTimeout(timeoutMs);
};

bell::Result<> net::TLSSocket::setSendTimeout(int timeoutMs) {
  return innerSocket.setSendTimeout(timeoutMs);
};

bell::Result<int> net::TLSSocket::getReceiveTimeout() {
  return innerSocket.getReceiveTimeout();
};

bell::Result<int> net::TLSSocket::getSendTimeout() {
  return innerSocket.getSendTimeout();
};

bell::Result<> net::TLSSocket::setBlocking(bool blocking) {
  setupBioCallbacks(blocking);
  return innerSocket.setBlocking(blocking);
}

bell::Result<bool> net::TLSSocket::getBlocking() const {
  return innerSocket.getBlocking();
}

int net::TLSSocket::getFd() const {
  return innerSocket.getFd();
}

int net::TLSSocket::takeFd() {
  return innerSocket.takeFd();
}

bell::Result<size_t> net::TLSSocket::read(std::byte* buf, size_t len) {
  int res = mbedtls_ssl_read(&sslCtx, reinterpret_cast<uint8_t*>(buf), len);

  if (res < 0) {
    return tl::make_unexpected(mbedtlsToCommonErrc(res));
  }

  return static_cast<size_t>(res);
}

bell::Result<size_t> net::TLSSocket::write(const std::byte* buf, size_t len) {
  int res =
      mbedtls_ssl_write(&sslCtx, reinterpret_cast<const uint8_t*>(buf), len);

  if (res < 0) {
    return tl::make_unexpected(mbedtlsToCommonErrc(res));
  }

  return static_cast<size_t>(res);
}

bool net::TLSSocket::isValid() const {
  return innerSocket.isValid();
}

void net::TLSSocket::close() {
  if (innerSocket.isValid()) {
    mbedtls_ssl_close_notify(&sslCtx);
    innerSocket.close();
  }
}
