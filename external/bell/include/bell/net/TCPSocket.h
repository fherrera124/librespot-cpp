#pragma once

// Bell includes
#include "bell/net/POSIXSocket.h"

namespace bell::net {
/**
 * @brief TCP implementation of the bell::Socket
 */
class TCPSocket : public POSIXSocket {
 public:
  // Delete copy constructor and copy assignment operator
  TCPSocket(const TCPSocket&) = delete;
  TCPSocket& operator=(const TCPSocket&) = delete;

  /**
  * @brief Default constructor for the TCPSocket class. Initializes the socket to INVALID_FD.
  */
  TCPSocket() = default;

  /**
   * @brief Destructor for the TCPSocket class. Closes the socket if it is open.
   */
  ~TCPSocket() override { close(); }

  /**
   * @brief Constructor that wraps an existing file descriptor.
   */
  explicit TCPSocket(int sockFd) noexcept { this->sockFd = sockFd; }

  /**
   * @brief Move constructor for the TCPSocket class
   */
  TCPSocket(TCPSocket&& sock) noexcept { this->sockFd = sock.takeFd(); }

  // Define move assignment operator
  TCPSocket& operator=(TCPSocket&& sock) noexcept {
    if (this != &sock) {
      this->sockFd = sock.takeFd();
    }
    return *this;
  }

  /**
   * @brief Resolve the provided host and port, and attempt to create a socket connected there.
   *
   * This method resolves the hostname and attempts to connect to the specified port. It will also set the default timeout for the socket.
   *
   * @param host String containing a hostname or IP address to connect to.
   * @param port The port number to connect to on the specified host.
   * @param timeout The maximum time to wait for the connection to be established, in milliseconds
   */
  bell::Result<> connect(const std::string& host, uint16_t port,
                         int timeoutMs = 0);

  /**
   * @brief Listen for incoming connections on the socket.
   *
   * @param backlog The maximum number of pending connections to allow.
   */
  bell::Result<> listen(int backlog = 5);

  /**
   * @brief Accept an incoming connection on the socket.
   *
   * @return Accepted socket, or an error if the accept operation fails.
   */
  bell::Result<TCPSocket> accept();

  // POSIXSocket interface override
  int getSockType() override { return SOCK_STREAM; }

 private:
  const char* LOG_TAG = "TCPSocket";
};
}  // namespace bell::net

namespace bell {
using TCPSocket = net::TCPSocket;
}
