#include "bell/net/TCPSocket.h"

#include "bell/Logger.h"
#include "bell/Result.h"
#include "bell/net/IpAddress.h"

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
#include <sys/poll.h>

using namespace bell::net;

bell::Result<> TCPSocket::connect(const std::string& host, uint16_t port,
                                  int timeoutMs) {
  // Close the socket if it is already open
  if (sockFd != INVALID_FD) {
    BELL_LOG(debug, LOG_TAG, "Socket already open");
    return make_unexpected_errc(std::errc::already_connected);
  }

  auto resolveRes = IpAddress::resolveDomain(host, SOCK_STREAM);

  if (!resolveRes) {
    BELL_LOG(error, LOG_TAG, "Could not resolve {}. Error {}", host.c_str(),
             resolveRes.error());
    return tl::make_unexpected(resolveRes.error());
  }

  resolveRes->setPort(port);

  auto res = createFd(resolveRes->getFamily(), IPPROTO_IP);

  if (!res) {
    BELL_LOG(error, LOG_TAG, "Could not create socket. Error {}", res.error());
    return res;
  }

  auto isBlockingRes = getBlocking();

  if (!isBlockingRes) {
    BELL_LOG(error, LOG_TAG, "Could not get socket flags. Error {}",
             isBlockingRes.error());
    return tl::make_unexpected(isBlockingRes.error());
  }

  // Cache the isBlocking value
  bool tmpIsBlocking = *isBlockingRes;

  // Required for the connect call
  auto blockingRes = setBlocking(false);
  if (!blockingRes) {
    return blockingRes;
  }

  int err = ::connect(sockFd, resolveRes->getSockAddrPtr(),
                      resolveRes->getSockAddrLen());

  if (err < 0 && errno != EINPROGRESS) {
    // Connection failed immediately
    close();

    return tl::make_unexpected(errorFromErrno());
  }

  if (timeoutMs > 0) {
    if (err < 0 && errno == EINPROGRESS) {
      // Connection is in progress; use poll to wait for completion
      struct pollfd pfd {};
      pfd.fd = sockFd;
      pfd.events = POLLOUT;

      int pollResult = ::poll(&pfd, 1, timeoutMs);
      if (pollResult <= 0) {
        // Timeout or error
        close();

        if (pollResult == 0) {
          return make_unexpected_errc(std::errc::timed_out);
        }

        return tl::make_unexpected(errorFromErrno());
      }

      // Check for connection success or error
      auto errCode = lastError();
      if (errCode) {
        return tl::make_unexpected(errCode);
      }

      // Success
      err = 0;
    }

    // Restore isBlocking
    auto setBlockingRes = setBlocking(tmpIsBlocking);
    if (!setBlockingRes) {
      return setBlockingRes;
    }
  }

  // OS-level TCP keepalive - catches a peer that goes silent at the network
  // level (WiFi AP drops the association, a NAT/router mapping expires,
  // etc.) without ever sending a FIN/RST, which otherwise leaves the socket
  // waiting forever with no error and nothing to poll() on. Mirrors
  // librespot-cpp's own hardware-tuned values (PlainConnection.cpp/
  // TLSSocket.cpp there), applied centrally here since every TCP-based
  // connection in this codebase (ApConnection, DealerClient's TLSSocket,
  // the HTTP client's plain/TLS sockets) goes through this same connect().
  int keepalive = 1;
  setsockopt(sockFd, SOL_SOCKET, SO_KEEPALIVE,
             reinterpret_cast<const char*>(&keepalive), sizeof(keepalive));
#ifndef _WIN32
  int keepIdle = 30;
  setsockopt(sockFd, IPPROTO_TCP, TCP_KEEPIDLE, &keepIdle, sizeof(keepIdle));
  int keepInterval = 10;
  setsockopt(sockFd, IPPROTO_TCP, TCP_KEEPINTVL, &keepInterval,
             sizeof(keepInterval));
  int keepCount = 3;
  setsockopt(sockFd, IPPROTO_TCP, TCP_KEEPCNT, &keepCount, sizeof(keepCount));
#endif

  return {};
}

bell::Result<> TCPSocket::listen(int backlog) {
  if (!isValid()) {
    return make_unexpected_errc(std::errc::bad_file_descriptor);
  }

  if (::listen(sockFd, backlog) != 0) {
    return tl::make_unexpected(errorFromErrno());
  }

  return {};
}

bell::Result<TCPSocket> TCPSocket::accept() {
  struct sockaddr_in clientAddr {};
  socklen_t addrLen = sizeof(clientAddr);

  // Accept the incoming connection
  int clientFd = ::accept(
      sockFd, reinterpret_cast<struct sockaddr*>(&clientAddr), &addrLen);

  if (clientFd < 0) {
    return tl::make_unexpected(errorFromErrno());
  }

  return TCPSocket(clientFd);
}
