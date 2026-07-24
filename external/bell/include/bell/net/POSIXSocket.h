#pragma once

// Standard includes
#include <netinet/in.h>
#include <sys/socket.h>
#include <cstdint>
#include <system_error>

// Own includes
#include "bell/Result.h"
#include "bell/net/IpAddress.h"
#include "bell/net/Socket.h"

namespace bell::net {
/**
 * @brief Common socket implementation for UDP and TCP sockets, later extended by specific implementations.
 */
class POSIXSocket : public Socket {
 public:
  POSIXSocket() = default;

  /**
   * @brief Create a socket file descriptor with the specified family, type and protocol
   *
   * @param domain The address family (e.g., AF_INET for IPv4, AF_INET6 for IPv6).
   * @param protocol The protocol to be used (e.g., IPPROTO_TCP for TCP, IPPROTO_UDP for UDP).
   * @return error code indicating success or failure.
   */
  bell::Result<> createFd(int domain, int protocol = 0);

  /**
   * @brief Set a socket option with a templated value.
   *
   * This method wraps the setsockopt function to set various socket options,
   * inferring the value's size based on the type of optionValue.
   *
   * @param level The level at which the option is defined (e.g., SOL_SOCKET).
   * @param optionName The name of the option to be set (e.g., SO_REUSEADDR).
   * @param optionValue The value of the option to be set.
   */
  template <typename T>
  bell::Result<> setOption(int level, int optionName, const T& optionValue) {
    return setOptionImpl(level, optionName, &optionValue, sizeof(T));
  }

  /**
   * @brief Get a socket option with a templated value.
   *
   * This method wraps the getsockopt function to retrieve various socket options, inferring the value's size based on the type of optionValue.
   *
   * @param level The level at which the option is defined (e.g., SOL_SOCKET).
   * @param optionName The name of the option to be retrieved (e.g., SO_REUSEADDR).
   * @return bell::Result<T> containing the value of the option or an error code.
   */
  template <typename T>
  bell::Result<T> getOption(int level, int optionName) {
    T optionValue{};
    socklen_t optionLen = sizeof(T);

    auto res = getOptionImpl(level, optionName, &optionValue, optionLen);

    if (!res) {
      return tl::unexpected(res);
    }

    return optionValue;
  }

  /**
   * @brief Bind the socket to a specific address and port.
   *
   * @param address A string representation of the address to bind to (e.g., "127.0.0.1").
   * @param port The port number to bind to, or 0 for a random port.
   * @param reuseAddr If true, allows the socket to bind to an address that is already in use.
   *
   * @return bell::Result<int> resulting port number or an error code.
   */
  bell::Result<int> bind(const std::string& address, uint16_t port,
                         bool reuseAddr = true);

  /**
   * @brief Returns the last error code from the socket, using SO_ERROR.
   */
  std::error_code lastError() const;

  /**
   * @brief Returns the peer address of the socket.
   */
  bell::Result<IpAddress> getPeerName() const;

  // Socket interface overrides
  bell::Result<> setReceiveTimeout(int timeoutMs) override;
  bell::Result<> setSendTimeout(int timeoutMs) override;
  bell::Result<int> getReceiveTimeout() override;
  bell::Result<int> getSendTimeout() override;
  bell::Result<size_t> read(std::byte* buf, size_t len) override;
  bell::Result<size_t> write(const std::byte* buf, size_t len) override;
  bell::Result<> setBlocking(bool blocking) override;
  bell::Result<bool> getBlocking() const override;
  bool isValid() const override;
  void close() override;
  int takeFd() override;
  int getFd() const override;

  // To be implemented by derived classes
  virtual int getSockType() = 0;

  // Default value for invalid file descriptor
  static const int INVALID_FD = -1;

 private:
  const char* LOG_TAG = "POSIXSocket";

  bell::Result<> setOptionImpl(int level, int optionName,
                               const void* optionValue,
                               socklen_t optionLen) const;

  bell::Result<> getOptionImpl(int level, int optionName, void* optionValue,
                               socklen_t optionLen) const;

 protected:
  // File descriptor associated with the socket
  int sockFd = INVALID_FD;

  static std::error_code errorFromErrno() {
    return {errno, std::system_category()};
  }
};
}  // namespace bell::net

namespace bell {
using POSIXSocket = net::POSIXSocket;
}
