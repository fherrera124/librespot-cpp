#include "bell/net/POSIXSocket.h"

#include <fmt/format.h>
#include <system_error>
#include "bell/Logger.h"
#include "bell/Result.h"
#include "bell/net/IpAddress.h"
#include "bell/utils/Utils.h"
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

bell::Result<> POSIXSocket::setBlocking(bool blocking) {
  if (isValid()) {
#ifdef _WIN32
    unsigned long mode = blocking ? 0 : 1;
    if (ioctlsocket(sockFd, FIONBIO, &mode) != 0) {
      return errorFromErrno();
    }
#else
    int flags = fcntl(sockFd, F_GETFL, 0);
    if (flags < 0) {
      return tl::make_unexpected(errorFromErrno());
    }
    flags = blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK);
    if (fcntl(sockFd, F_SETFL, flags) != 0) {
      return tl::make_unexpected(errorFromErrno());
    }
#endif
  }

  return {};
}

std::error_code POSIXSocket::lastError() const {
  int sockErr;
  socklen_t sockErrLen = sizeof(sockErr);
  if (getsockopt(sockFd, SOL_SOCKET, SO_ERROR, &sockErr, &sockErrLen) < 0 ||
      sockErr != 0) {
    return {sockErr, std::system_category()};
  }

  return std::error_code();
}

bell::Result<size_t> POSIXSocket::read(std::byte* buf, size_t len) {
  if (!isValid()) {
    return tl::make_unexpected(
        std::make_error_code(std::errc::invalid_argument));
  }

  // Perform the actual read operation
  ssize_t res = ::recv(sockFd, buf, len, 0);
  if (res < 0) {
    return tl::make_unexpected(errorFromErrno());
  }

  return static_cast<size_t>(res);
}

bell::Result<size_t> POSIXSocket::write(const std::byte* buf, size_t len) {
  if (!isValid()) {
    return tl::make_unexpected(
        std::make_error_code(std::errc::invalid_argument));
  }

  // Perform the actual write operation
  ssize_t res = ::send(sockFd, buf, len, MSG_NOSIGNAL);
  if (res < 0) {
    return tl::make_unexpected(errorFromErrno());
  }

  return static_cast<size_t>(res);
}

bell::Result<> POSIXSocket::createFd(int domain, int protocol) {
  this->sockFd = socket(domain, getSockType(), protocol);
  if (sockFd < 0) {
    return tl::make_unexpected(errorFromErrno());
  }

  return {};
}

int POSIXSocket::getFd() const {
  return sockFd;
}

int POSIXSocket::takeFd() {
  int fd = sockFd;
  sockFd = INVALID_FD;
  return fd;
}

bool POSIXSocket::isValid() const {
  return sockFd != INVALID_FD;
}

void POSIXSocket::close() {
  if (isValid()) {
#ifdef _WIN32
    closesocket(sockFd);
#else
    ::close(sockFd);
#endif
    sockFd = INVALID_FD;
  }
}

bell::Result<int> POSIXSocket::bind(const std::string& address, uint16_t port,
                                    bool reuseAddr) {
  auto resolveRes = IpAddress::resolveDomain(address, getSockType());

  if (!resolveRes) {
    return tl::make_unexpected(resolveRes.error());
  }

  resolveRes->setPort(port);

  auto fdRes = createFd(resolveRes->getFamily(), IPPROTO_IP);

  if (!fdRes) {
    return tl::make_unexpected(fdRes.error());
  }

  if (reuseAddr) {
    auto optionRes = setOption(SOL_SOCKET, SO_REUSEADDR, 1);

    if (!optionRes) {
      return tl::make_unexpected(optionRes.error());
    }
  }

  if (::bind(sockFd, resolveRes->getSockAddrPtrConst(),
             resolveRes->getSockAddrLen()) != 0) {
    close();
    return tl::make_unexpected(errorFromErrno());
  }

  socklen_t servSockLen = resolveRes->getSockAddrLen();

  // Retrieve assigned port
  if (getsockname(sockFd, resolveRes->getSockAddrPtr(), &servSockLen) != 0) {
    close();
    return tl::make_unexpected(errorFromErrno());
  }

  if (resolveRes->getPort().has_value()) {
    return *resolveRes->getPort();
  }

  return make_unexpected_errc<int>(std::errc::invalid_argument);
}

bell::Result<> POSIXSocket::setOptionImpl(int level, int optionName,
                                          const void* optionValue,
                                          socklen_t optionLen) const {
  if (!isValid()) {
    return make_unexpected_errc(std::errc::bad_file_descriptor);
  }

  if (setsockopt(getFd(), level, optionName, optionValue, optionLen) == -1) {
    return tl::make_unexpected(errorFromErrno());
  }

  return {};
}

bell::Result<IpAddress> POSIXSocket::getPeerName() const {
  if (!isValid()) {
    return make_unexpected_errc<IpAddress>(std::errc::bad_file_descriptor);
  }

  struct sockaddr addr {};
  socklen_t addrLen = sizeof(addr);

  if (getpeername(getFd(), &addr, &addrLen) == -1) {
    return tl::make_unexpected(errorFromErrno());
  }

  return IpAddress(&addr);
}

bell::Result<> POSIXSocket::getOptionImpl(int level, int optionName,
                                          void* optionValue,
                                          socklen_t optionLen) const {
  if (!isValid()) {
    return make_unexpected_errc(std::errc::bad_file_descriptor);
  }

  if (getsockopt(getFd(), level, optionName, optionValue, &optionLen) == -1) {
    return tl::make_unexpected(errorFromErrno());
  }

  return {};
}

bell::Result<> POSIXSocket::setReceiveTimeout(int timeoutMs) {
  auto timeVal = bell::utils::millisecondsToTimeval(timeoutMs);
  return setOption(SOL_SOCKET, SO_RCVTIMEO, timeVal);
}

bell::Result<> POSIXSocket::setSendTimeout(int timeoutMs) {
  auto timeVal = bell::utils::millisecondsToTimeval(timeoutMs);
  return setOption(SOL_SOCKET, SO_SNDTIMEO, timeVal);
}

bell::Result<int> POSIXSocket::getReceiveTimeout() {
  struct timeval timeVal {};

  return getOptionImpl(SOL_SOCKET, SO_RCVTIMEO, &timeVal, sizeof(timeVal))
      .transform([&timeVal]() {
        return static_cast<int>(utils::timevalToMilliseconds(timeVal));
      });
}

bell::Result<int> POSIXSocket::getSendTimeout() {
  struct timeval timeVal {};
  return getOptionImpl(SOL_SOCKET, SO_SNDTIMEO, &timeVal, sizeof(timeVal))
      .transform([&timeVal]() {
        return static_cast<int>(utils::timevalToMilliseconds(timeVal));
      });
}

bell::Result<bool> POSIXSocket::getBlocking() const {
  if (!isValid()) {
    return make_unexpected_errc<bool>(std::errc::bad_file_descriptor);
  }

  int flags = fcntl(sockFd, F_GETFL, 0);
  if (flags == -1) {
    return tl::make_unexpected(errorFromErrno());
  }

  return !(flags & O_NONBLOCK);
}
