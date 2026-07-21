// Fixes a real bug found on real hardware (finding F17,
// docs/spotify_component_analysis.md) - not related to mbedTLS 4.0/ESP-IDF
// v6, this affects upstream cspot on any platform. Everything below is
// unchanged from upstream except connect(), which is marked where it
// differs.
#include "PlainConnection.h"

#ifndef _WIN32
#include <netdb.h>  // for addrinfo, freeaddrinfo, getaddrinfo
#include <netinet/in.h>   // for IPPROTO_IP, IPPROTO_TCP
#include <netinet/tcp.h>  // for TCP_NODELAY
#include <sys/errno.h>    // for EAGAIN, EINTR, ETIMEDOUT, errno
#include <sys/socket.h>   // for setsockopt, connect, recv, send, shutdown
#include <sys/time.h>     // for timeval
#include <cstring>        // for memset
#include <stdexcept>      // for runtime_error
#else
#include <ws2tcpip.h>
#endif
#include "BellLogger.h"  // for AbstractLogger
#include "Logger.h"      // for CSPOT_LOG
#include "Packet.h"      // for cspot
#include "Utils.h"       // for extract, pack

using namespace cspot;

static int getErrno() {
#ifdef _WIN32
  int code = WSAGetLastError();
  if (code == WSAETIMEDOUT)
    return ETIMEDOUT;
  if (code == WSAEINTR)
    return EINTR;
  return code;
#else
  return errno;
#endif
}

PlainConnection::PlainConnection() {
  this->apSock = -1;
};

PlainConnection::~PlainConnection() {
  this->close();
};

void PlainConnection::connect(const std::string& apAddress) {
  struct addrinfo h, *airoot = nullptr, *ai;
  std::string hostname = apAddress.substr(0, apAddress.find(":"));
  std::string portStr =
      apAddress.substr(apAddress.find(":") + 1, apAddress.size());
  memset(&h, 0, sizeof(h));
  h.ai_family = AF_INET;
  h.ai_socktype = SOCK_STREAM;
  h.ai_protocol = IPPROTO_IP;

  // getaddrinfo() doesn't guarantee touching `airoot` on failure - using or
  // freeing it afterwards was UB on a DNS/resolution failure. See F28.
  if (getaddrinfo(hostname.c_str(), portStr.c_str(), &h, &airoot)) {
    CSPOT_LOG(error, "getaddrinfo failed");
    throw std::runtime_error("getaddrinfo failed");
  }

  // FIX vs. upstream: try every resolved address before giving up, instead
  // of throwing on the first candidate that fails to connect - a hostname
  // resolving to multiple addresses (common for Spotify's AP hosts) used to
  // get only one attempt. See F17.
  for (ai = airoot; ai; ai = ai->ai_next) {
    if (ai->ai_family != AF_INET && ai->ai_family != AF_INET6)
      continue;

    this->apSock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (this->apSock < 0)
      continue;

    if (::connect(this->apSock, (struct sockaddr*)ai->ai_addr,
                  ai->ai_addrlen) != -1) {
#ifdef _WIN32
      uint32_t tv = 3000;
#else
      struct timeval tv;
      tv.tv_sec = 3;
      tv.tv_usec = 0;
#endif
      setsockopt(this->apSock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv,
                 sizeof tv);
      setsockopt(this->apSock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv,
                 sizeof tv);

      int flag = 1;
      setsockopt(this->apSock, IPPROTO_TCP, TCP_NODELAY, (char*)&flag,
                 sizeof(int));

      // OS-level TCP keepalive - catches a dead peer independent of the AP
      // protocol's own ping/pongAck watchdog (~125s). Windows skips the
      // IDLE/INTVL/CNT tuning (needs WSAIoctl, not plain setsockopt); plain
      // SO_KEEPALIVE still helps there via the OS's own defaults. See §49.
      int keepalive = 1;
      setsockopt(this->apSock, SOL_SOCKET, SO_KEEPALIVE, (char*)&keepalive,
                 sizeof(keepalive));
#ifndef _WIN32
      int keepIdle = 30;
      setsockopt(this->apSock, IPPROTO_TCP, TCP_KEEPIDLE, &keepIdle,
                 sizeof(keepIdle));
      int keepInterval = 10;
      setsockopt(this->apSock, IPPROTO_TCP, TCP_KEEPINTVL, &keepInterval,
                 sizeof(keepInterval));
      int keepCount = 3;
      setsockopt(this->apSock, IPPROTO_TCP, TCP_KEEPCNT, &keepCount,
                 sizeof(keepCount));
#endif
      break;
    }

    CSPOT_LOG(error, "connect() failed (errno=%d), trying next address",
              getErrno());
#ifdef _WIN32
    closesocket(this->apSock);
#else
    ::close(this->apSock);
#endif
    apSock = -1;
  }

  freeaddrinfo(airoot);

  if (this->apSock < 0) {
    throw std::runtime_error("Can't connect to spotify servers");
  }

  CSPOT_LOG(debug, "Connected to spotify server");
}

std::vector<uint8_t> PlainConnection::recvPacket() {
  std::vector<uint8_t> packetBuffer(4);
  readBlock(packetBuffer.data(), 4);
  uint32_t packetSize = ntohl(extract<uint32_t>(packetBuffer, 0));

  // packetSize is untrusted network input, read before any encryption
  // layer kicks in - below 4 underflows `packetSize - 4` below, too large
  // drives a huge resize(). See F34.
  constexpr uint32_t kMaxPacketSize = 1 * 1024 * 1024;
  if (packetSize < 4 || packetSize > kMaxPacketSize) {
    CSPOT_LOG(error, "recvPacket: implausible packet size %u",
              (unsigned)packetSize);
    throw std::runtime_error("recvPacket: implausible packet size");
  }

  packetBuffer.resize(packetSize, 0);
  readBlock(packetBuffer.data() + 4, packetSize - 4);

  return packetBuffer;
}

std::vector<uint8_t> PlainConnection::sendPrefixPacket(
    const std::vector<uint8_t>& prefix, const std::vector<uint8_t>& data) {
  uint32_t actualSize = prefix.size() + data.size() + sizeof(uint32_t);

  // Packet structure: [PREFIX] + [SIZE] + [DATA]
  auto sizeRaw = pack<uint32_t>(htonl(actualSize));
  sizeRaw.insert(sizeRaw.begin(), prefix.begin(), prefix.end());
  sizeRaw.insert(sizeRaw.end(), data.begin(), data.end());

  writeBlock(sizeRaw);

  return sizeRaw;
}

void PlainConnection::readBlock(const uint8_t* dst, size_t size) {
  unsigned int idx = 0;
  ssize_t n;
  int retries = 0;

  while (idx < size) {
  READ:
    if ((n = recv(this->apSock, (char*)&dst[idx], size - idx, 0)) <= 0) {
      switch (getErrno()) {
        case EAGAIN:
        case ETIMEDOUT:
          if (timeoutHandler()) {
            CSPOT_LOG(error, "Connection lost, will need to reconnect...");
            throw std::runtime_error("Reconnection required");
          }
          goto READ;
        case EINTR:
          goto READ;
        default:
          if (retries++ > 4)
            throw std::runtime_error("Error in read");
          goto READ;
      }
    }
    idx += n;
  }
}

size_t PlainConnection::writeBlock(const std::vector<uint8_t>& data) {
  unsigned int idx = 0;
  ssize_t n;

  int retries = 0;

  while (idx < data.size()) {
  WRITE:
    if ((n = send(this->apSock, (char*)&data[idx],
                  data.size() - idx < 64 ? data.size() - idx : 64, 0)) <= 0) {
      switch (getErrno()) {
        case EAGAIN:
        case ETIMEDOUT:
          if (timeoutHandler()) {
            throw std::runtime_error("Reconnection required");
          }
          goto WRITE;
        case EINTR:
          goto WRITE;
        default:
          if (retries++ > 4)
            throw std::runtime_error("Error in write");
          goto WRITE;
      }
    }
    idx += n;
  }

  return data.size();
}

void PlainConnection::close() {
  if (this->apSock < 0)
    return;

  CSPOT_LOG(info, "Closing socket...");
  // SO_LINGER{1,0} (abortive RST close) was tried and reverted here - see
  // TLSSocket.cpp's close() for the real hardware regression. See §23.
  shutdown(this->apSock, SHUT_RDWR);
#ifdef _WIN32
  closesocket(this->apSock);
#else
  ::close(this->apSock);
#endif
  this->apSock = -1;
}
