#pragma once

#include "bell/net/IpAddress.h"
#include "bell/net/POSIXSocket.h"

namespace bell::net {
/**
 * @brief UDP implementation of the net::Socket
 */
class UDPSocket : public POSIXSocket {
 public:
  // Delete copy constructor and copy assignment operator
  UDPSocket(const UDPSocket&) = delete;
  UDPSocket& operator=(const UDPSocket&) = delete;

  /**
   * @brief Default constructor for the UDPSocket class. Initializes the socket to INVALID_FD.
   */
  UDPSocket() = default;

  /**
   * @brief Destructor for the UDPSocket class. Closes the socket if it is open.
   */
  ~UDPSocket() override { close(); }

  /**
   * @brief Constructor that wraps an existing file descriptor.
   */
  explicit UDPSocket(int sockFd) noexcept { this->sockFd = sockFd; }

  /**
   * @brief Move constructor for the UDPSocket class
   */
  UDPSocket(UDPSocket&& sock) noexcept { this->sockFd = sock.takeFd(); }

  /**
   * @brief Receive data from the provided address
   *
   * This method receives data from the socket and stores it in the provided buffer.
   * The method blocks until data is available or an error occurs. The return value
   * indicates the number of bytes successfully read.
   *
   * @param buf Pointer to the buffer where the received data will be stored.
   * @param len The maximum number of bytes to read into the buffer.
   * @param address The address from which the data should be received.
   * @return The number of bytes successfully read. A return value of 0 may indicate
   * that the connection was closed, while a value less than len could indicate that
   * no more data is currently available.
   */
  bell::Result<size_t> recvfrom(std::byte* buf, size_t len,
                                const IpAddress& address);

  /**
   * @brief Send data to the provided address
   *
   * This method sends data from the provided buffer to the socket. The method
   * blocks until the data is sent or an error occurs. The return value indicates
   * the number of bytes successfully written.
   *
   * @param buf Pointer to the buffer containing the data to send.
   * @param len The number of bytes to write from the buffer.
   * @param address The address to which the data should be sent.
   * @return The number of bytes successfully written.
   */
  bell::Result<size_t> sendto(const std::byte* buf, size_t len,
                              const IpAddress& address);

  // POSIXSocket interface override
  int getSockType() override { return SOCK_DGRAM; }

 private:
  const char* LOG_TAG = "UDPSocket";
};
}  // namespace bell::net

namespace bell {
using UDPSocket = net::UDPSocket;
}
