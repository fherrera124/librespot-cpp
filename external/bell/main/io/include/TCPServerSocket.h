#pragma once

#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include "win32shim.h"
#else
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>  // for struct timeval (select()'s timeout)
#include <unistd.h>
#endif

#include <BellLogger.h>

namespace bell {

// Portable listening TCP socket - bind()/listen()/accept(), the server-side
// counterpart TCPSocket.h (client connect()) didn't have. Same platform
// split (win32shim.h for closesocket()/strcasecmp/etc.) and BSD socket
// calls as TCPSocket.h - lwIP (ESP-IDF) and POSIX both speak the same API
// here, only Windows needs the shim.
class TCPServerSocket {
 public:
  TCPServerSocket() {}
  ~TCPServerSocket() { close(); }

  void open(uint16_t port) {
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

    listenFd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd < 0) {
      throw std::runtime_error("TCPServerSocket: socket() failed");
    }

    int reuse = 1;
    setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse,
               sizeof(reuse));

    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(listenFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
      close();
      throw std::runtime_error("TCPServerSocket: bind() failed on port " +
                               std::to_string(port));
    }

    // Backlog of 4 - pairing/local-control traffic is low-frequency and
    // handled one connection at a time (see SimpleHTTPServer's accept
    // loop), no need for a deep queue.
    if (listen(listenFd, 4) < 0) {
      close();
      throw std::runtime_error("TCPServerSocket: listen() failed");
    }
  }

  // Returns -1 on timeout (no connection within timeoutMs) or error -
  // callers use this to periodically re-check their own stop condition
  // instead of blocking in accept() forever with no portable way to
  // interrupt it. Same RECEIVE_POLL_MS-style pattern as
  // DealerSession::runTask().
  int acceptWithTimeout(int timeoutMs) {
    if (listenFd < 0) {
      return -1;
    }

    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(listenFd, &readSet);

    struct timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;

    int ready = select((int)listenFd + 1, &readSet, nullptr, nullptr, &tv);
    if (ready <= 0) {
      return -1;
    }

    return (int)accept(listenFd, nullptr, nullptr);
  }

  void close() {
    if (listenFd >= 0) {
#ifdef _WIN32
      closesocket(listenFd);
#else
      ::close(listenFd);
#endif
      listenFd = -1;
    }
  }

 private:
  int listenFd = -1;
};

}  // namespace bell
