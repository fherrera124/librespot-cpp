#pragma once

// Standard includes
#include <netinet/in.h>
#include <array>
#include <cstdint>

// MbedTLS includes
#include "bell/net/TCPSocket.h"
#include "mbedtls/build_info.h"  // for MBEDTLS_VERSION_NUMBER, checked below
#if MBEDTLS_VERSION_NUMBER < 0x04000000
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#endif
#include "mbedtls/error.h"
#include "mbedtls/ssl.h"

#include "bell/Result.h"
#include "bell/net/Socket.h"

namespace internal {
class tls_errc_category : public std::error_category {
 public:
  const char* name() const noexcept override { return "MbedTLS"; }
  std::string message(int ev) const override {
    std::array<char, 256> errBuf;
    mbedtls_strerror(ev, errBuf.data(), sizeof(errBuf));
    return {errBuf.data()};
  }
};
}  // namespace internal

namespace bell::net {

// Define a custom error category for MbedTLS errors
inline const internal::tls_errc_category& tls_errc_category() {
  static internal::tls_errc_category c;
  return c;
}

// Make a custom error code for MbedTLS errors
inline ::std::error_code make_tls_error_code(int err) {
  return {err, tls_errc_category()};
}

// TLS Error type
class tls_error : public ::std::system_error {
 public:
  tls_error(int err) : system_error{make_tls_error_code(err)} {}
};

/**
 * @brief TLSSocket implementation of the bell::Socket, using MbedTLS.
 */
class TLSSocket : public Socket {
 public:
  // Delete copy constructor and copy assignment operator
  TLSSocket(const TLSSocket&) = delete;
  TLSSocket& operator=(const TLSSocket&) = delete;

  // implement move constructor and move assignment operator
  TLSSocket(TLSSocket&& sock) noexcept {
    this->innerSocket = std::move(sock.innerSocket);
    this->sslCtx = sock.sslCtx;
    this->sslConf = sock.sslConf;
#if MBEDTLS_VERSION_NUMBER < 0x04000000
    this->ctrDrbgCtx = sock.ctrDrbgCtx;
    this->entropyCtx = sock.entropyCtx;
#endif
  }

  TLSSocket& operator=(TLSSocket&& sock) noexcept {
    if (this != &sock) {
      this->innerSocket = std::move(sock.innerSocket);
      this->sslCtx = sock.sslCtx;
      this->sslConf = sock.sslConf;
#if MBEDTLS_VERSION_NUMBER < 0x04000000
      this->ctrDrbgCtx = sock.ctrDrbgCtx;
      this->entropyCtx = sock.entropyCtx;
#endif
    }
    return *this;
  }

  TLSSocket();
  ~TLSSocket() override;

  /**
   * @brief Return the last error that occurred on the socket.
   *
   * @return std::error_code error code
   */
  std::error_code lastError() const;

  /**
   * @brief Resolve the provided host and port, and attempt to create a socket connected there.
   *
   * This method resolves the hostname and attempts to connect to the specified port. It will also set the default timeout for the socket.
   *
   * @param host String containing a hostname or IP address to connect to.
   * @param port The port number to connect to on the specified host.
   * @param timeout The maximum time to wait for the connection to be established, in milliseconds. This parameter is ignored, if the socket is set to a blocking mode.
   */
  bell::Result<> connect(const std::string& host, uint16_t port,
                         int timeoutMs = 0);

  // Socket interface overrides
  bell::Result<> setSendTimeout(int timeoutMs) override;
  bell::Result<> setReceiveTimeout(int timeoutMs) override;
  bell::Result<int> getSendTimeout() override;
  bell::Result<int> getReceiveTimeout() override;
  bell::Result<size_t> read(std::byte* buf, size_t len) override;
  bell::Result<size_t> write(const std::byte* buf, size_t len) override;
  bell::Result<> setBlocking(bool blocking) override;
  bell::Result<bool> getBlocking() const override;
  bool isValid() const override;
  void close() override;
  int getFd() const override;
  int takeFd() override;

 private:
  const char* LOG_TAG = "TLSSocket";

  // MbedTLS structures. mbedTLS 4.0 removed manual RNG context setup
  // (mbedtls_ctr_drbg_context/mbedtls_entropy_context) entirely - the SSL
  // layer draws randomness from PSA unconditionally there instead. See
  // TLSSocket.cpp.
#if MBEDTLS_VERSION_NUMBER < 0x04000000
  mbedtls_entropy_context entropyCtx{};
  mbedtls_ctr_drbg_context ctrDrbgCtx{};
#endif
  mbedtls_ssl_context sslCtx{};
  mbedtls_ssl_config sslConf{};

  // Hooks for MbedTLS bio functions, depending on the blocking mode
  void setupBioCallbacks(bool blocking);

 protected:
  bell::TCPSocket innerSocket;  // Inner socket for TLS connection
};
}  // namespace bell::net

namespace bell {
using TLSSocket = net::TLSSocket;
}
