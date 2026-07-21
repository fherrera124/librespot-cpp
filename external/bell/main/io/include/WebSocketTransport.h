#pragma once

#include <cstddef>  // for size_t
#include <cstdint>  // for uint32_t
#include <memory>   // for unique_ptr
#include <string>   // for string

namespace bell {

// A hand-rolled RFC 6455 WebSocket client, one implementation for both
// ESP32 firmware and the host test suite - see WebSocketTransport.cpp's
// file header for why it's hand-rolled instead of esp_websocket_client or
// bell::SocketStream. Originally built for cspot's Spotify Dealer client
// (docs/dealer_websocket_migration.md §3.3/§5.2/§22/§28/§29 has the full
// history); moved here 2026-07-18 once nothing about it turned out to be
// Dealer-specific - the ping/timeout/message-size tuning that used to be
// hardcoded now come from create()'s caller instead.
//
// NOT thread-safe: single-task design, all reads and writes must happen on
// one caller thread - see WebSocketTransport.cpp's file header for why
// (mbedTLS gives no safety guarantee for concurrent read/write on one
// mbedtls_ssl_context).
//
// Delivers only complete text messages (assumes a JSON/text-only protocol,
// like the Dealer's); fragmented messages are reassembled internally,
// control frames (ping/pong/close) are handled internally, binary messages
// are logged and dropped.
class WebSocketTransport {
 public:
  virtual ~WebSocketTransport() = default;

  /**
  * @brief Connects and completes the WebSocket handshake. Blocking.
  * @param url ws:// or wss:// URL, query string included
  */
  virtual bool connect(const std::string& url) = 0;

  virtual void disconnect() = 0;

  virtual bool isConnected() = 0;

  /**
  * @brief Sends one complete text frame.
  */
  virtual bool sendText(const std::string& payload) = 0;

  /**
  * @brief Sends a WebSocket control ping (keepalive).
  */
  virtual bool sendPing() = 0;

  /**
  * @brief Waits for the next complete text message.
  * @returns false on timeout or disconnection - check isConnected() to
  * tell the two apart
  */
  virtual bool receiveMessage(std::string& message, int timeoutMs) = 0;

  /**
  * @brief Creates the implementation for the current platform.
  * @param pingIntervalMs how long to wait, silent, before proactively
  * sending a WS-protocol ping (dead-peer detection ahead of TCP's own much
  * slower timeout).
  * @param pingTimeoutMs how long to wait for that ping's pong before giving
  * up on the connection.
  * @param maxMessageSize reassembly cap for a (possibly fragmented)
  * incoming message - larger messages are drained off the wire (to keep
  * framing in sync) but dropped, not delivered.
  */
  static std::unique_ptr<WebSocketTransport> create(
      uint32_t pingIntervalMs = 30000, uint32_t pingTimeoutMs = 7000,
      size_t maxMessageSize = 256 * 1024);
};

}  // namespace bell
