#pragma once

#include <array>
#include <memory>

// Websocketpp includes
#include <websocketpp/client.hpp>
#include <websocketpp/config/core.hpp>

// Tao JSON
#include <tao/json/value.hpp>

// Bell includes
#include <bell/Logger.h>
#include <bell/Result.h>
#include <bell/net/SocketPollListener.h>
#include <bell/net/TCPSocket.h>
#include <bell/net/TLSSocket.h>

// Own includes
#include "events/EventLoop.h"

namespace cspot {
class DealerClient {
 public:
  // Disconnected: never connected, or a previous connection ended (peer
  // close, read error, missed pong watchdog, handshake failure/timeout).
  // Connecting: connect() was called, TCP/TLS transport is up, WS upgrade
  // handshake in flight - neither confirmed nor failed yet.
  // Connected: WS upgrade completed (onWSOpen fired).
  // Failed: this connect() attempt ended without ever reaching Connected
  // (handshake rejected, dropped, or timed out).
  // Callers (Session::runPoller()) are responsible for reconnecting on
  // anything other than Connected - Disconnected and Failed are handled
  // identically there, the distinction exists mainly for logging/debugging.
  enum class State { Disconnected, Connecting, Connected, Failed };

  DealerClient(std::shared_ptr<cspot::EventLoop> eventLoop);

  bell::Result<> connect(
      const std::string& dealerAddress, const std::string& accessKey,
      const std::shared_ptr<bell::SocketPollListener>& socketPoll);

  bell::Result<> replyToRequest(bool success, const std::string& requestKey);

  // Used for keep alive messages, and to time out a Connecting attempt
  // whose WS handshake never resolves (see connectTimeout in the .cpp).
  void doHousekeeping();

  State state() const { return state_; }
  bool isConnected() const { return state_ == State::Connected; }

 private:
  const char* LOG_TAG = "DealerClient";
  using WSClient = websocketpp::client<websocketpp::config::core>;
  using WSConnection = websocketpp::connection<websocketpp::config::core>;

  std::mutex accessMutex;

  std::shared_ptr<cspot::EventLoop> eventLoop;

  std::array<std::byte, 1024 * 8L> inputBuffer{};

  std::shared_ptr<bell::TLSSocket> socket;
  std::shared_ptr<bell::SocketPollListener> socketPoll;
  bell::SocketPollListener::Registration readableReg_;
  bell::SocketPollListener::Registration writeableReg_;

  // steady_clock, not system_clock: the AP Ping packet handler
  // (ApClient.cpp) calls settimeofday() to sync the system clock, which
  // can jump it forward by hours in one step - a system_clock-based
  // watchdog here would see a huge bogus elapsed time right after that
  // jump and declare the connection dead. steady_clock is immune to
  // wall-clock adjustments.
  std::chrono::steady_clock::time_point lastPingTime;
  std::chrono::steady_clock::time_point lastPongTime;

  State state_ = State::Disconnected;
  // Only meaningful while state_ == Connecting - deadline for the WS
  // handshake to resolve (onWSOpen/onWSFail/onWSClose) before
  // doHousekeeping() gives up on it and moves to Failed.
  std::chrono::steady_clock::time_point connectDeadline{};

  // websocketpp related structs
  WSClient wsClient;
  WSConnection::ptr wsConnection;

  // Handles websocketpp specific events
  void onWSMessage(websocketpp::connection_hdl conn, WSClient::message_ptr msg);
  void onWSOpen(websocketpp::connection_hdl conn);
  void onWSFail(websocketpp::connection_hdl conn);
  void onWSClose(websocketpp::connection_hdl conn);
  std::error_code wsWriteHandler(websocketpp::connection_hdl hdl,
                                 char const* data, size_t size);

  // Common teardown for every "the connection is gone" path (read error,
  // peer EOF, missed pong watchdog) - marks us disconnected and stops the
  // poller from touching this socket again. Does not reconnect; that's
  // Session::runPoller()'s job once isConnected() reports false.
  void handleDisconnect();
};
}  // namespace cspot
