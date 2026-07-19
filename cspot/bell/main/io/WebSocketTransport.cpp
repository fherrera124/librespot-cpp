// Single implementation of WebSocketTransport (docs/dealer_websocket_
// migration.md §22/§28/§29, cspot's Spotify Dealer client - the only
// consumer so far): a hand-rolled RFC 6455 client over bell::TLSSocket -
// not esp_websocket_client (vendor close-detection heuristic caused
// unexplained mid-session disconnects), not bell::SocketStream (its
// std::iostream failbit can't distinguish "timed out, try again" from
// "connection is dead" - see TLSSocket::lastReadTimedOut(), needed to
// tolerate a server that goes quiet for 30s+ between pushes). Absorbed the
// old host-only PortableWebSocketTransport.cpp (see WsSocket below) and the
// esp_websocket_client A/B (EspWebSocketClientTransport.cpp, deleted as
// dead code once retired).
//
// NOT thread-safe: single-task design, all reads AND writes must happen on
// the caller's own thread - never call connect(), sendText(), sendPing() or
// receiveMessage() on this object from more than one thread, ever,
// concurrently. mbedTLS doesn't document mbedtls_ssl_context as safe for
// concurrent read+write without external locking, so this avoids the
// question by construction rather than assume it or add an unverified
// mutex. Cost: a per-frame read can block the caller up to
// FRAME_READ_TIMEOUT_MS - fine for a caller polling in its own loop. A
// two-task design (independent ping task + mutex) was tried and reverted
// for cspot's Dealer client - see §33/§35.
//
// Platform split, ws:// vs wss://: on ESP_PLATFORM, only wss:// is
// supported - untested plain-text code just isn't worth shipping. On host,
// ws:// is also accepted (WsSocket's PlainSocket branch below) purely so
// ws_transport_echo_test.cpp can validate the framing against a plain-text
// echo server without standing up TLS for a local test.
#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>
#include <vector>

#include "BellLogger.h"  // for AbstractLogger, BELL_LOG
#include "Crypto.h"
#include "TLSSocket.h"
#include "WebSocketTransport.h"

#ifndef ESP_PLATFORM
#include <arpa/inet.h>
#include <cerrno>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace bell {

namespace {
// Handshake-phase reads (HTTP upgrade response) - a slow/broken TLS layer
// here should fail fast, matching the old F58 HTTP timeout in spirit.
constexpr uint32_t CONNECT_READ_TIMEOUT_MS = 10000;

// Once a frame has started, budget for each read step needed to finish it.
// Generous on purpose - abandoning mid-frame would desync framing anyway,
// this just needs to comfortably fit any working link, up to create()'s
// maxMessageSize.
constexpr uint32_t FRAME_READ_TIMEOUT_MS = 10000;

const char* WS_ACCEPT_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

struct ParsedWsUrl {
  bool valid = false;
  bool isSSL = false;
  std::string host;
  int port = 0;
  std::string pathAndQuery;
};

ParsedWsUrl parseWsUrl(const std::string& url) {
  ParsedWsUrl parsed;
  std::string rest;
  if (url.rfind("wss://", 0) == 0) {
    parsed.isSSL = true;
    rest = url.substr(6);
  } else if (url.rfind("ws://", 0) == 0) {
    rest = url.substr(5);
  } else {
    return parsed;
  }

  auto pathPos = rest.find('/');
  std::string hostPort =
      pathPos == std::string::npos ? rest : rest.substr(0, pathPos);
  parsed.pathAndQuery =
      pathPos == std::string::npos ? "/" : rest.substr(pathPos);

  auto colonPos = hostPort.find(':');
  if (colonPos == std::string::npos) {
    parsed.host = hostPort;
    parsed.port = parsed.isSSL ? 443 : 80;
  } else {
    parsed.host = hostPort.substr(0, colonPos);
    parsed.port = std::stoi(hostPort.substr(colonPos + 1));
  }

  parsed.valid = !parsed.host.empty();
  return parsed;
}

#ifndef ESP_PLATFORM
// Host-only, bare/unencrypted TCP socket - only for ws_transport_echo_test's
// plain-text echo server (never compiled on-device). Mirrors bell::TLSSocket's
// read/write/timeout surface so WsSocket below can dispatch to either.
class PlainSocket {
 public:
  ~PlainSocket() { close(); }

  bool open(const std::string& host, uint16_t port) {
    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints,
                    &res) != 0) {
      return false;
    }
    fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
      freeaddrinfo(res);
      return false;
    }
    bool connected = ::connect(fd, res->ai_addr, res->ai_addrlen) == 0;
    freeaddrinfo(res);
    if (!connected) {
      ::close(fd);
      fd = -1;
      return false;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    applyReadTimeout();
    return true;
  }

  void setReadTimeout(uint32_t ms) {
    readTimeoutMs = ms;
    if (fd >= 0) {
      applyReadTimeout();
    }
  }

  size_t read(uint8_t* buf, size_t len) {
    timedOut = false;
    if (fd < 0) {
      return 0;
    }
    ssize_t n = ::recv(fd, buf, len, 0);
    if (n > 0) {
      return (size_t)n;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      timedOut = true;  // SO_RCVTIMEO expired - routine, not fatal
    }
    return 0;
  }

  size_t write(uint8_t* buf, size_t len) {
    if (fd < 0) {
      return 0;
    }
    ssize_t n = ::send(fd, buf, len, 0);
    return n > 0 ? (size_t)n : 0;
  }

  bool lastReadTimedOut() const { return timedOut; }
  bool isOpen() const { return fd >= 0; }

  void close() {
    if (fd >= 0) {
      ::close(fd);
      fd = -1;
    }
  }

 private:
  void applyReadTimeout() {
    struct timeval tv;
    tv.tv_sec = readTimeoutMs / 1000;
    tv.tv_usec = (readTimeoutMs % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  }

  int fd = -1;
  uint32_t readTimeoutMs = 15000;
  bool timedOut = false;
};
#endif  // !ESP_PLATFORM

// Thin dispatcher over bell::TLSSocket (always, on-device) and, host-only,
// PlainSocket (for ws://) - see both classes' own comments above. On
// ESP_PLATFORM this collapses to a plain bell::TLSSocket member with no
// extra indirection: the #ifndef branches below are compiled out entirely,
// not just false at runtime.
class WsSocket {
 public:
  bool open(const std::string& host, uint16_t port, bool ssl) {
#ifndef ESP_PLATFORM
    isPlain = !ssl;
    if (isPlain) {
      return plainSocket.open(host, port);
    }
#endif
    (void)ssl;
    try {
      tlsSocket.open(host, port);
    } catch (const std::exception& e) {
      BELL_LOG(error, "websocket", "TLS connect to %s failed: %s",
               host.c_str(), e.what());
      return false;
    }
    return tlsSocket.isOpen();
  }

  void setReadTimeout(uint32_t ms) {
#ifndef ESP_PLATFORM
    if (isPlain) {
      plainSocket.setReadTimeout(ms);
      return;
    }
#endif
    tlsSocket.setReadTimeout(ms);
  }

  size_t read(uint8_t* buf, size_t len) {
#ifndef ESP_PLATFORM
    if (isPlain) {
      return plainSocket.read(buf, len);
    }
#endif
    return tlsSocket.read(buf, len);
  }

  size_t write(uint8_t* buf, size_t len) {
#ifndef ESP_PLATFORM
    if (isPlain) {
      return plainSocket.write(buf, len);
    }
#endif
    return tlsSocket.write(buf, len);
  }

  bool lastReadTimedOut() const {
#ifndef ESP_PLATFORM
    if (isPlain) {
      return plainSocket.lastReadTimedOut();
    }
#endif
    return tlsSocket.lastReadTimedOut();
  }

  void close() {
#ifndef ESP_PLATFORM
    plainSocket.close();
#endif
    tlsSocket.close();  // idempotent - safe even if never opened
  }

 private:
  bell::TLSSocket tlsSocket;
#ifndef ESP_PLATFORM
  PlainSocket plainSocket;
  bool isPlain = false;
#endif
};
}  // namespace

class WebSocketTransportImpl : public WebSocketTransport {
 public:
  WebSocketTransportImpl(uint32_t pingIntervalMs, uint32_t pingTimeoutMs,
                         size_t maxMessageSize)
      : pingIntervalMs(pingIntervalMs), pingTimeoutMs(pingTimeoutMs),
        maxMessageSize(maxMessageSize) {}
  ~WebSocketTransportImpl() override { disconnect(); }

  bool connect(const std::string& url) override {
    auto parsed = parseWsUrl(url);
#ifdef ESP_PLATFORM
    // ws:// support is deliberately not implemented on-device - avoids
    // shipping untested, never-exercised code.
    if (!parsed.valid || !parsed.isSSL) {
      BELL_LOG(error, "websocket",
               "invalid or non-TLS WebSocket URL (only wss:// is supported "
               "on-device)");
      return false;
    }
#else
    // Host build additionally accepts plain ws:// (see WsSocket above) -
    // needed for ws_transport_echo_test.cpp's plain-text echo server.
    if (!parsed.valid) {
      BELL_LOG(error, "websocket", "invalid WebSocket URL");
      return false;
    }
#endif

    socket.setReadTimeout(CONNECT_READ_TIMEOUT_MS);
    if (!socket.open(parsed.host, parsed.port, parsed.isSSL)) {
      return false;
    }

    if (!handshake(parsed)) {
      socket.close();
      return false;
    }

    connected = true;
    idleMs = 0;
    pingOutstanding = false;
    reassembly.clear();
    expectingContinuation = false;
    droppingOversized = false;
    oversizedReported = false;
    return true;
  }

  void disconnect() override {
    if (connected.exchange(false)) {
      uint8_t closePayload[2] = {0x03, 0xE8};
      sendFrame(0x8, closePayload, sizeof(closePayload));
    }
    socket.close();  // idempotent - safe even if fail() already closed it
  }

  bool isConnected() override { return connected; }

  bool sendText(const std::string& payload) override {
    if (!connected) {
      return false;
    }
    // A failed write must fail() immediately - the caller only logs a
    // false return, so a silently-stale `connected` would linger until
    // some unrelated read eventually caught the problem.
    bool ok = sendFrame(0x1, (const uint8_t*)payload.data(), payload.size());
    if (!ok) {
      fail("failed to send text message");
    }
    return ok;
  }

  bool sendPing() override {
    if (!connected) {
      return false;
    }
    return sendFrame(0x9, nullptr, 0);
  }

  // Reads and parses exactly one complete text message, or returns false
  // if none arrived within timeoutMs (routine idle - check isConnected()
  // to tell that apart from a failure that ended the connection).
  bool receiveMessage(std::string& message, int timeoutMs) override {
    if (!connected) {
      return false;
    }

    // Wait for the start of the next frame within the caller's own short
    // poll window - this is what makes routine idle time (a server that
    // goes quiet for a while is normal, not a problem) show up as a cheap
    // "nothing yet" instead of a long blocking call on every poll tick.
    socket.setReadTimeout((uint32_t)std::max(timeoutMs, 1));
    uint8_t firstByte;
    if (!readExact(&firstByte, 1)) {
      if (socket.lastReadTimedOut()) {
        onIdlePoll(timeoutMs);
        return false;
      }
      fail("read error waiting for next frame");
      return false;
    }
    onTrafficSeen();

    // A frame has started - commit to finishing it (see the file header
    // comment for why this uses a longer, fixed budget instead of the
    // caller's short one).
    socket.setReadTimeout(FRAME_READ_TIMEOUT_MS);

    while (true) {
      uint8_t secondByte;
      if (!readExact(&secondByte, 1)) {
        fail(socket.lastReadTimedOut() ? "frame receive stalled"
                                       : "read error mid-frame");
        return false;
      }

      bool fin = firstByte & 0x80;
      uint8_t opcode = firstByte & 0x0F;
      bool masked = secondByte & 0x80;
      uint64_t length = secondByte & 0x7F;

      if (masked) {
        fail("server sent a masked WebSocket frame (RFC 6455 violation)");
        return false;
      }

      if (length == 126) {
        uint8_t ext[2];
        if (!readExact(ext, 2)) {
          fail("read error on extended frame length");
          return false;
        }
        length = ((uint64_t)ext[0] << 8) | ext[1];
      } else if (length == 127) {
        uint8_t ext[8];
        if (!readExact(ext, 8)) {
          fail("read error on extended frame length");
          return false;
        }
        length = 0;
        for (int i = 0; i < 8; i++) {
          length = (length << 8) | ext[i];
        }
      }

      bool isDataFrame = (opcode == 0x0 || opcode == 0x1 || opcode == 0x2);

      // Fragmentation state machine validation
      if (opcode == 0x0 && !expectingContinuation) {
        fail("received continuation frame without prior data frame");
        return false;
      }
      if ((opcode == 0x1 || opcode == 0x2) && expectingContinuation) {
        fail("received new data frame while expecting continuation");
        return false;
      }

      if (!isDataFrame && length > 125) {
        // Control frames strictly limited to <= 125 bytes by RFC 6455
        fail("oversized control frame");
        return false;
      }

      bool oversized = false;
      if (isDataFrame) {
        if (droppingOversized) {
          oversized = true;
        } else if (reassembly.size() + length > maxMessageSize) {
          oversized = true;
          droppingOversized = true;
        }
      }

      std::vector<uint8_t> payload;
      if (length > 0) {
        payload.resize(oversized ? 0 : length);
        if (oversized) {
          // Still have to drain exactly `length` bytes off the wire to
          // keep framing in sync for whatever comes next, even though
          // we're not keeping any of it.
          std::vector<uint8_t> scratch(std::min<uint64_t>(length, 4096));
          uint64_t remaining = length;
          while (remaining > 0) {
            size_t chunk = (size_t)std::min<uint64_t>(remaining, scratch.size());
            if (!readExact(scratch.data(), chunk)) {
              fail(socket.lastReadTimedOut() ? "frame receive stalled"
                                             : "read error on payload");
              return false;
            }
            remaining -= chunk;
          }
        } else if (!readExact(payload.data(), length)) {
          fail(socket.lastReadTimedOut() ? "frame receive stalled"
                                         : "read error on payload");
          return false;
        }
      }
      onTrafficSeen();

      switch (opcode) {
        case 0x9:  // ping -> pong, echoing the payload
          // Same reasoning as sendText(): a failed echo here (previously
          // ignored) means the connection is broken, not "connection is
          // fine" as the comment below would otherwise imply.
          if (!sendFrame(0xA, payload.data(), payload.size())) {
            fail("failed to send pong reply");
          }
          return false;  // nothing to deliver this call, connection is fine
        case 0xA:  // pong
          pingOutstanding = false;
          return false;
        case 0x8: {  // close -> echo the status code back, then stop
          // Close code/reason are server-controlled diagnostics, not
          // protocol requirements - logged to tell a deliberate server-side
          // rotation (e.g. code 1000/1001) apart from an actual error on
          // our end, since "peer sent WebSocket close" alone looks the same
          // either way.
          uint16_t closeCode =
              payload.size() >= 2
                  ? (uint16_t)((payload[0] << 8) | payload[1])
                  : 0;
          std::string closeReason =
              payload.size() > 2
                  ? std::string((char*)payload.data() + 2, payload.size() - 2)
                  : "";
          BELL_LOG(info, "websocket", "close code=%d reason='%s'",
                   (int)closeCode, closeReason.c_str());
          sendFrame(0x8, payload.data(),
                    payload.size() >= 2 ? 2 : payload.size());
          fail("peer sent WebSocket close");
          return false;
        }
        case 0x2:  // binary - this transport assumes a text-only protocol
                   // (see the class comment). Fragmented binary isn't
                   // handled (a stray continuation frame after it would
                   // wrongly append to `reassembly`) - never seen in
                   // practice, not worth the extra state.
          fail("unexpected binary WebSocket message");
          return false;
        case 0x1:  // text (start of a new message)
        case 0x0:  // continuation
          if (!oversized) {
            reassembly.append((const char*)payload.data(), payload.size());
          } else if (!oversizedReported) {
            BELL_LOG(error, "websocket", "message exceeded %d bytes, dropping",
                     (int)maxMessageSize);
            reassembly.clear();
            oversizedReported = true;
          }

          if (fin) {
            bool delivered = !oversized;
            if (delivered) {
              message = std::move(reassembly);
            }
            reassembly.clear();
            expectingContinuation = false;
            droppingOversized = false;
            oversizedReported = false;
            return delivered;
          }

          expectingContinuation = true;

          // Not done yet - read the next frame of this same message,
          // still under the committed FRAME_READ_TIMEOUT_MS budget.
          if (!readExact(&firstByte, 1)) {
            fail(socket.lastReadTimedOut()
                     ? "frame receive stalled"
                     : "read error waiting for continuation frame");
            return false;
          }
          continue;
        default:
          fail("unknown WebSocket opcode");
          return false;
      }
    }
  }

 private:
  bool handshake(const ParsedWsUrl& parsed) {
    Crypto crypto;
    auto keyBytes = crypto.generateVectorWithRandomData(16);
    auto key = Crypto::base64Encode(keyBytes);

    // User-Agent: every other outbound request already sends one - this
    // handshake was the exception. go-librespot always sets one too. See
    // §27 for the A/B that prompted adding it here.
    std::string request = "GET " + parsed.pathAndQuery + " HTTP/1.1\r\n" +
                          "Host: " + parsed.host + ":" +
                          std::to_string(parsed.port) + "\r\n" +
                          "Upgrade: websocket\r\n" +
                          "Connection: Upgrade\r\n" +
                          "Sec-WebSocket-Key: " + key + "\r\n" +
                          "Sec-WebSocket-Version: 13\r\n" +
                          "User-Agent: cspot/1.0\r\n\r\n";
    if (!writeAll((const uint8_t*)request.data(), request.size())) {
      BELL_LOG(error, "websocket", "handshake request failed to send");
      return false;
    }

    std::string statusLine;
    if (!readHeaderLine(statusLine) ||
        statusLine.find(" 101 ") == std::string::npos) {
      BELL_LOG(error, "websocket", "handshake rejected: %s",
               statusLine.c_str());
      return false;
    }

    std::string acceptValue;
    bool extensionsSelected = false;
    std::string line;
    while (readHeaderLine(line) && !line.empty()) {
      auto colon = line.find(':');
      if (colon == std::string::npos) {
        continue;
      }
      std::string name = line.substr(0, colon);
      std::transform(name.begin(), name.end(), name.begin(), ::tolower);
      std::string value = line.substr(colon + 1);
      value.erase(0, value.find_first_not_of(" \t"));

      if (name == "sec-websocket-accept") {
        acceptValue = value;
      } else if (name == "sec-websocket-extensions" && !value.empty()) {
        extensionsSelected = true;
      }
    }

    // We never offer extensions; a server selecting one anyway would send
    // frames we can't interpret (e.g. permessage-deflate) - fail loudly.
    if (extensionsSelected) {
      BELL_LOG(error, "websocket", "server selected an unrequested extension");
      return false;
    }

    crypto.sha1Init();
    crypto.sha1Update(key + WS_ACCEPT_GUID);
    auto expected = Crypto::base64Encode(crypto.sha1FinalBytes());
    if (acceptValue != expected) {
      BELL_LOG(error, "websocket", "Sec-WebSocket-Accept mismatch");
      return false;
    }

    return true;
  }

  bool readHeaderLine(std::string& line) {
    line.clear();
    char c;
    while (true) {
      if (!readExact((uint8_t*)&c, 1)) {
        return false;
      }
      if (c == '\n') {
        break;
      }
      if (c != '\r') {
        line.push_back(c);
      }
    }
    return true;
  }

  // Reads until exactly `len` bytes are in `buf`. False means: check
  // socket.lastReadTimedOut() - a routine "nothing arrived in time" vs a
  // real I/O failure.
  bool readExact(uint8_t* buf, size_t len) {
    size_t got = 0;
    while (got < len) {
      size_t n = socket.read(buf + got, len - got);
      if (n == 0) {
        return false;
      }
      got += n;
    }
    return true;
  }

  bool writeAll(const uint8_t* buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
      size_t n = socket.write(const_cast<uint8_t*>(buf + sent), len - sent);
      if (n == 0) {
        return false;
      }
      sent += n;
    }
    return true;
  }

  // Client frames MUST be masked (RFC 6455 §5.3).
  bool sendFrame(uint8_t opcode, const uint8_t* payload, size_t length) {
    uint8_t header[14];
    size_t headerLength = 2;
    header[0] = 0x80 | opcode;  // FIN, no fragmentation on send

    if (length <= 125) {
      header[1] = 0x80 | (uint8_t)length;
    } else if (length <= 0xFFFF) {
      header[1] = 0x80 | 126;
      header[2] = (length >> 8) & 0xFF;
      header[3] = length & 0xFF;
      headerLength = 4;
    } else {
      header[1] = 0x80 | 127;
      for (int i = 0; i < 8; i++) {
        // FIX: Cast to uint64_t to prevent undefined behavior on ESP32
        // 32-bit architecture when shifting > 31 bits.
        header[2 + i] = ((uint64_t)length >> (8 * (7 - i))) & 0xFF;
      }
      headerLength = 10;
    }

    Crypto crypto;
    auto mask = crypto.generateVectorWithRandomData(4);
    memcpy(header + headerLength, mask.data(), 4);
    headerLength += 4;

    std::vector<uint8_t> masked(length);
    for (size_t i = 0; i < length; i++) {
      masked[i] = payload[i] ^ mask[i % 4];
    }

    if (!writeAll(header, headerLength)) {
      return false;
    }
    if (length > 0 && !writeAll(masked.data(), masked.size())) {
      return false;
    }
    return true;
  }

  void onTrafficSeen() {
    idleMs = 0;
    pingOutstanding = false;
  }

  // Called on every poll tick that sees nothing - proactive-ping/
  // give-up-on-silence state machine.
  void onIdlePoll(int timeoutMs) {
    idleMs += (uint32_t)timeoutMs;
    if (!pingOutstanding) {
      if (idleMs >= pingIntervalMs) {
        if (!sendFrame(0x9, nullptr, 0)) {
          fail("failed to send keepalive ping");
          return;
        }
        BELL_LOG(debug, "websocket", "sent keepalive ping (idle %ums)",
                 idleMs);
        pingOutstanding = true;
        pingSentAtIdleMs = idleMs;
      }
    } else if (idleMs - pingSentAtIdleMs >= pingTimeoutMs) {
      fail("no response to keepalive ping - connection presumed dead");
    }
  }

  void fail(const char* reason) {
    if (connected.exchange(false)) {
      BELL_LOG(error, "websocket", "%s, disconnecting", reason);
      socket.close();
    }
  }

  const uint32_t pingIntervalMs;
  const uint32_t pingTimeoutMs;
  const size_t maxMessageSize;

  WsSocket socket;
  std::atomic<bool> connected{false};

  std::string reassembly;
  bool expectingContinuation = false;
  bool droppingOversized = false;
  bool oversizedReported = false;

  uint32_t idleMs = 0;
  bool pingOutstanding = false;
  uint32_t pingSentAtIdleMs = 0;
};

std::unique_ptr<WebSocketTransport> WebSocketTransport::create(
    uint32_t pingIntervalMs, uint32_t pingTimeoutMs, size_t maxMessageSize) {
  return std::make_unique<WebSocketTransportImpl>(pingIntervalMs, pingTimeoutMs,
                                                   maxMessageSize);
}

}  // namespace bell
