#include "bell/net/UDPSocket.h"

#include "bell/Logger.h"
#include "bell/net/IpAddress.h"
#include "tl/expected.hpp"

// Platform specific socket includes
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include "win32shim.h"
#else
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef __sun
#include <sys/filio.h>
#endif
#endif

#include <fcntl.h>

using namespace bell::net;

tl::expected<size_t, std::error_code> UDPSocket::recvfrom(
    std::byte* buf, size_t len, const IpAddress& address) {
  if (!isValid()) {
    return make_unexpected_errc<size_t>(std::errc::bad_file_descriptor);
  }

  socklen_t addressLen = address.getSockAddrLen();

  // Perform the actual read operation
  // Using const_cast to remove the const qualifier from the address, as recvfrom expects a non-const
  // This is quite ugly, but recvfrom is a C API and we can't change it.
  ssize_t res = ::recvfrom(
      sockFd, buf, len, 0,
      const_cast<sockaddr*>(address.getSockAddrPtrConst()),  // NOLINT
      &addressLen);
  if (res < 0) {
    return tl::make_unexpected(errorFromErrno());
  }

  return static_cast<size_t>(res);
}

bell::Result<size_t> UDPSocket::sendto(const std::byte* buf, size_t len,
                                       const IpAddress& address) {
  if (!isValid()) {
    return make_unexpected_errc<size_t>(std::errc::bad_file_descriptor);
  }

  // Using const_cast to remove the const qualifier from the address, as sendto expects a non-const
  // This is quite ugly, but recvfrom is a C API and we can't change it.
  ssize_t res =
      ::sendto(sockFd, buf, len, 0,
               const_cast<sockaddr*>(address.getSockAddrPtrConst()),  // NOLINT
               address.getSockAddrLen());
  if (res < 0) {
    return tl::make_unexpected(errorFromErrno());
  }

  return static_cast<size_t>(res);
}
