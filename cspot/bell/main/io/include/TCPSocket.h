#ifndef BELL_BASIC_SOCKET_H
#define BELL_BASIC_SOCKET_H

#include <ctype.h>
#include <stdlib.h>
#include <sys/types.h>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include "BellSocket.h"
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
#include <sys/time.h>  // for struct timeval (SO_RCVTIMEO/SO_SNDTIMEO)
#include <unistd.h>
#ifdef __sun
#include <sys/filio.h>
#endif
#endif
#include <BellLogger.h>
#include <fstream>
#include <sstream>

namespace bell {
class TCPSocket : public bell::Socket {
 private:
  // Was uninitialized before open() - getFd() (used by SocketStream::getFd()
  // to shutdown() a raw fd out from under a blocked reader) could return
  // garbage on a never-opened socket instead of a safe sentinel. TLSSocket
  // doesn't have this problem: mbedtls_net_init() in its constructor leaves
  // server_fd in a defined state immediately, not just after open(). See
  // docs/dealer_websocket_migration.md §48.
  int sockFd = -1;
  bool isClosed = true;

 public:
  TCPSocket(){};
  ~TCPSocket() { close(); };

  int getFd() { return sockFd; }

  void open(const std::string& host, uint16_t port) {
    int err;
    int domain = AF_INET;
    int socketType = SOCK_STREAM;

    struct addrinfo hints {
    }, *addr;
    //fine-tune hints according to which socket you want to open
    hints.ai_family = domain;
    hints.ai_socktype = socketType;
    hints.ai_protocol =
        IPPROTO_IP;  // no enum : possible value can be read in /etc/protocols
    hints.ai_flags = AI_CANONNAME | AI_ALL | AI_ADDRCONFIG;

    // BELL_LOG(info, "http", "%s %d", host.c_str(), port);

    char portStr[6];
    sprintf(portStr, "%u", port);
    err = getaddrinfo(host.c_str(), portStr, &hints, &addr);
    if (err != 0) {
      throw std::runtime_error("Resolve failed");
    }

    sockFd = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);

    err = connect(sockFd, addr->ai_addr, addr->ai_addrlen);
    if (err < 0) {
      close();
      // `addr` was leaked here before - the throw below skipped the
      // freeaddrinfo() call further down. See docs/dealer_websocket_
      // migration.md §48.
      freeaddrinfo(addr);
      BELL_LOG(error, "http", "Could not connect to %s. Error %d", host.c_str(),
               errno);
      throw std::runtime_error("Resolve failed");
    }

    int flag = 1;
    setsockopt(sockFd,       /* socket affected */
               IPPROTO_TCP,  /* set option at TCP level */
               TCP_NODELAY,  /* name of option */
               (char*)&flag, /* the cast is historical cruft */
               sizeof(int)); /* length of option value */

    // Unlike TLSSocket (readTimeoutMs/SO_SNDTIMEO, F58/§22), this class had
    // no timeout at all - a dead/stalled peer would block recv()/send()
    // forever. Matches TLSSocket's own hardcoded default (15000ms) for
    // consistency between the two bell::Socket implementations. See
    // docs/dealer_websocket_migration.md §48.
#ifdef _WIN32
    // SO_RCVTIMEO/SO_SNDTIMEO take a DWORD of milliseconds on Windows, not
    // a struct timeval (POSIX) - different ABI, not just a cast.
    DWORD timeoutMs = 15000;
    setsockopt(sockFd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeoutMs,
               sizeof(timeoutMs));
    setsockopt(sockFd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeoutMs,
               sizeof(timeoutMs));
#else
    struct timeval timeout;
    timeout.tv_sec = 15;
    timeout.tv_usec = 0;
    setsockopt(sockFd, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout,
               sizeof(timeout));
    setsockopt(sockFd, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout,
               sizeof(timeout));
#endif

    freeaddrinfo(addr);
    isClosed = false;
  }

  // recv()/send() return ssize_t (can be -1 on error) but this interface
  // returns size_t - clamp instead of letting -1 implicitly become a huge
  // size_t. TLSSocket already does this (documented as F30); this class
  // relied on the negative value's bit pattern surviving the round trip
  // back to a signed ssize_t in SocketStream.cpp, which isn't guaranteed
  // the way an explicit clamp is. See docs/dealer_websocket_migration.md §48.
  size_t read(uint8_t* buf, size_t len) {
    ssize_t ret = recv(sockFd, (char*)buf, len, 0);
    return ret < 0 ? 0 : (size_t)ret;
  }

  size_t write(uint8_t* buf, size_t len) {
    ssize_t ret = send(sockFd, (char*)buf, len, 0);
    return ret < 0 ? 0 : (size_t)ret;
  }

  size_t poll() {
#ifdef _WIN32
    unsigned long value;
    ioctlsocket(sockFd, FIONREAD, &value);
#else
    int value;
    ioctl(sockFd, FIONREAD, &value);
#endif
    return value;
  }
  bool isOpen() {
    return !isClosed;
  }

  void close() {
    if (!isClosed) {
#ifdef _WIN32
      closesocket(sockFd);
#else
      ::close(sockFd);
#endif
      sockFd = -1;
      isClosed = true;
    }
  }
};

}  // namespace bell

#endif
