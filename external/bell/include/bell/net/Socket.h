#pragma once

// Standard includes
#include <cstdint>

// Own includes
#include "bell/Result.h"

namespace bell::net {
/**
 * @brief Base pure socket class to be implemented by different socket types.
 *
 * This class provides a standard interface for socket operations, which can be
 * extended by different socket types (e.g., TCP, UDP). It defines essential
 * methods for opening, closing, reading from, writing to, and polling a socket,
 * as well as wrapping existing file descriptors.
 */
class Socket {
 public:
  Socket() = default;  ///< Default constructor.
  virtual ~Socket() =
      default;  ///< Virtual destructor for proper cleanup in derived classes.

  // Non-copyable
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;

  // Movable
  Socket(Socket&&) noexcept = default;
  Socket& operator=(Socket&&) noexcept = default;

  /**
   * @brief Resolve the provided host and port, and attempt to create a socket connected there.
   *
   * This method resolves the hostname and attempts to connect to the specified port. It will also set the default timeout for the socket.
   *
   *
   * @param host String containing a hostname or IP address to connect to.
   * @param port The port number to connect to on the specified host.
   * @param timeout The maximum time to wait for the connection to be established, in milliseconds
   */
  bell::Result<> connect(const std::string& host, uint16_t port,
                         int timeoutMs = 0);

  /**
   * @brief Set the blocking mode of the socket.
   *
   * @param blocking True to set the socket to blocking mode, false for non-blocking.
   */
  virtual bell::Result<> setBlocking(bool blocking) = 0;

  /**
   * @brief Get the blocking mode of the socket.
   *
   * @return true if the socket is in blocking mode, false otherwise.
   */
  virtual bell::Result<bool> getBlocking() const = 0;

  /**
   * @brief Set the read timeout for socket operations.
   *
   * @param timeoutMs Timeout in milliseconds. A value of 0 indicates a non-blocking operation.
   */
  virtual bell::Result<> setReceiveTimeout(int timeoutMs) = 0;

  /**
   * @brief Sets the send timeout for socket operations
   *
   * @param timeoutMs Timeout in milliseconds. A value of 0 indicates a non-blocking operation.
   */
  virtual bell::Result<> setSendTimeout(int timeoutMs) = 0;

  /**
   * @brief Get the read timeout for socket operations.
   *
   * @return The read timeout in milliseconds. A value of 0 indicates a non-blocking operation.
   */
  virtual bell::Result<int> getReceiveTimeout() = 0;

  /**
   * @brief Get the send timeout for socket operations.
   *
   * @return The send timeout in milliseconds. A value of 0 indicates a non-blocking operation.
   */
  virtual bell::Result<int> getSendTimeout() = 0;

  /**
   * @brief Write data to the socket.
   *
   * This method sends data from the provided buffer to the socket. The method
   * blocks until the data is sent or an error occurs. The return value indicates
   * the number of bytes successfully written.
   *
   * @param buf Pointer to the buffer containing the data to send.
   * @param len The number of bytes to write from the buffer.
   * @return The number of bytes successfully written.
   */
  virtual bell::Result<size_t> write(const std::byte* buf, size_t len) = 0;

  /**
   * @brief Read data from the socket.
   *
   * This method receives data from the socket and stores it in the provided buffer.
   * The method blocks until data is available or an error occurs. The return value
   * indicates the number of bytes successfully read.
   *
   * @param buf Pointer to the buffer where the received data will be stored.
   * @param len The maximum number of bytes to read into the buffer.
   * @return The number of bytes successfully read. A return value of 0 may indicate
   * that the connection was closed, while a value less than len could indicate that
   * no more data is currently available.
   */
  virtual bell::Result<size_t> read(std::byte* buf, size_t len) = 0;

  /**
   * @brief Check if the socket is open
   *
   * @return True if the socket is open, false otherwise.
   */
  virtual bool isValid() const = 0;

  /**
   * @brief Close the socket.
   *
   * This method closes the socket and releases any resources associated with it.
   * After calling this method, the socket is no longer usable until reopened.
   */
  virtual void close() = 0;

  /**
   * @brief Get the file descriptor associated with the socket.
   *
   * @return The file descriptor associated with the socket.
   */
  virtual int getFd() const = 0;

  /**
   * @brief Take ownership of the file descriptor associated with the socket.
   *
   * This method transfers ownership of the file descriptor to the caller, making
   * the socket invalid for further operations.
   *
   * @return The file descriptor associated with the socket.
   */
  virtual int takeFd() = 0;
};
}  // namespace bell::net

namespace bell {
using Socket = net::Socket;
}
